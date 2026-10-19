// HLSL to SPIR-V for the Vulkan backend. Two compilers, in this order:
//
// 1. DXC (libdxcompiler.so), loaded with dlopen so a machine without it still runs: the same
//    compiler family the Windows build's HLSL comes from, invoked through IDxcCompiler3 with
//    -spirv. CMake defines XBE_HAVE_DXC and adds /opt/dxc/include when DXC is installed.
// 2. glslang's HLSL front end (libglslang, linked): TShader with the HLSL input mode and the
//    HLSL functionality target, shifted so its bindings land where DXC's do.
//
// Both are asked for the one descriptor layout vulkan.cpp binds: cbuffers b0/b1 at bindings
// 0/1, textures t0..t3 at 2..5, samplers s0..s3 at 6..9, descriptor set 0. DXC gets it from
// -fvk-b/t/s-shift; glslang from setShiftBinding with HLSL IO mapping on.
#if !defined(_WIN32)

#include "gpu/spirv.h"

#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include "core/log.h"
#include "gpu/shaders.h"  // Hash

#if defined(XBE_HAVE_DXC) && __has_include(<dxc/dxcapi.h>)
#define XBE_SPIRV_DXC 1
#include "dxc/WinAdapter.h"
#include "dxc/dxcapi.h"
#endif

#if __has_include(<glslang/Public/ShaderLang.h>)
#define XBE_SPIRV_GLSLANG 1
#ifndef ENABLE_HLSL
#define ENABLE_HLSL 1  // the distro libglslang is built with its HLSL front end; the headers gate its API on this
#endif
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#endif

namespace gpu {
namespace {

std::mutex g_lock;
std::unordered_map<uint64_t, std::vector<uint32_t>> g_cache;
std::filesystem::path g_dir;
std::string g_compiler = "none";

std::filesystem::path CacheFile(uint64_t key, bool fragment) {
    char name[48];
    std::snprintf(name, sizeof(name), "%016llx.%s.spv", (unsigned long long)key, fragment ? "ps" : "vs");
    return g_dir / name;
}

bool ReadWords(const std::filesystem::path& path, std::vector<uint32_t>& words) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize bytes = f.tellg();
    if (bytes <= 0 || bytes % 4) return false;
    words.resize(size_t(bytes) / 4);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(words.data()), bytes);
    // SPIR-V magic, little endian as both compilers emit it on this host
    return f.good() && words[0] == 0x07230203u;
}

void WriteWords(const std::filesystem::path& path, const std::vector<uint32_t>& words) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) f.write(reinterpret_cast<const char*>(words.data()), std::streamsize(words.size() * 4));
}

#ifdef XBE_SPIRV_DXC
using DxcCreateInstanceFn = HRESULT (*)(REFCLSID rclsid, REFIID riid, LPVOID* ppv);

// libdxcompiler's handle and entry point, taken once; null when the library is not there
DxcCreateInstanceFn DxcEntry() {
    static DxcCreateInstanceFn fn = [] {
        void* lib = dlopen("libdxcompiler.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib) lib = dlopen("/opt/dxc/lib/libdxcompiler.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib) lib = dlopen("/usr/local/lib/libdxcompiler.so", RTLD_NOW | RTLD_LOCAL);
        return lib ? reinterpret_cast<DxcCreateInstanceFn>(dlsym(lib, "DxcCreateInstance")) : nullptr;
    }();
    return fn;
}

std::wstring Wide(const char* s) {
    std::wstring w;
    while (*s) w += wchar_t(uint8_t(*s++));
    return w;
}

bool CompileDxc(const std::string& hlsl, const char* entry, bool fragment, std::vector<uint32_t>& words,
                std::string* error) {
    DxcCreateInstanceFn create = DxcEntry();
    if (!create) return false;
    IDxcUtils* utils = nullptr;
    IDxcCompiler3* compiler = nullptr;
    if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils))) ||
        FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
        if (utils) utils->Release();
        if (compiler) compiler->Release();
        if (error) *error = "dxc: DxcCreateInstance failed";
        return false;
    }
    std::wstring wentry = Wide(entry);
    std::wstring wprofile = fragment ? L"ps_6_0" : L"vs_6_0";
    // the binding shifts of the header comment: b in place, t past the two cbuffers, s past them and the textures
    const wchar_t* args[] = {L"-E", wentry.c_str(), L"-T", wprofile.c_str(), L"-spirv",
                             L"-fspv-target-env=vulkan1.2", L"-HV", L"2018", L"-O1",
                             L"-fvk-b-shift", L"0", L"0", L"-fvk-t-shift", L"2", L"0",
                             L"-fvk-s-shift", L"6", L"0", L"-fvk-u-shift", L"10", L"0"};
    DxcBuffer source{hlsl.data(), hlsl.size(), DXC_CP_UTF8};
    IDxcResult* result = nullptr;
    HRESULT hr = compiler->Compile(&source, args, uint32_t(std::size(args)), nullptr, IID_PPV_ARGS(&result));
    bool ok = false;
    if (SUCCEEDED(hr) && result) {
        HRESULT status = E_FAIL;
        result->GetStatus(&status);
        IDxcBlobUtf8* messages = nullptr;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&messages), nullptr);
        std::string text = messages && messages->GetStringLength() ? messages->GetStringPointer() : "";
        if (messages) messages->Release();
        if (SUCCEEDED(status)) {
            IDxcBlob* blob = nullptr;
            if (SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&blob), nullptr)) && blob &&
                blob->GetBufferSize() % 4 == 0) {
                words.assign(static_cast<const uint32_t*>(blob->GetBufferPointer()),
                             static_cast<const uint32_t*>(blob->GetBufferPointer()) + blob->GetBufferSize() / 4);
                ok = !words.empty() && words[0] == 0x07230203u;
            }
            if (blob) blob->Release();
        }
        if (!ok && error) *error = "dxc: " + text;
    } else if (error) {
        *error = "dxc: Compile call failed";
    }
    if (result) result->Release();
    compiler->Release();
    utils->Release();
    if (ok) g_compiler = "dxc";
    return ok;
}
#endif  // XBE_SPIRV_DXC

#ifdef XBE_SPIRV_GLSLANG
bool CompileGlslang(const std::string& hlsl, const char* entry, bool fragment, std::vector<uint32_t>& words,
                    std::string* error) {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
    EShLanguage stage = fragment ? EShLangFragment : EShLangVertex;
    glslang::TShader shader(stage);
    const char* text = hlsl.c_str();
    shader.setStrings(&text, 1);
    shader.setEntryPoint(entry);
    shader.setSourceEntryPoint(entry);
    shader.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 120);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
    shader.setEnvTargetHlslFunctionality1();
    shader.setHlslIoMapping(true);
    // HLSL register numbers become bindings, shifted into the layout of the header comment
    shader.setShiftBinding(glslang::EResUbo, 0);
    shader.setShiftBinding(glslang::EResTexture, 2);
    shader.setShiftBinding(glslang::EResSampler, 6);
    shader.setShiftBinding(glslang::EResImage, 10);
    shader.setAutoMapBindings(false);
    shader.setAutoMapLocations(true);
    if (!shader.parse(GetDefaultResources(), 120, false, EShMsgDefault)) {
        if (error) *error = std::string("glslang: ") + shader.getInfoLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) {
        if (error) *error = std::string("glslang: ") + program.getInfoLog();
        return false;
    }
    glslang::SpvOptions options;
    options.generateDebugInfo = false;
    options.optimizeSize = false;
    options.disassemble = false;
    options.validate = true;
    glslang::GlslangToSpv(*program.getIntermediate(stage), words, nullptr, &options);
    if (words.empty() || words[0] != 0x07230203u) {
        if (error) *error = "glslang: no SPIR-V produced";
        return false;
    }
    g_compiler = "glslang";
    return true;
}
#endif  // XBE_SPIRV_GLSLANG

}  // namespace

void SetSpirvCacheDir(std::filesystem::path dir, std::string device_key) {
    std::lock_guard lock(g_lock);
    // a folder name, nothing else: keep the characters a path is happy with
    for (char& c : device_key)
        if (!(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '.' || c == '-' || c == '_'))
            c = '_';
    g_dir = dir / device_key;
}

bool CompileHlslToSpirv(const std::string& hlsl, const char* entry, bool fragment, std::vector<uint32_t>& words,
                        std::string* error) {
    uint64_t key = Hash(entry, std::strlen(entry), fragment ? 0x7073ull : 0x7673ull);
    key = Hash(hlsl.data(), hlsl.size(), key);
    {
        std::lock_guard lock(g_lock);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) {
            words = it->second;
            return !words.empty();
        }
    }
    std::filesystem::path file;
    {
        std::lock_guard lock(g_lock);
        if (!g_dir.empty()) file = CacheFile(key, fragment);
    }
    if (!file.empty() && ReadWords(file, words)) {
        std::lock_guard lock(g_lock);
        g_cache[key] = words;
        return true;
    }
    std::string dxc_error, glslang_error;
    bool ok = false;
#ifdef XBE_SPIRV_DXC
    ok = CompileDxc(hlsl, entry, fragment, words, &dxc_error);
#endif
#ifdef XBE_SPIRV_GLSLANG
    if (!ok) ok = CompileGlslang(hlsl, entry, fragment, words, &glslang_error);
#endif
    if (!ok) {
        if (error) *error = (dxc_error.empty() ? "" : dxc_error + "\n") + glslang_error;
        // cached as a failure, like d3d12.cpp's Compile: a bad shader is not recompiled every draw
        std::lock_guard lock(g_lock);
        g_cache[key] = {};
        return false;
    }
    {
        std::lock_guard lock(g_lock);
        g_cache[key] = words;
    }
    if (!file.empty()) WriteWords(file, words);
    return true;
}

const char* SpirvCompilerName() {
    std::lock_guard lock(g_lock);
    return g_compiler.c_str();
}

}  // namespace gpu

#endif  // !defined(_WIN32)

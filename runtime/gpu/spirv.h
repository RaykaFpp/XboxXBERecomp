#pragma once
// HLSL (as gpu/shaders.cpp emits it) to SPIR-V, at run time. Split out of vulkan.cpp so the
// shader-compiler choice lives in one place: DXC (libdxcompiler) first, glslang's HLSL front
// end as the fallback. See spirv.cpp for which one a build got and why.
#if !defined(_WIN32)

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gpu {

// Compiles one HLSL entry point to SPIR-V words. fragment selects ps vs vs. Bindings come out
// in the one layout the Vulkan backend binds: b0/b1 at bindings 0/1, t0..t3 at 2..5, s0..s3
// at 6..9, all in descriptor set 0. Results are cached in memory and, once SetSpirvCacheDir
// has run, as .spv files keyed by a hash of source, entry point and stage. On failure returns
// false and, when error is given, leaves the compiler's message there.
bool CompileHlslToSpirv(const std::string& hlsl, const char* entry, bool fragment,
                        std::vector<uint32_t>& words, std::string* error = nullptr);

// Where compiled SPIR-V (and, by the backend, its pipeline cache) is kept between runs: the
// shadercache folder beside the exe. device_key names the adapter and driver the cache belongs
// to, so a cache never crosses adapters; it becomes a folder name under dir.
void SetSpirvCacheDir(std::filesystem::path dir, std::string device_key);

// "dxc", "glslang", or "none": the compiler the last CompileHlslToSpirv call used (the one the
// first call picked, after that). For the backend's init log line.
const char* SpirvCompilerName();

}  // namespace gpu

#endif  // !defined(_WIN32)

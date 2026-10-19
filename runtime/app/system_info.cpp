// the log's opening lines: what the game runs on and with which settings
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#else
#include <cpuid.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#endif

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"

#if defined(_WIN32)
#pragma comment(lib, "advapi32.lib")
#endif

namespace app {

namespace {

#if defined(_WIN32)

std::string RegString(HKEY key, const char* name) {
    char text[256] = {};
    DWORD size = sizeof(text) - 1;
    if (RegQueryValueExA(key, name, nullptr, nullptr, reinterpret_cast<LPBYTE>(text), &size) != ERROR_SUCCESS) return {};
    return text;
}

std::string OsVersion() {
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ | KEY_WOW64_64KEY,
                      &key) != ERROR_SUCCESS)
        return "unknown";
    std::string name = RegString(key, "ProductName"), display = RegString(key, "DisplayVersion"),
                build = RegString(key, "CurrentBuildNumber");
    DWORD ubr = 0, size = sizeof(ubr);
    RegQueryValueExA(key, "UBR", nullptr, nullptr, reinterpret_cast<LPBYTE>(&ubr), &size);
    RegCloseKey(key);
    // Windows 11 still names itself Windows 10 here, its build number tells
    if (auto ten = name.find("Windows 10"); ten != std::string::npos && std::atoi(build.c_str()) >= 22000)
        name.replace(ten, 10, "Windows 11");
    if (!display.empty()) name += " " + display;
    if (!build.empty()) name += " (build " + build + "." + std::to_string(ubr) + ")";
    using WineVersion = const char*(__cdecl*)();
    if (auto wine = reinterpret_cast<WineVersion>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version"))))
        name += std::string(" on Wine ") + wine();
    return name;
}

std::string CpuName() {
    int r[4] = {};
    __cpuid(r, int(0x80000000));
    if (uint32_t(r[0]) < 0x80000004) return "unknown";
    char brand[49] = {};
    for (int i = 0; i < 3; i++) {
        __cpuid(r, int(0x80000002 + i));
        std::memcpy(brand + i * 16, r, 16);
    }
    std::string s = brand;
    s.erase(0, s.find_first_not_of(' '));
    s.erase(s.find_last_not_of(' ') + 1);
    return s;
}

#else

// The distribution's own name for itself (PRETTY_NAME in /etc/os-release), with the
// running kernel behind it: the counterpart of the Windows product name and build.
std::string OsVersion() {
    utsname u{};
    if (uname(&u) != 0) return "Linux (uname failed)";
    std::string pretty;
    std::ifstream f("/etc/os-release");
    for (std::string line; std::getline(f, line);) {
        if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
        std::string v = line.substr(12);
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) {
            char quote = v.front();
            v = v.substr(1, v.size() - 2);
            if (quote == '"') {  // os-release double quoting backslash-escapes the next char
                std::string un;
                for (size_t i = 0; i < v.size(); i++) {
                    if (v[i] == '\\' && i + 1 < v.size()) i++;
                    un += v[i];
                }
                v = un;
            }
        }
        pretty = v;
        break;
    }
    std::string out = pretty.empty() ? std::string(u.sysname) + " " + u.release : pretty;
    out += " (kernel ";
    out += u.release;
    out += ' ';
    out += u.machine;
    out += ')';
    return out;
}

// The same CPUID brand leaves the Windows path reads (0x80000002..4), through
// <cpuid.h>'s __get_cpuid instead of MSVC's __cpuid intrinsic.
std::string CpuName() {
    if (__get_cpuid_max(0x80000000, nullptr) < 0x80000004) return "unknown";
    char brand[49] = {};
    for (unsigned i = 0; i < 3; i++) {
        unsigned int r[4] = {};
        if (!__get_cpuid(0x80000002 + i, &r[0], &r[1], &r[2], &r[3])) return "unknown";
        std::memcpy(brand + i * 16, r, 16);
    }
    std::string s = brand;
    size_t first = s.find_first_not_of(' ');
    if (first == std::string::npos) return "unknown";
    s.erase(0, first);
    s.erase(s.find_last_not_of(' ') + 1);
    return s;
}

// This process's own executable, resolved the way ExeDir resolves it in main_sdl.cpp.
std::string ExePath() {
    char self[4096] = {};
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) return {};
    return {self, size_t(n)};
}

// Windows enumerates a GPU line per display adapter; Linux has no equivalent list to
// read before the renderer starts (the Vulkan backend logs its "Vulkan adapter: ..."
// line when it picks a device), so the honest fact here is the display server this
// session is running on, named by the environment every compositor sets.
std::string DisplayStack() {
    if (const char* w = std::getenv("WAYLAND_DISPLAY"); w && w[0])
        return std::string("Wayland (WAYLAND_DISPLAY=") + w + ")";
    if (const char* x = std::getenv("DISPLAY"); x && x[0]) return std::string("X11 (DISPLAY=") + x + ")";
    return "no display server in the environment (neither WAYLAND_DISPLAY nor DISPLAY is set)";
}

#endif

}  // namespace

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls (%s), %s", kGame.title, IMAGE_XBE, date);
#if defined(_WIN32)
    LOGR("Path: %ls", ExeDir().c_str());
#else
    LOGR("Path: %s", ExeDir().string().c_str());
    std::string exe = ExePath();
    if (exe.empty()) {
        LOGR("Exe: /proc/self/exe could not be read");
    } else {
        std::error_code ec;
        uintmax_t size = std::filesystem::file_size(exe, ec);
        if (ec) LOGR("Exe: %s", exe.c_str());
        else LOGR("Exe: %s (%llu bytes)", exe.c_str(), (unsigned long long)size);
    }
#endif
    LOGR("OS: %s", OsVersion().c_str());
#if defined(_WIN32)
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    LOGR("CPU: %s (%lu threads)", CpuName().c_str(), si.dwNumberOfProcessors);
    MEMORYSTATUSEX mem{sizeof(mem)};
    GlobalMemoryStatusEx(&mem);
    LOGR("RAM: %.1f GB", mem.ullTotalPhys / 1073741824.0);
    // one line per adapter, each monitor reports its own
    std::vector<std::string> seen;
    for (DWORD i = 0;; i++) {
        DISPLAY_DEVICEA dd{sizeof(dd)};
        if (!EnumDisplayDevicesA(nullptr, i, &dd, 0)) break;
        if (std::find(seen.begin(), seen.end(), dd.DeviceString) != seen.end()) continue;
        seen.emplace_back(dd.DeviceString);
        LOGR("GPU %zu: %s%s", seen.size(), dd.DeviceString, (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " (primary)" : "");
    }
#else
    unsigned threads = std::thread::hardware_concurrency();
    if (threads) LOGR("CPU: %s (%u threads)", CpuName().c_str(), threads);
    else LOGR("CPU: %s (thread count unavailable)", CpuName().c_str());
    long pages = sysconf(_SC_PHYS_PAGES), page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0)
        LOGR("RAM: %.1f GB", double(uint64_t(pages) * uint64_t(page_size)) / 1073741824.0);
    else
        LOGR("RAM: unknown (sysconf reported no physical memory)");
    LOGR("Display: %s", DisplayStack().c_str());
#endif
    Mode desk = Desktop();
    LOGR("Desktop: %ux%u", desk.width, desk.height);
    LogSettings();
}

}  // namespace app

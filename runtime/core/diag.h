#pragma once
#include <array>
#include <cstdlib>
#include <filesystem>

// diagnostics switched on by environment variables, read once
namespace diag {

// XBE_PERF set: perf log lines once a second and the read-backs named
inline bool Perf() {
    static const bool on = std::getenv("XBE_PERF") != nullptr;
    return on;
}

// XBE_CAPTURE_DIR: where F3 saves a frame, empty: F3 does not capture
inline const std::filesystem::path& CaptureDir() {
    static const std::filesystem::path dir = [] {
#ifdef _WIN32
        const wchar_t* v = _wgetenv(L"XBE_CAPTURE_DIR");
#else
        const char* v = std::getenv("XBE_CAPTURE_DIR");
#endif
        return v ? std::filesystem::path(v) : std::filesystem::path();
    }();
    return dir;
}

// XBE_CAPTURE_PROBE "x,y": a point of the presented picture as fractions of its width and height. An F3 capture reads
// that pixel back after every clear and draw into the frame and lists the ones that changed it or whose triangles
// reach it; false when unset or not two fractions
inline bool CaptureProbe(float& x, float& y) {
    static const std::array<float, 2> at = [] {
        std::array<float, 2> none{-1, -1};
        const char* v = std::getenv("XBE_CAPTURE_PROBE");
        if (!v) return none;
        char* end = nullptr;
        float px = std::strtof(v, &end);
        if (end == v || *end != ',') return none;
        const char* ys = end + 1;
        float py = std::strtof(ys, &end);
        if (end == ys || !(px >= 0 && px <= 1 && py >= 0 && py <= 1)) return none;
        return std::array<float, 2>{px, py};
    }();
    x = at[0];
    y = at[1];
    return x >= 0;
}

}  // namespace diag

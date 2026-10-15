// the settings file beside the exe
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/log.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

// the aspects a setting can name; auto is first and takes the output's own
struct NamedAspect {
    const char* key;
    float aspect;
};
const NamedAspect kAspects[] = {{"auto", 0.0f},           {"4:3", 4.0f / 3.0f},   {"16:9", 16.0f / 9.0f},
                                {"16:10", 16.0f / 10.0f}, {"21:9", 21.0f / 9.0f}, {"32:9", 32.0f / 9.0f}};

// the chosen entry's index, auto for a name this build does not know
uint32_t ChosenAspect() {
    const std::string& key = Get().aspect_ratio;
    for (uint32_t i = 1; i < std::size(kAspects); i++)
        if (key == kAspects[i].key) return i;
    return 0;
}

std::filesystem::path File() { return ExeDir() / kGame.settings; }

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

void Load() {
    Settings& s = Get();
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    std::string aspect = t["aspect_ratio"].value_or(s.aspect_ratio);
    for (const NamedAspect& a : kAspects)
        if (aspect == a.key) s.aspect_ratio = aspect;
    s.ultrawide = t["ultrawide"].value_or(s.ultrawide);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"aspect_ratio", s.aspect_ratio},
                  {"ultrawide", s.ultrawide},
                  {"join_ip", s.join_ip}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

void Save() {
    std::ofstream f(File());
    f << Table() << "\n";
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

Mode Desktop() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}

Mode Output() {
    const Settings& s = Get();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

namespace {

// the frame's shape: what the console is told, how much wider than 16:9 it is drawn, what it is shown at
struct Shape {
    bool wide;
    float widen;
    float target;
};

// asked per frame, per viewport change and per fold push: the desktop mode lookup only runs when an input changes
Shape Shaped() {
    static std::atomic<uint64_t> mode_key{~0ull};
    static std::atomic<uint32_t> pick_key{~0u};
    static std::atomic<bool> wide{true};
    static std::atomic<float> widen{1.0f};
    static std::atomic<float> target{16.0f / 9.0f};
    const Settings& s = Get();
    uint64_t mode = uint64_t(s.width) << 32 | s.height;
    uint32_t pick = ChosenAspect() << 1 | uint32_t(s.ultrawide);
    bool changed = mode_key.exchange(mode) != mode;
    changed |= pick_key.exchange(pick) != pick;
    if (changed) {
        Mode m = Output();
        bool w = m.width * 2 >= m.height * 3;  // 3:2 or wider
        uint32_t chosen = pick >> 1;
        float aspect = chosen ? kAspects[chosen].aspect
                              : (m.height ? float(m.width) / float(m.height) : 16.0f / 9.0f);
        // a 16:9 output divides out to exactly 1: every number downstream stays what it was
        float f = w && s.ultrawide ? std::clamp(aspect / (16.0f / 9.0f), 1.0f, 4.0f) : 1.0f;
        wide = w;
        widen = f;
        target = w ? f * (16.0f / 9.0f) : 4.0f / 3.0f;
    }
    return {wide, widen, target};
}

}  // namespace

bool Widescreen() { return Shaped().wide; }

float AspectWiden() { return Shaped().widen; }

float TargetAspect() { return Shaped().target; }

float RenderScale(uint32_t guest_height) {
    float h = Output().height * (Get().render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

}  // namespace app

// the settings file beside the exe
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <atomic>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#if !defined(_WIN32)
#include "app/platform.h"
#endif
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

const char* kHdrModes[] = {"off", "auto", "on"};  // HdrMode's order

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
    // a file from before HDR has none of these: off, and the rest as they default
    std::string hdr = t["hdr"].value_or(s.hdr);
    for (const char* m : kHdrModes)
        if (hdr == m) s.hdr = hdr;
    s.hdr_paper_white = t["hdr_paper_white"].value_or(s.hdr_paper_white);
    s.hdr_peak = t["hdr_peak"].value_or(s.hdr_peak);
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
                  {"join_ip", s.join_ip},
                  {"hdr", s.hdr},
                  {"hdr_paper_white", int64_t(s.hdr_paper_white)},
                  {"hdr_peak", int64_t(s.hdr_peak)}};
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
#if defined(_WIN32)
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
#else
    // The same answer through SDL3: the bounds of the display the window is on, the
    // primary display before the window exists. platform::DesktopSize() (the same calls
    // over the current display mode) is the fallback when SDL has no display to name yet,
    // e.g. LogSystem asking before SDL_Init; it falls back to 1280x720 itself.
    SDL_Window* window = platform::SdlWindow();
    SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) : SDL_GetPrimaryDisplay();
    SDL_Rect bounds{};
    if (display && SDL_GetDisplayBounds(display, &bounds) && bounds.w > 0 && bounds.h > 0)
        return {uint32_t(bounds.w), uint32_t(bounds.h)};
    platform::Size desk = platform::DesktopSize();
    return {desk.width, desk.height};
#endif
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

float HostAspect() {
    // Host Hor+ widening (the Vulkan frame target, the XDK centre shift and the
    // guest projection override) engages only past 16:9: at 16:9 the title is
    // already projecting its own anamorphic widescreen into the same buffer, so
    // presenting that buffer at 16:9 is the faithful picture and widening the
    // target would only re-derive it. Wider aspects have no title mode, so the
    // host widens for real: target, projection and present all at this aspect.
    //
    // OFF unless SIMSBO_HORPLUS is set. The widening is written but has never
    // been proven in a loaded lot, and a partial one has twice shipped a broken
    // picture (the HUD off-centre, the world squeezed back into a 4:3 box). It
    // stays opt-in until a household screenshot shows: no bars, the HUD the same
    // size as with it off, and more world at the sides at an unchanged height.
    // Without it a wider-than-16:9 display gets the title's own 16:9 picture
    // pillarboxed, which is correct, just not yet generous.
    static const bool enabled = [] {
        const char* v = std::getenv("SIMSBO_HORPLUS");
        return v && v[0] && std::strcmp(v, "0") != 0;
    }();
    if (!enabled || !Get().ultrawide) return 0.0f;
    float target = TargetAspect();
    return target > 16.0f / 9.0f + 1e-4f ? target : 0.0f;
}

float RenderScale(uint32_t guest_height) {
    float h = Output().height * (Get().render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

HdrMode HdrWanted() {
    const std::string& m = Get().hdr;
    for (uint32_t i = 1; i < std::size(kHdrModes); i++)
        if (m == kHdrModes[i]) return HdrMode(i);
    return HdrMode::Off;
}

// Windows shows SDR content in HDR at 80 to 480 nits; below 80 the game would be dimmer than scRGB's own white
uint32_t PaperWhiteNits() { return std::clamp(Get().hdr_paper_white, 80u, 480u); }

// 10000 nits is where PQ, the signal an HDR display is sent, ends
uint32_t PeakNits() {
    uint32_t p = Get().hdr_peak;
    return p ? std::clamp(p, 80u, 10000u) : 0;
}

namespace {

std::mutex g_hdr_lock;  // the renderer sets the status, the UI reads it
HdrStatus g_hdr{false, false, 0, "the display was not looked at yet"};

}  // namespace

HdrStatus Hdr() {
    std::lock_guard lock(g_hdr_lock);
    return g_hdr;
}

void SetHdr(const HdrStatus& s) {
    std::lock_guard lock(g_hdr_lock);
    g_hdr = s;
}

}  // namespace app

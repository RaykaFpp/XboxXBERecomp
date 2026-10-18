#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace input {
struct Pad;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the output height
    uint32_t fps_cap = 60;           // 0 unlimited
    bool vsync = true;
    uint32_t aniso = 0;              // 0 as the game asks
    // the shape the frame is drawn and shown at: auto is the real output aspect, else 4:3, 16:9, 16:10, 21:9 or 32:9
    std::string aspect_ratio = "auto";
    bool ultrawide = true;  // false: the frame stays 16:9 and every number below is exactly what it was
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
    // HDR output (gpu/d3d12.cpp): off never, auto when the window's display is in HDR in Windows, on always
    std::string hdr = "off";
    uint32_t hdr_paper_white = 200;  // nits the game's white is shown at in HDR
    uint32_t hdr_peak = 0;           // nits nothing goes past in HDR, 0 the display's own maximum
};

Settings& Get();
void Load();
void Save();
void LogSettings();  // every setting, as the settings file has them
void LogSystem();    // the machine and the settings, at a run's start
void ApplyDisplay();  // any thread
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
Mode Output();
float RenderScale(uint32_t guest_height);
// the output is 3:2 or wider: the console reports a widescreen TV and the frame is shown 16:9, else 4:3 as on a 4:3 TV
bool Widescreen();
// how much wider than 16:9 the frame is rendered, 1 to 4; exactly 1 when ultrawide is off, the output is not
// widescreen, or the chosen aspect is no wider than 16:9. widening moves the viewport offset, never its scale,
// so the extra width is extra world at the sides: Hor+ at an unchanged vertical field of view
float AspectWiden();
// the aspect the frame is presented at: the widened 16:9 on a widescreen output, else 4:3
float TargetAspect();

// HDR output: the settings read each present, so a change shows at once without app::ApplyDisplay
enum class HdrMode { Off, Auto, On };
HdrMode HdrWanted();        // the setting, parsed: anything but "auto" and "on" is off
uint32_t PaperWhiteNits();  // the setting, clamped to 80-480 nits, the range Windows offers SDR content in HDR
uint32_t PeakNits();        // the setting, 0 means the display decides, else clamped to 80-10000 nits
// set by the renderer each time the swap chain is made or the window's display changes, read by the UI
struct HdrStatus {
    bool active;                // the swap chain is FP16 scRGB
    bool display_capable;       // the window's display is in HDR in Windows: DXGI reports it in PQ
    uint32_t display_max_nits;  // that display's peak as it reports it, also when HDR is off; 0 unknown
    // never null, lowercase without a full stop: why HDR is off, or, active on a display Windows runs in SDR, why
    // the picture stays at SDR brightness; "" when active on an HDR display
    const char* why;
};
HdrStatus Hdr();
void SetHdr(const HdrStatus&);

std::filesystem::path ExeDir();
// the game in assets (game_files.cpp): its folder and this executable's xbe, from the extracted files or a disc
// image (.iso), which it mounts as D:; false after telling the player what is missing
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
bool FindGame(GameFiles& game);
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go

// held around ImGui: the window thread's messages and the renderer's frame
std::recursive_mutex& UiLock();

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file beside the exe
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when neither the game files nor a disc image are found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    std::vector<Option> options;
    // the settings file's toml::table, for what options cannot hold
    void (*load)(const void* table);
    void (*save)(void* table);
    void (*init)();                // after the settings load, before the window and boot
    void (*loaded)();              // image in memory, before its entry runs
    void (*window)(void* hwnd);    // the game window is up, before boot
    // window thread, each message after ImGui's; true returns result instead of the host's handling
    bool (*message)(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
    void (*ui)(double fps);        // each frame inside the ImGui frame, UiLock held
    void (*pad)(int port, input::Pad& pad);  // a polled pad before the game gets it
    void (*rumble)(int port, uint16_t& left, uint16_t& right);
    // The rows of the console's frame that hold picture. A TV overscanned the 480-line signal, so a
    // title could draw a shorter picture inside the frame and no console ever showed the dead rows;
    // a PC window has no overscan and shows them as black bars at the top and bottom. The host
    // presents these rows alone, scaled to the output. Height 0 presents the whole frame.
    uint32_t picture_top = 0, picture_height = 0;
};
extern const GameInfo kGame;

// sleeps out the rest of the frame for the fps cap
void Pace();

}  // namespace app

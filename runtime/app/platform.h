#pragma once
// The host-window seam: everything the rest of the runtime needs from the
// platform window, without naming Win32 or SDL types. app/app.h keeps the
// game-facing declarations (settings, splash and handover hooks, UiLock,
// Pace); this header is the lower layer underneath them.
//
// Windows: implemented in app/main.cpp over the HWND window.
// Linux:   implemented in app/main_sdl.cpp over the SDL3 window.
//
// Threading: unless noted, call only on the window thread after the window
// exists. NativeHandle()/SdlWindow() return nullptr before creation.
#include <cstdint>

#if defined(_WIN32)
// Keep the header free of <windows.h>: HWND is only ever passed through as
// the opaque NativeWindow below, exactly as app::Window() already does.
#else
struct SDL_Window;
#endif

namespace app::platform {


// The native window handle: HWND on Windows, SDL_Window* on Linux. This is
// the same pointer app::Window() returns, typed for platform code.
using NativeWindow = void*;
NativeWindow NativeHandle();

#if !defined(_WIN32)
// The typed SDL window, for SDL_Vulkan_CreateSurface and the ImGui SDL3
// backend. Nullptr before the window is created and after it is destroyed.
SDL_Window* SdlWindow();
#endif

struct Size {
    uint32_t width = 0;
    uint32_t height = 0;
};

// Drawable client size in pixels (SDL_GetWindowSizeInPixels / Win32 client
// rect). {0,0} when there is no window yet.
Size ClientSize();
// Size of the display the window is on (its current display mode), falling
// back to the primary display. Backs app::Desktop() on each platform.
Size DesktopSize();

// Input focus: the window is the foreground/keyboard-focus window. Keyboard
// and mouse-to-pad polling gates on this so an alt-tabbed game stands down.
bool HasFocus();
// The window is mapped/shown (not hidden for a splash or handover).
bool IsVisible();
bool IsMinimized();
void SetVisible(bool visible);
// Raise the window and give it input focus (OpenGameWindow's foreground step).
void Raise();

// Cursor: shown by default; the game/overlay hide it while capturing the
// mouse. Maps to ShowCursor on Windows, SDL_Show/HideCursor on Linux.
void SetCursorVisible(bool visible);
bool CursorVisible();

// HDR capability of the display the window is on, honestly reported.
// On Linux there is no HDR output path in this build: QueryHdr() always
// returns capable=false with why naming that, so app::Hdr() never claims
// an HDR swap chain that does not exist. On Windows it mirrors the status
// the renderer published through app::SetHdr().
struct HdrInfo {
    bool capable = false;       // the display can show HDR at all
    bool active = false;        // the current swap chain is HDR
    uint32_t max_nits = 0;      // display peak, 0 unknown
    const char* why = "";       // never null; why HDR is off, "" when active
};
HdrInfo QueryHdr();

}  // namespace app::platform

namespace app {
// The aspect the host renders and presents the frame at when host Hor+ widening
// is active: the chosen aspect, or the output's own, when `ultrawide` is on and
// that aspect is wider than 16:9. Zero when widening is off (ultrawide off, or an
// aspect no wider than 16:9 - 16:9 itself is the title's own anamorphic
// widescreen, presented as authored, never widened). Defined in settings.cpp.
float HostAspect();
}  // namespace app

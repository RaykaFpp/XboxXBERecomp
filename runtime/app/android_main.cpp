// Android SDL3/Vulkan application host. This deliberately omits desktop Linux
// process handling; game data is selected from shared storage at /Sims2.
#if defined(__ANDROID__)

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

#include <SDL3/SDL_system.h>
#include <SDL3/SDL_main.h>

#include "app/app.h"
#include "app/platform.h"
#include "common/input_kbm.h"  // kbm::NoteMouse (D11 mouse cache)
#include "common/overlay.h"
#include "gpu/spirv.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"  // gpu::g_capture (F3)
#include "gpu/vulkan.h"
#include "image_symbols.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace app {

namespace {

SDL_Window* g_window = nullptr;
char** g_argv = nullptr;
int g_argc = 0;
// Written on the window thread (creation, resize events, ApplyNow), read on
// any thread: SDL video calls belong to the main thread, guest threads do not.
std::atomic<uint32_t> g_client_w{0}, g_client_h{0};
std::atomic<bool> g_focus{false};
std::atomic<bool> g_imgui_vulkan{false};
struct TouchControl {
    SDL_FingerID finger = 0;
    bool active = false;
    int slot = -1;
    input::Pad pad;
};
std::array<TouchControl, 10> g_touch_controls{};
// Cross-thread requests to the window thread, mirroring main.cpp's
// kApplyDisplay / kOpenWindow window messages.
Uint32 g_apply_event = 0, g_open_event = 0;

std::string Narrow(const wchar_t* wide) {
    std::string out;
    if (!wide) return out;
    for (const wchar_t* p = wide; *p; p++) {
        char32_t c = char32_t(*p);  // wchar_t is one UTF-32 code unit on Linux
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

bool Relaunched() { return false; }

// Window thread only. SDL_GetMouseState is main-thread-only, so it is sampled
// here, in the event loop, and published through kbm::NoteMouse's atomics; guest
// polling threads read that cache and never call SDL_GetMouseState themselves.
void CacheMouseState() {
    float x = 0.0f, y = 0.0f;
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(&x, &y);
    kbm::NoteMouse(x, y, (buttons & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) != 0,
                   (buttons & SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)) != 0);
}

void CacheClientSize() {
    if (!g_window) return;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    g_client_w = uint32_t(std::max(w, 0));
    g_client_h = uint32_t(std::max(h, 0));
}

// F3, as on Windows: dump every guest thread's stack and capture the frame.
void DumpThreads() {
    gpu::g_capture = true;
    for (auto& t : kernel::Threads())
        if (t->ctx) {
            LOGI("thread %X", t->id);
            kernel::LogGuestStack(*t->ctx, 32);
        }
}

// No minidump on Linux: the guest registers and return addresses go to the
// log, then the process dies with the signal, as the Windows filter leaves
// it to the system after writing its dump.
void CrashHandler(int sig) {
    if (kernel::Thread* t = kernel::Current(); t && t->ctx) {
        LOGE("signal %d on guest thread %X", sig, t->id);
        kernel::LogGuestStack(*t->ctx, 256);
    } else {
        LOGE("signal %d", sig);
    }
    logx::Flush();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
    _exit(128 + sig);
}

// Android display settings are fixed to the current full-screen display.
void ApplyNow() {
    if (!g_window) return;
    SDL_SetWindowFullscreen(g_window, true);
    CacheClientSize();
}

struct OpenRequest {
    bool apply;
    SDL_Semaphore* done;
};

void DoOpenGameWindow(bool apply) {
    CloseSplash();
    if (apply) ApplyNow();
    SDL_ShowWindow(g_window);
    SDL_RaiseWindow(g_window);
}

// The Vulkan backend owns the device; once its Init has run (on a guest
// thread), finish the ImGui Vulkan backend on this thread under UiLock.
void TryInitImGuiVulkan() {
    if (g_imgui_vulkan.load(std::memory_order_relaxed) || !ImGui::GetCurrentContext()) return;
    const gpu::VulkanHandles& h = gpu::GetVulkanHandles();
    if (h.device == VK_NULL_HANDLE || h.renderPass == VK_NULL_HANDLE) return;
    std::lock_guard lock(UiLock());
    if (g_imgui_vulkan.load(std::memory_order_relaxed)) return;
    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = h.instance;
    info.PhysicalDevice = h.physicalDevice;
    info.Device = h.device;
    info.QueueFamily = h.graphicsQueueFamily;
    info.Queue = h.graphicsQueue;
    info.PipelineCache = h.pipelineCache;
    info.DescriptorPool = h.descriptorPool;
    info.RenderPass = h.renderPass;
    info.Subpass = 0;
    info.MinImageCount = h.minImageCount;
    info.ImageCount = h.imageCount;
    info.MSAASamples = h.msaaSamples;
    info.UseDynamicRendering = false;
    if (!ImGui_ImplVulkan_Init(&info)) {
        LOGE("imgui: Vulkan backend initialization failed");
        return;
    }
    g_imgui_vulkan.store(true, std::memory_order_relaxed);
    LOGI("imgui: Vulkan backend ready (%u images)", h.imageCount);
}

void HandleTouch(const SDL_TouchFingerEvent& touch) {
    auto it = std::find_if(g_touch_controls.begin(), g_touch_controls.end(), [&](const TouchControl& c) {
        return c.active && c.finger == touch.fingerID;
    });
    if (touch.type == SDL_EVENT_FINGER_UP || touch.type == SDL_EVENT_FINGER_CANCELED) {
        if (it != g_touch_controls.end()) {
            input::SetTouchControl(it->slot, false, {});
            *it = {};
        }
    } else if (touch.type == SDL_EVENT_FINGER_DOWN || touch.type == SDL_EVENT_FINGER_MOTION) {
        if (it == g_touch_controls.end()) {
            it = std::find_if(g_touch_controls.begin(), g_touch_controls.end(), [](const TouchControl& c) { return !c.active; });
            if (it == g_touch_controls.end()) return;
            it->active = true;
            it->slot = int(it - g_touch_controls.begin());
            it->finger = touch.fingerID;
        }
        const float w = float(std::max(g_client_w.load(std::memory_order_relaxed), 1u));
        const float h = float(std::max(g_client_h.load(std::memory_order_relaxed), 1u));
        const float x = touch.x * w, y = touch.y * h;
        input::Pad pad{};
        pad.connected = true;
        if (x < w * 0.45f) {
            float dx = (x - w * 0.22f) / (w * 0.15f);
            float dy = (y - h * 0.70f) / (h * 0.20f);
            float length = std::sqrt(dx * dx + dy * dy);
            if (length > 1.0f) { dx /= length; dy /= length; }
            pad.lx = int16_t(dx * 32767.0f);
            pad.ly = int16_t(-dy * 32767.0f);
        } else if (y < h * 0.32f) {
            pad.buttons = 0x0010;  // Start
        } else if (y > h * 0.68f) {
            pad.buttons = 0x0020;  // Back
        } else {
            const float xfrac = x / w, yfrac = y / h;
            if (xfrac < 0.80f && yfrac < 0.50f) pad.analog[0] = 0xFF;  // A
            else if (xfrac >= 0.80f && yfrac < 0.50f) pad.analog[1] = 0xFF;  // B
            else if (xfrac < 0.80f) pad.analog[2] = 0xFF;  // X
            else pad.analog[3] = 0xFF;  // Y
        }
        it->pad = pad;
        input::SetTouchControl(it->slot, true, pad);
    } else {
        return;
    }

}

bool HandleEvent(const SDL_Event& e) {  // window thread; true: quit
    // ImGui first, under the same lock the renderer builds its frame with,
    // as ImGui_ImplWin32_WndProcHandler is in main.cpp's WndProc.
    if (ImGui::GetCurrentContext()) {
        std::lock_guard lock(UiLock());
        ImGui_ImplSDL3_ProcessEvent(&e);
    }
    if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        if (!overlay::SettingsOpen()) kbm::NoteWheel(int(std::lround(e.wheel.y)));
        return false;
    }
    // GameInfo::message is a Win32-message hook; there are no window
    // messages to offer it here, and games leave it null off Windows.
    if (g_apply_event && e.type == g_apply_event) {
        ApplyNow();
        return false;
    }
    if (g_open_event && e.type == g_open_event) {
        auto* req = static_cast<OpenRequest*>(e.user.data1);
        DoOpenGameWindow(req->apply);
        SDL_SignalSemaphore(req->done);
        return false;
    }
    if (e.type == SDL_EVENT_FINGER_DOWN || e.type == SDL_EVENT_FINGER_MOTION ||
        e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED) {
        HandleTouch(e.tfinger);
        return false;
    }
    if (e.type == SDL_EVENT_QUIT) return true;
    if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(g_window)) return true;
    // Pixel size / display scale changes refresh the cached drawable size; the
    // Vulkan backend polls it (platform::ClientSize) in ResizeSwapIfNeeded and
    // recreates the swapchain at the new size - on Wayland the swapchain
    // extent is the window size, so this event is what drives fullscreen.
    if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || e.type == SDL_EVENT_WINDOW_RESIZED ||
        e.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
        uint32_t was_w = g_client_w.load(std::memory_order_relaxed), was_h = g_client_h.load(std::memory_order_relaxed);
        CacheClientSize();
        if (g_client_w.load(std::memory_order_relaxed) != was_w || g_client_h.load(std::memory_order_relaxed) != was_h)
            LOGI("display: window drawable now %ux%u", g_client_w.load(std::memory_order_relaxed),
                 g_client_h.load(std::memory_order_relaxed));
    }
    if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) g_focus = true;
    if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        g_focus = false;
        for (TouchControl& control : g_touch_controls) {
            if (control.active) input::SetTouchControl(control.slot, false, {});
            control = {};
        }
    }
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
        if (e.key.scancode == SDL_SCANCODE_F3) {
            DumpThreads();
        } else if (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)) {
            SDL_SetWindowFullscreen(g_window, (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) == 0);
            CacheClientSize();
        }
        // F1/F2 need no host handling: ImGui has the key from ProcessEvent
        // above and the overlay toggles itself in GameInfo::ui.
    }
    return false;
}

}  // namespace

std::filesystem::path ExeDir() {
    const char* path = SDL_GetAndroidInternalStoragePath();
    return std::filesystem::path(path ? path : ".");
}

int Instance() {
    static const int n = [] {
        auto clamp = [](int v) { return std::max(0, std::min(8, v)); };
        for (int i = 1; i < g_argc; i++) {
            if (!g_argv[i]) continue;
            // D10: the documented forms are "--instance 2" (two argv entries) and
            // "--instance=2"; the old strstr(p + 10) read the empty string / "=2"
            // for both and silently yielded instance 0. The glued "--instance2"
            // that used to be the only working form is still accepted.
            if (std::strcmp(g_argv[i], "--instance") == 0) {
                if (i + 1 < g_argc && g_argv[i + 1]) return clamp(std::atoi(g_argv[i + 1]));
                return 0;
            }
            if (std::strncmp(g_argv[i], "--instance=", 11) == 0) return clamp(std::atoi(g_argv[i] + 11));
            if (std::strncmp(g_argv[i], "--instance", 10) == 0 && g_argv[i][10] >= '0' && g_argv[i][10] <= '9')
                return clamp(std::atoi(g_argv[i] + 10));
        }
        return 0;
    }();
    return n;
}

void* Window() { return g_window; }

std::recursive_mutex& UiLock() {
    static std::recursive_mutex m;
    return m;
}

void ApplyDisplay() {
    if (!g_window) return;
    if (g_apply_event) {
        SDL_Event e{};
        e.type = g_apply_event;
        SDL_PushEvent(&e);
    } else {
        ApplyNow();
    }
}

void OpenGameWindow(bool apply) {
    if (!g_window) return;
    if (!g_open_event) {
        DoOpenGameWindow(apply);
        return;
    }
    // SendMessage on Windows: returns once the window is up.
    SDL_Semaphore* done = SDL_CreateSemaphore(0);
    OpenRequest req{apply, done};
    SDL_Event e{};
    e.type = g_open_event;
    e.user.data1 = &req;
    if (!SDL_PushEvent(&e)) {
        DoOpenGameWindow(apply);
        SDL_DestroySemaphore(done);
        return;
    }
    SDL_WaitSemaphore(done);
    SDL_DestroySemaphore(done);
}

void Pace() {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    uint32_t cap = Get().fps_cap;
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

namespace platform {

NativeWindow NativeHandle() { return g_window; }

SDL_Window* SdlWindow() { return g_window; }

Size ClientSize() { return {g_client_w.load(std::memory_order_relaxed), g_client_h.load(std::memory_order_relaxed)}; }

Size DesktopSize() {
    SDL_DisplayID display = g_window ? SDL_GetDisplayForWindow(g_window) : SDL_GetPrimaryDisplay();
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display)) return {uint32_t(mode->w), uint32_t(mode->h)};
    return {1280, 720};
}

bool HasFocus() { return g_focus.load(std::memory_order_relaxed); }

bool IsVisible() { return g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_HIDDEN) == 0; }

bool IsMinimized() { return g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) != 0; }

void SetVisible(bool visible) {
    if (!g_window) return;
    if (visible) SDL_ShowWindow(g_window);
    else SDL_HideWindow(g_window);
}

void Raise() {
    if (g_window) SDL_RaiseWindow(g_window);
}

void SetCursorVisible(bool visible) {
    if (visible) SDL_ShowCursor();
    else SDL_HideCursor();
}

bool CursorVisible() { return SDL_CursorVisible(); }

HdrInfo QueryHdr() {
    // No HDR swap chain exists in the Android Vulkan build; say so, never pretend.
    return {false, false, 0, "HDR is not available on Android"};
}

}  // namespace platform

}  // namespace app

extern "C" int SDL_main(int argc, char** argv) {
    app::g_argc = argc;
    app::g_argv = argv;
    for (int sig : {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE}) std::signal(sig, app::CrashHandler);
    // One log a session: a relaunch into another image carries on its predecessor's.
    bool relaunched = app::Relaunched();
    std::string log = app::kGame.log;
    if (app::Instance() > 1) log += "-" + std::to_string(app::Instance());
    std::error_code path_error;
    std::filesystem::create_directories(app::ExeDir() / "logs", path_error);
    logx::Init((app::ExeDir() / "logs").string(), log, relaunched);
    app::Load();
    if (app::kGame.init) app::kGame.init();
    if (relaunched && !logx::kVerbose) LOGR("Relaunched: %s", IMAGE_XBE);
    else app::LogSystem();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        LOGE("sdl: video failed to start: %s", SDL_GetError());
        return 1;
    }
    Uint32 events = SDL_RegisterEvents(2);
    if (events != Uint32(-1)) {
        app::g_apply_event = events;
        app::g_open_event = events + 1;
    }

    std::filesystem::path game_dir("/storage/emulated/0/Sims2");
    if (!std::filesystem::is_directory(game_dir, path_error)) {
        LOGE("game: /storage/emulated/0/Sims2 is missing or inaccessible (%s)", path_error.message().c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "The Sims 2",
                                 "Create /storage/emulated/0/Sims2 and put your Xbox game files there.", nullptr);
        return 1;
    }
    app::GameFiles game;
    if (!app::FindGame(game)) return 1;

    bool splash = app::OpenSplash(), handover = app::StartHandover();

    app::Mode out = app::Output();
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
    // Android supplies a full-screen surface; desktop display modes do not apply.
    if (splash || handover) flags |= SDL_WINDOW_HIDDEN;
    std::string title = app::Narrow(app::kGame.title);
    app::g_window = SDL_CreateWindow(title.c_str(), int(std::max(out.width, 1u)), int(std::max(out.height, 1u)), flags);
    if (!app::g_window) {
        LOGE("sdl: window failed: %s", SDL_GetError());
        return 1;
    }
    // Android owns window placement; SDL positions are unsupported on this platform.
    app::CacheClientSize();
    app::g_focus = SDL_GetKeyboardFocus() == app::g_window;
    // HDR is honestly off on this platform, from the start.
    app::SetHdr({false, false, 0, "HDR is not available on Android"});

    // Vulkan handshake: the backend owns the instance; SDL names the
    // extensions it needs, the backend creates it, SDL makes the surface.
    uint32_t ext_count = 0;
    char const* const* exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
    if (!exts) {
        LOGE("sdl: no Vulkan instance extensions: %s", SDL_GetError());
        return 1;
    }
    gpu::SetVulkanInstanceExtensions(exts, ext_count);
    VkInstance instance = gpu::VulkanInstance();
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (instance == VK_NULL_HANDLE || !SDL_Vulkan_CreateSurface(app::g_window, instance, nullptr, &surface)) {
        LOGE("vulkan: surface failed: %s", SDL_GetError());
        return 1;
    }
    gpu::SetVulkanSurface(instance, surface);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad |
                      ImGuiConfigFlags_IsTouchScreen;
    ImGui_ImplSDL3_InitForVulkan(app::g_window);
    // ImGui_ImplVulkan_Init waits for the backend's device: TryInitImGuiVulkan
    // in the loop below, once gpu::GetVulkanHandles() is valid.

    input::Init();
    if (app::kGame.window) app::kGame.window(app::g_window);
    std::string hdd = app::Instance() > 1 ? "hdd" + std::to_string(app::Instance()) : "hdd";
    gpu::SetSpirvCacheDir(app::ExeDir() / "shadercache", "android-vulkan");
    if (app::Instance()) SDL_SetWindowTitle(app::g_window, (title + " [" + std::to_string(app::Instance()) + "]").c_str());
    // Behind a splash the window stays hidden until the game's first frame;
    // handed over, it is set up hidden.
    if (handover) {
        SDL_HideWindow(app::g_window);
    } else if (!splash) {
        SDL_ShowWindow(app::g_window);
    }
    if (!kernel::Boot(std::move(game.xbe), IMAGE_XBE, game.dir.string(), (app::ExeDir() / hdd).string())) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, app::Narrow(app::kGame.short_title).c_str(),
                                 "The game failed to start, see the log in the logs folder.", app::g_window);
        return 1;
    }

    bool quit = false;
    while (!quit) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 16)) {
            if (app::HandleEvent(e)) quit = true;
            while (!quit && SDL_PollEvent(&e))
                if (app::HandleEvent(e)) quit = true;
        }
        app::CacheMouseState();
        app::TryInitImGuiVulkan();
    }
    // Vulkan teardown handshake (the one main_sdl change in the present fix): the
    // render thread records ImGui draw data into the backend's command buffers, so
    // ImGui's pipeline can still be in flight here. Wait for the device to go idle
    // before ImGui destroys it - validation otherwise reports vkDestroyPipeline
    // on a pipeline still in use by a command buffer at shutdown.
    if (app::g_imgui_vulkan.load(std::memory_order_relaxed)) {
        const gpu::VulkanHandles& vh = gpu::GetVulkanHandles();
        if (vh.device != VK_NULL_HANDLE) vkDeviceWaitIdle(vh.device);
        ImGui_ImplVulkan_Shutdown();
    }
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    // The instance and surface belong to the Vulkan backend and die with it
    // in kernel::Shutdown; the window is the process's, exiting with it.
    logx::Flush();
    kernel::Shutdown(0);
}

#endif  // defined(__ANDROID__)

// The boot splash: GameInfo::splash .bmp or .png beside the exe, centred in a borderless window. The game boots behind
// it with its window hidden and waits at its first frame until the splash has been up kMinimum, since this boot takes
// a fraction of a second. No file, or a relaunch: no splash.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#else
#include <SDL3/SDL.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#if defined(_WIN32)
#include <cwchar>
#endif
#include <string>
#include <thread>
#include <vector>

#include "app/app.h"
#if !defined(_WIN32)
#include "app/image_decode.h"
#endif
#include "core/log.h"

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;
#endif

namespace app {

namespace {

constexpr auto kMinimum = std::chrono::seconds(1);

std::atomic<bool> g_waiting{false};
std::chrono::steady_clock::time_point g_until;

#if defined(_WIN32)

HWND g_splash = nullptr;

// premultiplied BGRA, as UpdateLayeredWindow wants it; a picture without alpha comes out opaque
bool Read(const std::filesystem::path& path, std::vector<uint32_t>& pixels, UINT& w, UINT& h) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr))
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                   WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = converter->GetSize(&w, &h);
    if (SUCCEEDED(hr) && w && h) {
        pixels.resize(size_t(w) * h);
        hr = converter->CopyPixels(nullptr, w * 4, UINT(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data()));
    }
    if (FAILED(hr) || !w || !h) {
        LOGW("splash: %s could not be read (%08X)", path.string().c_str(), unsigned(hr));
        return false;
    }
    return true;
}

#else  // !defined(_WIN32)

// Linux: a borderless, always-on-top SDL window with the picture as a
// texture, the counterpart of the layered Win32 window above. SDL video is
// started here (refcounted) because app/main_sdl.cpp calls OpenSplash
// before its own SDL_Init; images come from SDL_LoadBMP (.bmp) and from
// app/image_decode.cpp (.png, decoded with zlib - SDL3_image is absent).
SDL_Window* g_splash_window = nullptr;
SDL_Renderer* g_splash_renderer = nullptr;
SDL_Texture* g_splash_texture = nullptr;

// wchar_t is one UTF-32 code unit on Linux; game names are plain text.
std::string Narrow(const wchar_t* wide) {
    std::string out;
    if (!wide) return out;
    for (const wchar_t* p = wide; *p; p++) {
        char32_t c = char32_t(*p);
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

// The --relaunched rule, as GetCommandLineW serves it on Windows:
// /proc/self/cmdline is the same command line, NUL-separated.
bool Relaunched() {
    FILE* f = std::fopen("/proc/self/cmdline", "rb");
    if (!f) return false;
    std::string cmd;
    char buf[1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) cmd.append(buf, n);
    std::fclose(f);
    return cmd.find("--relaunched") != std::string::npos;
}

void DestroySplashObjects() {
    if (g_splash_texture) SDL_DestroyTexture(g_splash_texture);
    if (g_splash_renderer) SDL_DestroyRenderer(g_splash_renderer);
    if (g_splash_window) SDL_DestroyWindow(g_splash_window);
    g_splash_texture = nullptr;
    g_splash_renderer = nullptr;
    g_splash_window = nullptr;
}

#endif  // defined(_WIN32)

}  // namespace

bool OpenSplash() {
#if defined(_WIN32)
    if (std::wcsstr(GetCommandLineW(), L"--relaunched")) return false;
    std::vector<uint32_t> pixels;
    UINT w = 0, h = 0;
    std::filesystem::path path;
    for (const wchar_t* ext : {L".bmp", L".png"}) {
        std::error_code ec;
        path = ExeDir() / (std::wstring(kGame.splash) + ext);
        if (std::filesystem::exists(path, ec) && Read(path, pixels, w, h)) break;
        pixels.clear();
    }
    if (pixels.empty()) return false;

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32514));  // IDC_WAIT
    std::wstring cls = std::wstring(kGame.name) + L"Splash";
    wc.lpszClassName = cls.c_str();
    RegisterClassExW(&wc);
    // centred on the primary screen at one image pixel per screen pixel
    POINT at{(GetSystemMetrics(SM_CXSCREEN) - int(w)) / 2, (GetSystemMetrics(SM_CYSCREEN) - int(h)) / 2};
    g_splash = CreateWindowExW(WS_EX_LAYERED, wc.lpszClassName, kGame.title, WS_POPUP,
                               at.x, at.y, int(w), int(h), nullptr, nullptr, inst, nullptr);
    if (!g_splash) return false;

    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), LONG(w), -LONG(h), 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits) {
        std::memcpy(bits, pixels.data(), pixels.size() * 4);
        HGDIOBJ old = SelectObject(dc, bitmap);
        SIZE size{LONG(w), LONG(h)};
        POINT origin{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(g_splash, screen, &at, &size, dc, &origin, 0, &blend, ULW_ALPHA);
        SelectObject(dc, old);
    }
    if (bitmap) DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    // to the front once, without staying on top
    SetWindowPos(g_splash, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(g_splash, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    g_until = std::chrono::steady_clock::now() + kMinimum;
    g_waiting = true;
    LOGI("splash: %s, %ux%u", path.string().c_str(), w, h);
    return true;
#else  // !defined(_WIN32)
    if (Relaunched()) return false;
    std::vector<uint8_t> pixels;  // straight RGBA8, from the PNG decoder
    uint32_t w = 0, h = 0;
    SDL_Surface* bmp = nullptr;  // set instead when the .bmp is the picture
    std::filesystem::path path;
    std::string base = Narrow(kGame.splash);
    for (const char* ext : {".bmp", ".png"}) {
        std::error_code ec;
        path = ExeDir() / (base + ext);
        if (!std::filesystem::exists(path, ec)) continue;
        if (ext[1] == 'b') {
            bmp = SDL_LoadBMP(path.string().c_str());
            if (bmp) {
                w = uint32_t(bmp->w);
                h = uint32_t(bmp->h);
                break;
            }
            LOGW("splash: %s could not be read (%s)", path.string().c_str(), SDL_GetError());
        } else if (DecodePng(path, pixels, w, h)) {
            break;
        }
        pixels.clear();
    }
    if (!bmp && pixels.empty()) return false;

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        LOGW("splash: SDL video failed to start: %s", SDL_GetError());
        if (bmp) SDL_DestroySurface(bmp);
        return false;
    }
    // centred on the primary screen at one image pixel per screen pixel
    std::string title = Narrow(kGame.title);
    g_splash_window = SDL_CreateWindow(title.c_str(), int(w), int(h),
                                       SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_UTILITY);
    if (!g_splash_window) {
        LOGW("splash: window failed: %s", SDL_GetError());
        if (bmp) SDL_DestroySurface(bmp);
        return false;
    }
    SDL_SetWindowPosition(g_splash_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    g_splash_renderer = SDL_CreateRenderer(g_splash_window, nullptr);
    if (!g_splash_renderer) {
        LOGW("splash: renderer failed: %s", SDL_GetError());
        DestroySplashObjects();
        if (bmp) SDL_DestroySurface(bmp);
        return false;
    }
    // The texture copies the pixels, so the surface (loaded .bmp, or the one
    // wrapping the decoded RGBA) is released either way.
    SDL_Surface* surface = bmp;
    if (!surface) {
        surface = SDL_CreateSurfaceFrom(int(w), int(h), SDL_PIXELFORMAT_RGBA32, pixels.data(), int(w) * 4);
        if (!surface) LOGW("splash: surface failed: %s", SDL_GetError());
    }
    if (surface) {
        g_splash_texture = SDL_CreateTextureFromSurface(g_splash_renderer, surface);
        SDL_DestroySurface(surface);
    }
    if (!g_splash_texture) {
        LOGW("splash: texture failed: %s", SDL_GetError());
        DestroySplashObjects();
        return false;
    }
    SDL_SetTextureBlendMode(g_splash_texture, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_splash_renderer, 0, 0, 0, 0);
    SDL_RenderClear(g_splash_renderer);
    SDL_RenderTexture(g_splash_renderer, g_splash_texture, nullptr, nullptr);
    SDL_RenderPresent(g_splash_renderer);
    g_until = std::chrono::steady_clock::now() + kMinimum;
    g_waiting = true;
    LOGI("splash: %s, %ux%u", path.string().c_str(), w, h);
    return true;
#endif  // defined(_WIN32)
}

void SplashFirstFrame() {
    if (!g_waiting.exchange(false)) return;
    std::this_thread::sleep_until(g_until);
    OpenGameWindow();
}

void CloseSplash() {
#if defined(_WIN32)
    if (g_splash) DestroyWindow(g_splash);
    g_splash = nullptr;
#else
    DestroySplashObjects();
#endif
}

}  // namespace app

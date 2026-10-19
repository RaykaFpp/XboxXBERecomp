// mixer output through SDL3 audio (non-Windows counterpart of xaudio2.cpp)
#if !defined(_WIN32)

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstring>
#include <thread>


#include "audio/audio.h"
#include "core/log.h"

namespace audio {
namespace {

// Mirrors xaudio2.cpp exactly: 48 kHz interleaved stereo IEEE float, rendered
// in kRate/100-frame (10 ms, 480-frame) chunks, with at most ~3 chunks' worth
// of audio ahead of the device. XAudio2 polls BuffersQueued < 3 before
// submitting another 10 ms buffer; here SDL's get-callback is the pull, and
// each callback is satisfied in the same 10 ms chunks, capped at the same
// three-chunk lookahead so latency does not drift above the Windows path.
constexpr uint32_t kFrames = kRate / 100;         // 480 frames = 10 ms
constexpr uint32_t kChannels = 2;
constexpr int kChunkBytes = int(kFrames * kChannels * sizeof(float));  // 3840
constexpr int kMaxChunksPerCallback = 3;

// Host-side proof counters, same meaning as in xaudio2.cpp: what actually
// reached the device, and the mixed output's peak since the last log line.
std::atomic<uint64_t> g_submitted{0}, g_rendered{0};
std::atomic<float> g_peak{0.0f};
std::atomic<bool> g_started{false};

struct Output {
    std::function<void(float*, uint32_t)> render;
    SDL_AudioStream* stream = nullptr;
    SDL_AudioDeviceID device = 0;
    // SDL_GetTicksNS() of the last get-callback (or of stream open). The
    // watcher reopens the stream when callbacks stall: that is what a
    // removed/lost playback device looks like from here.
    std::atomic<uint64_t> last_pull_ns{0};
    bool muted = false;
    // one chunk's scratch: the callback renders into this, never allocates
    float buf[kFrames * kChannels];
};

void SDLCALL OnGet(void* userdata, SDL_AudioStream* stream, int additional_amount, int /*total_amount*/) {
    auto* o = static_cast<Output*>(userdata);
    o->last_pull_ns.store(SDL_GetTicksNS(), std::memory_order_relaxed);
    if (additional_amount <= 0) return;
    int chunks = (additional_amount + kChunkBytes - 1) / kChunkBytes;
    if (chunks > kMaxChunksPerCallback) chunks = kMaxChunksPerCallback;
    for (int c = 0; c < chunks; c++) {
        o->render(o->buf, kFrames);
        g_rendered.fetch_add(1, std::memory_order_relaxed);
        float peak = 0.0f;
        for (uint32_t i = 0; i < kFrames * kChannels; i++) peak = std::max(peak, std::fabs(o->buf[i]));
        float prev = g_peak.load(std::memory_order_relaxed);
        while (peak > prev && !g_peak.compare_exchange_weak(prev, peak, std::memory_order_relaxed)) {
        }
        if (!SDL_PutAudioStreamData(stream, o->buf, kChunkBytes)) {
            LOGE("audio: SDL_PutAudioStreamData failed: %s", SDL_GetError());
            return;
        }
        uint64_t sub = g_submitted.fetch_add(1, std::memory_order_relaxed) + 1;
        if (sub % 500 == 0) {
            float pk = g_peak.exchange(0.0f, std::memory_order_relaxed);
            // no SDL_GetAudioStreamQueued here: the stream lock is already
            // held in this callback, so asking would re-enter it
            LOGI("audio: sdl submitted=%llu rendered=%llu requested=%d peak=%.4f", (unsigned long long)sub,
                 (unsigned long long)g_rendered.load(std::memory_order_relaxed), additional_amount, pk);
        }
    }
}

bool OpenStream(Output* o) {
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = int(kChannels);
    spec.freq = int(kRate);
    // SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK: SDL migrates this logical device
    // when the system default changes; the watcher below handles the cases
    // migration cannot (device removed with nowhere to go, stalled device).
    SDL_AudioStream* stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, OnGet, o);
    if (!stream) {
        LOGE("audio: SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
        return false;
    }
    o->stream = stream;
    o->device = SDL_GetAudioStreamDevice(stream);
    // XBE_MUTE silences the output for automated runs on the owner's desktop.
    // It turns the stream gain down and nothing else: the mixer still renders
    // every voice, so the guest's sound state and timing are exactly what
    // they are in a normal run. Unset, the game is audible - that is the
    // shipping behaviour and this must never default on.
    if (o->muted && !SDL_SetAudioStreamGain(stream, 0.0f))
        LOGW("audio: SDL_SetAudioStreamGain(0) failed: %s", SDL_GetError());
    o->last_pull_ns.store(SDL_GetTicksNS(), std::memory_order_relaxed);
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        LOGE("audio: SDL_ResumeAudioStreamDevice failed: %s", SDL_GetError());
        SDL_DestroyAudioStream(stream);
        o->stream = nullptr;
        o->device = 0;
        return false;
    }
    const char* name = o->device ? SDL_GetAudioDeviceName(o->device) : nullptr;
    LOGI("audio: sdl stream ok, device '%s' %u Hz %u ch F32", name ? name : "default", kRate, kChannels);
    if (o->muted) LOGI("audio: XBE_MUTE, output silenced");
    return true;
}

void CloseStream(Output* o) {
    if (!o->stream) return;
    // Detach the callback first: SDL_SetAudioStreamGetCallback takes the
    // stream lock, so an in-flight OnGet finishes before we destroy, and no
    // new one starts. (Destroying a stream from inside its own callback is
    // forbidden; this runs on the watcher thread, never the callback.)
    SDL_SetAudioStreamGetCallback(o->stream, nullptr, nullptr);
    SDL_PauseAudioStreamDevice(o->stream);
    SDL_DestroyAudioStream(o->stream);
    o->stream = nullptr;
    o->device = 0;
}

// Detached for the process lifetime, like the XAudio2 thread: owns the
// stream, retries a failed open (a machine may gain a device later), and
// reopens on device loss. SDL_EVENT_AUDIO_DEVICE_REMOVED is owned by the
// app layer's event pump, so nothing is pulled from the event queue here;
// instead a dead stream is recognised by its pull stopping (below). A
// default-playback stream is migrated by SDL itself when the system
// default changes, so the stall path only fires when there is nowhere
// for it to migrate to.
[[noreturn]] void Watch(Output* o) {
    bool audio_init = false;
    bool init_logged = false;
    for (;;) {
        if (!audio_init) {
            // Refcounted: safe alongside the app layer's own SDL init.
            if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
                audio_init = true;
            } else {
                if (!init_logged) {
                    LOGE("audio: SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s", SDL_GetError());
                    init_logged = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
        }
        if (!o->stream && !OpenStream(o)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // A removed device that SDL could not migrate away from, or a wedged
        // driver, stops pulling: no get-callback for a second while the
        // stream is supposed to be playing means the stream is dead.
        bool reopen = false;
        if (o->stream && !reopen) {
            uint64_t last = o->last_pull_ns.load(std::memory_order_relaxed);
            if (last && SDL_GetTicksNS() - last > 1'000'000'000ull) {
                LOGW("audio: no pull from SDL for 1s, reopening stream");
                reopen = true;
            }
        }
        if (reopen) {
            CloseStream(o);
            OpenStream(o);  // failure is retried at the top of the loop
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

}  // namespace

void StartOutput(std::function<void(float*, uint32_t)> render) {
    // dsound guards its own start, but a second stream/callback pair would
    // double-pull the one mixer; keep exactly one output per process.
    if (g_started.exchange(true, std::memory_order_acq_rel)) return;
    auto* o = new Output();  // process-lifetime, owned by the watcher thread
    o->render = std::move(render);
    if (const char* m = std::getenv("XBE_MUTE"); m && m[0] && std::strcmp(m, "0") != 0) o->muted = true;
    std::thread(Watch, o).detach();
}

}  // namespace audio

#endif  // !defined(_WIN32)

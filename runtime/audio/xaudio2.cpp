// mixer output through XAudio2
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>

#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <algorithm>
#include <atomic>
#include <cmath>
#include "audio/audio.h"
#include "core/log.h"

namespace audio {

// Host-side proof counters: what actually reached XAudio2, and the mixed
// output's peak since the last periodic log line.
static std::atomic<uint64_t> g_submitted{0}, g_rendered{0};
static std::atomic<float> g_peak{0.0f};

void StartOutput(std::function<void(float*, uint32_t)> render) {
    std::thread([render] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IXAudio2* xa = nullptr;
        IXAudio2MasteringVoice* master = nullptr;
        IXAudio2SourceVoice* voice = nullptr;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8;
        wf.nAvgBytesPerSec = kRate * 8;
        if (FAILED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(xa->CreateMasteringVoice(&master)) ||
            FAILED(xa->CreateSourceVoice(&voice, &wf))) {
            LOGE("audio: XAudio2 unavailable, no sound");
            return;
        }
        LOGI("audio: mastering voice ok, source voice %u Hz %u ch IEEE float", kRate, 2u);
        // XBE_MUTE silences the output for automated runs on the owner's desktop. It turns the
        // mastering voice down and nothing else: the mixer still renders every voice, so the
        // guest's sound state and timing are exactly what they are in a normal run, and the
        // Windows mixer is never touched. Unset, the game is audible - that is the shipping
        // behaviour and this must never default on.
        if (const char* m = std::getenv("XBE_MUTE"); m && m[0] && std::strcmp(m, "0") != 0) {
            master->SetVolume(0.0f);
            LOGI("audio: XBE_MUTE, output silenced");
        }
        voice->Start();
        constexpr uint32_t kFrames = kRate / 100, kSlots = 4;
        std::vector<float> bufs(kSlots * kFrames * 2);
        for (uint32_t n = 0;;) {
            XAUDIO2_VOICE_STATE st;
            voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (st.BuffersQueued >= 3) {
                Sleep(2);
                continue;
            }
            float* b = &bufs[(n % kSlots) * kFrames * 2];
            render(b, kFrames);
            g_rendered.fetch_add(1, std::memory_order_relaxed);
            float peak = 0.0f;
            for (uint32_t i = 0; i < kFrames * 2; i++) peak = std::max(peak, std::fabs(b[i]));
            float prev = g_peak.load(std::memory_order_relaxed);
            while (peak > prev && !g_peak.compare_exchange_weak(prev, peak, std::memory_order_relaxed)) {
            }
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = kFrames * 8;
            xb.pAudioData = reinterpret_cast<const BYTE*>(b);
            voice->SubmitSourceBuffer(&xb);
            uint64_t sub = g_submitted.fetch_add(1, std::memory_order_relaxed) + 1;
            if (sub % 500 == 0) {
                float pk = g_peak.exchange(0.0f, std::memory_order_relaxed);
                LOGI("audio: xaudio2 submitted=%llu rendered=%llu queued=%u peak=%.4f", (unsigned long long)sub,
                     (unsigned long long)g_rendered.load(std::memory_order_relaxed), st.BuffersQueued, pk);
            }
            n++;  // only a submitted buffer takes a slot, XAudio2 still reads the queued ones
        }
    }).detach();
}

}  // namespace audio

// A title may relaunch itself between the front end and a level (a quick reboot on the Xbox, a new process here).
// The old instance keeps its window up in kernel::Relaunch until this one has presented its first frames, so the
// kept picture stays on screen through the switch. Here the window is created hidden and opens at that point.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#endif

#include <atomic>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace app {

namespace {
#if defined(_WIN32)
HANDLE g_ready = nullptr;
#else
// Linux: the named event becomes an abstract-namespace AF_UNIX socket,
// named with the same key kernel::HandoverEvent gives Windows
// ("Local\<GameName>Handover-<new process pid>", narrowed). The old
// instance binds and listens at that name before starting this one and
// waits up to 15 s (kernel::Relaunch's bound) for the single byte
// FinishHandover sends; an abstract name dies with the old process, so
// no stale peer can outlive it as a leftover file or semaphore could.
int g_handover_fd = -1;

// wchar_t is one UTF-32 code unit on Linux; the event name is plain text.
std::string Narrow(const std::wstring& wide) {
    std::string out;
    for (char32_t c : wide) {
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
#endif
std::atomic<bool> g_handing{false};
}  // namespace

bool StartHandover() {
#if defined(_WIN32)
    g_ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, kernel::HandoverEvent(GetCurrentProcessId()).c_str());
    g_handing = g_ready != nullptr;
    if (g_handing) LOGI("handover: the previous instance waits for the first frames");
    return g_handing;
#else
    // Connect to the peer the previous instance left listening. No peer
    // (ECONNREFUSED / no listener at our name) is the ordinary first
    // launch: carry on alone, exactly as a failed OpenEventW does.
    std::string name = Narrow(kernel::HandoverEvent(static_cast<unsigned long>(getpid())));
    if (name.size() + 1 >= sizeof(sockaddr_un{}.sun_path)) {
        LOGW("handover: socket name too long (%zu bytes), carrying on alone", name.size());
        return false;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOGW("handover: socket failed (%s), carrying on alone", std::strerror(errno));
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';  // abstract namespace: the name is not a file
    std::memcpy(addr.sun_path + 1, name.data(), name.size());
    socklen_t len = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), len) != 0) {
        if (errno != ECONNREFUSED && errno != ENOENT)
            LOGW("handover: connect failed (%s), carrying on alone", std::strerror(errno));
        close(fd);
        return false;
    }
    g_handover_fd = fd;
    g_handing = true;
    LOGI("handover: the previous instance waits for the first frames");
    return g_handing;
#endif
}

bool HandingOver() { return g_handing; }

void FinishHandover() {
    if (!g_handing.exchange(false)) return;
    OpenGameWindow(false);
#if defined(_WIN32)
    SetEvent(g_ready);
    CloseHandle(g_ready);
#else
    if (g_handover_fd >= 0) {
        // One byte is the whole signal; the old instance then exits.
        const uint8_t ready = 1;
        const uint8_t* p = &ready;
        size_t left = 1;
        while (left) {
            ssize_t n = write(g_handover_fd, p, left);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                LOGW("handover: signal failed (%s)", n < 0 ? std::strerror(errno) : "closed");
                break;
            }
            p += n;
            left -= size_t(n);
        }
        close(g_handover_fd);
        g_handover_fd = -1;
    }
#endif
}

}  // namespace app

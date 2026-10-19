#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <fstream>

#include "app/app.h"
#include "kernel/kernel.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/syscall.h>  // SYS_pidfd_open: glibc 2.44's <sys/pidfd.h> declares pidfd_open with C++
                          // linkage under clang, so the wrapper does not resolve against libc
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif
#include "xbe/xbe.h"

namespace dispatch {
void Init();
uint32_t KernelThunkAddress(uint32_t ordinal);
}  // namespace dispatch

namespace xnet {
void CloseSockets();  // xdk/xnet.cpp
}

namespace kernel {

uint8_t* g_base = nullptr;
std::string g_launch_file;

namespace {
uint32_t g_saved_data = 0;
bool g_mark = false, g_kept_marked = false;
std::string g_picture, g_kept_picture;  // the last picture the title showed, raw path string with its terminator
constexpr uint32_t kPersistMagic = 0x50535044;  // "DPSP"

std::string PersistFile() { return g_launch_file.substr(0, g_launch_file.find_last_of("/\\") + 1) + "display.bin"; }

// surface header (Common, Data, Lock, Format, Size) then the pixels at +0x40, as the XDK lays out its persisted copy
void LoadPersistedDisplay() {
    std::ifstream f(PersistFile(), std::ios::binary);
    uint32_t head[7] = {};
    if (!f.read(reinterpret_cast<char*>(head), sizeof head) || head[0] != kPersistMagic || !head[4] || head[4] > 0x800000 ||
        head[6] > 1040)
        return;
    uint32_t block = mem::AllocContiguous(0x40 + head[4], 0, 0xFFFFFFFF, 0x1000);
    if (!block) return;
    if (!f.read(static_cast<char*>(HostPtr(block + 0x40)), head[4])) {
        mem::FreeContiguous(block);  // review note: a short file must not leak the block
        return;
    }
    g_kept_picture.resize(head[6]);
    if (!f.read(g_kept_picture.data(), head[6])) g_kept_picture.clear();
    W32(block, (head[1] & 0xFFFF0000) | 1);
    W32(block + 4, mem::PhysicalFromVirtual(block + 0x40));
    W32(block + 8, 0);
    W32(block + 0xC, head[2]);
    W32(block + 0x10, head[3]);
    g_saved_data = block;
    g_kept_marked = head[5] != 0;
    LOGI("boot: display kept from the previous run, %u bytes%s", head[4], g_kept_marked ? ", marked" : "");
}
}  // namespace

void SavePersistedDisplay(uint32_t common, uint32_t format, uint32_t size, const void* pixels, uint32_t bytes) {
    if (FILE* f = std::fopen(PersistFile().c_str(), "wb")) {
        // review note: the loader rejects a picture over 1040 bytes along with the whole file, so an
        // oversize path is dropped here and the pixels are still kept
        uint32_t picture = g_picture.size() <= 1040 ? uint32_t(g_picture.size()) : 0;
        uint32_t head[7] = {kPersistMagic, common, format, size, bytes, g_mark, picture};
        std::fwrite(head, sizeof head, 1, f);
        std::fwrite(pixels, 1, bytes, f);
        std::fwrite(g_picture.data(), 1, picture, f);
        std::fclose(f);
    }
}

void MarkPersistedDisplay() { g_mark = true; }
bool KeptDisplayMarked() { return g_kept_marked; }
void SetShownPicture(std::string path) { g_picture = std::move(path); }
const std::string& KeptPicture() { return g_kept_picture; }

uint32_t SavedDataAddress() { return g_saved_data; }
void SetSavedDataAddress(uint32_t surface) { g_saved_data = surface; }
const xbe::Image* g_image = nullptr;
static xbe::Image g_xbe;

bool Boot(std::vector<uint8_t> xbe, const std::string& xbe_name, const std::string& game_dir, const std::string& data_dir) {
    if (!mem::Init()) return false;
    g_base = mem::Base();

    std::string err;
    if (!g_xbe.Parse(std::move(xbe), err)) {
        LOGE("boot: %s: %s", xbe_name.c_str(), err.c_str());
        return false;
    }
    g_image = &g_xbe;
    const xbe::Image& img = g_xbe;
    if (img.entry != X86EntryPoint) {
        LOGE("boot: %s entry %08X does not match the recompiled entry %08X, wrong executable", xbe_name.c_str(), img.entry,
             X86EntryPoint);
        return false;
    }
    LOGI("boot: title %08X, entry %08X, %zu sections", img.title_id, img.entry, img.sections.size());
    for (auto& l : img.libraries) LOGI("boot: library %-8s %u.%u.%u", l.name.c_str(), l.major, l.minor, l.build);

    mem::ReserveImage(img.base, img.image_size);
    std::memcpy(g_base + img.base, img.file.data(), std::min<size_t>(img.header_size, img.file.size()));
    // XBE header 0x120 points at the section headers, 56 bytes each, +0x18 is the reference count that marks one loaded
    uint32_t sec_hdr = img.U32(0x120);
    for (size_t i = 0; i < img.sections.size(); i++) {
        auto& s = img.sections[i];
        if (!(s.flags & xbe::kPreload)) continue;
        std::memcpy(g_base + s.va, img.file.data() + s.raw, std::min(s.rsize, s.vsize));
        W32(sec_hdr + uint32_t(i) * 56 + 0x18, 1);
    }
    if (app::kGame.loaded) app::kGame.loaded();

    InitKernelData();
    dispatch::Init();

    uint32_t missing = 0;
    for (uint32_t slot = img.kernel_thunk;; slot += 4) {
        uint32_t v = R32(slot);
        if (!v) break;
        uint32_t ord = v & 0x7FFFFFFF;  // high bit flags an import by ordinal
        uint32_t data = DataAddress(ord);
        W32(slot, data ? data : dispatch::KernelThunkAddress(ord));
        if (!HasExport(ord)) {
            LOGW("boot: kernel import %u (%s) has no implementation", ord, ExportName(ord));
            missing++;
        }
    }
    LOGI("boot: kernel imports resolved, %u missing", missing);

    MountDefaults(game_dir, data_dir);

    // a quick reboot into this title left its launch data page behind
    g_launch_file = data_dir + "/launch.bin";
    std::ifstream lf(g_launch_file, std::ios::binary);
    if (lf) {
        uint32_t page = mem::AllocContiguous(0x1000, 0, 0xFFFFFFFF, 0x1000);
        lf.read(static_cast<char*>(HostPtr(page)), 0x1000);
        lf.close();
        std::remove(g_launch_file.c_str());
        W32(g_launch_data_page_va, page);
        LOGI("boot: launch data from the previous run, type %u", R32(page));
        LoadPersistedDisplay();
    }
    std::remove(PersistFile().c_str());

    uint32_t tls = 0;
    if (img.tls_addr) tls = ((img.tls.data_end - img.tls.data_start + img.tls.zero_fill + 15) & ~15u) + 4;
    ThreadRef t = CreateThread(std::max<uint32_t>(img.stack_commit, 0x10000), tls, 0, img.entry, 0, false);
    StartThread(t);
    return true;
}

const char* ImageExe(std::string xbe_path) {
    for (char& c : xbe_path) c = char(std::tolower(uint8_t(c)));
    for (auto& [xbe, exe] : app::kGame.images) {
        size_t n = std::strlen(xbe);
        if (xbe_path.size() >= n && xbe_path.compare(xbe_path.size() - n, n, xbe) == 0 &&
            (xbe_path.size() == n || xbe_path[xbe_path.size() - n - 1] == '\\' || xbe_path[xbe_path.size() - n - 1] == ';'))
            return exe;
    }
    return nullptr;
}

std::wstring HandoverEvent(unsigned long process) {
    return L"Local\\" + std::wstring(app::kGame.name) + L"Handover-" + std::to_wstring(process);
}

void Relaunch(const char* exe) {
    logx::Flush();
#ifdef _WIN32
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring path = self;
    path = path.substr(0, path.find_last_of(L'\\') + 1) + std::wstring(exe, exe + std::strlen(exe)) + L".exe";
    // the command line keeps its arguments (--instance) behind the new executable
    std::wstring line = GetCommandLineW();
    size_t args = line[0] == L'"' ? line.find(L'"', 1) + 1 : line.find(L' ');
    line = L"\"" + path + L"\"" + (args == std::wstring::npos ? L"" : line.substr(args));
    if (line.find(L"--relaunched") == std::wstring::npos) line += L" --relaunched";  // no boot splash again
    // this process lives on through the handover: the new one binds the same ports
    xnet::CloseSockets();
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(path.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        // this window stays up until the new instance shows its first frames (HandoverEvent), so no gap shows
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, HandoverEvent(pi.dwProcessId).c_str());
        AllowSetForegroundWindow(pi.dwProcessId);
        g_relaunching = true;
        ResumeThread(pi.hThread);
        HANDLE wait[] = {ready, pi.hProcess};
        if (ready) WaitForMultipleObjects(2, wait, FALSE, 15000);
        if (ready) ::CloseHandle(ready);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    } else {
        LOGE("relaunch: cannot start %s.exe (%lu)", exe, GetLastError());
    }
    std::_Exit(0);
#else
    char self[4096] = {};
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) {
        LOGE("relaunch: cannot find own executable (%s)", std::strerror(errno));
        std::_Exit(0);
    }
    std::string path = std::string(self, size_t(n));
    path = path.substr(0, path.find_last_of('/') + 1) + exe;

    // The new instance's argument vector: this one's, read back from
    // /proc/self/cmdline (the host's argv lives in app's translation unit),
    // re-pointed at the new executable and carrying --relaunched.
    std::vector<std::string> args;
    if (FILE* f = std::fopen("/proc/self/cmdline", "rb")) {
        std::string cmd;
        char buf[1024];
        size_t got;
        while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) cmd.append(buf, got);
        std::fclose(f);
        for (size_t at = 0; at < cmd.size();) {
            size_t end = cmd.find('\0', at);
            if (end == std::string::npos) end = cmd.size();
            args.emplace_back(cmd, at, end - at);
            at = end + 1;
        }
    }
    if (args.empty()) args.push_back(path);
    else args[0] = path;
    bool relaunched = false;
    for (const std::string& a : args) relaunched |= a.find("--relaunched") != std::string::npos;
    if (!relaunched) args.emplace_back("--relaunched");  // no boot splash again
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    // This process lives on through the handover: the new one binds the same ports.
    xnet::CloseSockets();

    // The handover socket is named for the child's pid, which only exists
    // after fork(); the child therefore waits on a pipe while the parent
    // binds and listens at that name, and is released only once it can
    // connect. Closing the pipe instead releases it just the same (it
    // execs on EOF too), so no failure below can leave it blocked.
    int sync_pipe[2] = {-1, -1};
    if (pipe2(sync_pipe, O_CLOEXEC) != 0) {
        LOGE("relaunch: cannot start %s (%s)", exe, std::strerror(errno));
        std::_Exit(0);
    }
    pid_t child = fork();
    if (child < 0) {
        LOGE("relaunch: cannot start %s (%s)", exe, std::strerror(errno));
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        std::_Exit(0);
    }
    if (child == 0) {
        // Only async-signal-safe calls between fork and exec: no allocation,
        // no logging. Closing our write end first is what lets the read
        // return at all if the parent dies without releasing us.
        close(sync_pipe[1]);
        char byte;
        ssize_t r;
        do {
            r = read(sync_pipe[0], &byte, 1);
        } while (r < 0 && errno == EINTR);
        close(sync_pipe[0]);
        execv(path.c_str(), argv.data());
        _Exit(127);
    }
    close(sync_pipe[0]);
    sync_pipe[0] = -1;

    // The abstract socket name: HandoverEvent(child) narrowed the way
    // handover.cpp narrows it (wchar_t is one UTF-32 code unit here).
    std::string name;
    for (char32_t c : HandoverEvent(static_cast<unsigned long>(child))) {
        if (c < 0x80) {
            name += char(c);
        } else if (c < 0x800) {
            name += char(0xC0 | (c >> 6));
            name += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            name += char(0xE0 | (c >> 12));
            name += char(0x80 | ((c >> 6) & 0x3F));
            name += char(0x80 | (c & 0x3F));
        } else {
            name += char(0xF0 | (c >> 18));
            name += char(0x80 | ((c >> 12) & 0x3F));
            name += char(0x80 | ((c >> 6) & 0x3F));
            name += char(0x80 | (c & 0x3F));
        }
    }

    int listener = -1, pidfd = -1;
    bool listening = false;
    if (name.size() + 1 >= sizeof(sockaddr_un{}.sun_path)) {
        LOGE("relaunch: handover socket name too long (%zu bytes)", name.size());
    } else if ((listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) {
        LOGE("relaunch: handover socket failed (%s)", std::strerror(errno));
    } else {
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        addr.sun_path[0] = '\0';  // abstract namespace: the name is not a file
        std::memcpy(addr.sun_path + 1, name.data(), name.size());
        socklen_t len = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + name.size());
        if (bind(listener, reinterpret_cast<const sockaddr*>(&addr), len) != 0) {
            LOGE("relaunch: handover bind failed (%s)", std::strerror(errno));
        } else if (listen(listener, 1) != 0) {
            LOGE("relaunch: handover listen failed (%s)", std::strerror(errno));
        } else {
            listening = true;
            pidfd = int(::syscall(SYS_pidfd_open, child, 0u));
            if (pidfd < 0) LOGW("relaunch: pidfd failed (%s), child exit will not cut the wait short", std::strerror(errno));
        }
    }

    g_relaunching = true;
    // Release the child to execv. SIGPIPE is silenced for this one write: a
    // child that somehow died first must not take the old instance with it.
    {
        auto prev = std::signal(SIGPIPE, SIG_IGN);
        const char go = 1;
        const char* p = &go;
        size_t left = 1;
        while (left) {
            ssize_t w = write(sync_pipe[1], p, left);
            if (w < 0 && errno == EINTR) continue;
            break;  // success, EOF and error all end the release
        }
        std::signal(SIGPIPE, prev);
    }
    close(sync_pipe[1]);
    sync_pipe[1] = -1;

    bool reaped = false;
    if (listening) {
        // This window stays up until the new instance shows its first frames
        // (one byte on the accepted connection), the child dies, or 15 s pass
        // -- WaitForMultipleObjects(..., 15000) on {ready, process} parity.
        int conn = -1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        bool done = false;
        while (!done) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) break;
            int timeout_ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            pollfd fds[2] = {{conn >= 0 ? conn : listener, POLLIN, 0}, {pidfd, POLLIN, 0}};
            int nfds = pidfd >= 0 ? 2 : 1;
            int ready = poll(fds, nfds, timeout_ms);
            if (ready < 0) {
                if (errno == EINTR) continue;
                LOGW("relaunch: handover wait failed (%s)", std::strerror(errno));
                break;
            }
            if (ready == 0) break;  // the 15 s are up
            if (pidfd >= 0 && (fds[1].revents & (POLLIN | POLLERR | POLLHUP))) {
                // pidfd readable means the child is a zombie: reap it.
                reaped = waitpid(child, nullptr, 0) == child;
                break;
            }
            if (!(fds[0].revents & (POLLIN | POLLERR | POLLHUP))) continue;
            if (conn < 0) {
                conn = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
                if (conn < 0) {
                    if (errno == EINTR || errno == EAGAIN) continue;
                    LOGW("relaunch: handover accept failed (%s)", std::strerror(errno));
                    close(listener);
                    listener = -1;
                    continue;
                }
                // review note: the listener stays open until the byte arrives, so a peer that
                // closes without one does not end every chance of the real handover
                continue;  // now wait for its byte (or its death) instead
            }
            char byte;
            ssize_t r = read(conn, &byte, 1);
            if (r == 1) {
                done = true;  // first frames presented: hand over
            } else if (r == 0) {
                // Closed without a byte: FinishHandover only closes after
                // writing, so this is a peer that gave up; go back to the
                // listener for another, as an unset event would keep waiting.
                close(conn);
                conn = -1;
                continue;
            } else if (errno != EINTR) {
                close(conn);
                conn = -1;
                continue;
            }
        }
        if (conn >= 0) close(conn);
    }
    if (listener >= 0) close(listener);
    if (pidfd >= 0) close(pidfd);
    // A child that died during the wait is reaped above (or here, if it died
    // in the last instant); one still running is left to run -- exiting now
    // reparents it, exactly as closing the process handle does on Windows.
    if (!reaped) reaped = waitpid(child, nullptr, WNOHANG) == child;
    (void)reaped;
    std::_Exit(0);
#endif
}

void Shutdown(int code) {
    LOGR("Exit (%d)", code);
    logx::Flush();
    std::_Exit(code);
}

}  // namespace kernel

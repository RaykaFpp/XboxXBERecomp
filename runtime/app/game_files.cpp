// The game in assets: its files, or a disc image (.iso) with this executable's xbe on it. The xbe must be the one
// this executable was recompiled from; another release's would run its own data under this code.
// One implementation serves both platforms: all path and byte work goes through std::filesystem and
// std::ifstream, and kernel::Disc streams the image (pread/overlapped reads per entry), so a 2 GB .iso is
// never loaded whole -- only the xbe entry (a few MB) is read for the SHA-1 check. The only platform
// branch left is the modal prompt at the end, which has no portable form.
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
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"
#include "kernel/crypto.h"
#include "kernel/disc.h"

extern const char kImageSha1[];  // the xbe recompiled into this executable, written by CMake

namespace app {

namespace {

namespace fs = std::filesystem;

// Case folding for file names, on purpose rather than by accident: Windows compares paths
// case-insensitively and Linux does not, so an extracted game whose files landed as
// DEFAULT.XBE, Data/ or an .ISO image must be found the same way on both. The game disc's own
// file names are ASCII, so ASCII folding is the whole job.
std::string Fold(std::string s) {
    for (char& c : s) c = char(std::tolower(uint8_t(c)));
    return s;
}

bool FoldEqual(std::string_view a, std::string_view b) { return Fold(std::string(a)) == Fold(std::string(b)); }

// kGame's strings are wchar_t: UTF-16 on Windows, UTF-32 on Linux. They only ever reach the
// log and the terminal on Linux, so convert them to UTF-8 at that boundary; app.h's
// signatures stay exactly as they are.
std::string WideToUtf8(const wchar_t* wide) {
    if (!wide) return {};
    std::wstring in(wide);
    std::string out;
    for (size_t i = 0; i < in.size(); i++) {
        char32_t c = char32_t(in[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            // UTF-16: join a surrogate pair, replace a lone surrogate.
            uint32_t unit = uint32_t(uint16_t(in[i]));
            if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < in.size()) {
                uint32_t lo = uint32_t(uint16_t(in[i + 1]));
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    c = char32_t(0x10000 + ((unit - 0xD800) << 10) + (lo - 0xDC00));
                    i++;
                } else {
                    c = 0xFFFD;
                }
            } else if (unit >= 0xD800 && unit <= 0xDFFF) {
                c = 0xFFFD;
            }
        }
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = 0xFFFD;
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

// assets beside the exe or in any folder above it (the build output)
fs::path FindAssets() {
    auto dir = ExeDir();
    for (int i = 0; i < 6; i++) {
        std::error_code ec;
        if (fs::is_directory(dir / "assets", ec)) return dir / "assets";
        // On a case-sensitive file system the folder may be spelled Assets or ASSETS; on
        // Windows the check above has already matched it whatever its case.
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_directory(ec) && FoldEqual(e.path().filename().string(), "assets")) return e.path();
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return {};
}

// this executable's xbe among the extracted files, whatever case its name landed in
fs::path FindXbe(const fs::path& assets) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(assets, ec))
        if (e.is_regular_file(ec) && FoldEqual(e.path().filename().string(), IMAGE_XBE)) return e.path();
    return {};
}

std::string Sha1(const std::vector<uint8_t>& data) {
    alignas(8) uint8_t ctx[128];
    uint8_t digest[20];
    crypto::ShaInit(ctx);
    crypto::ShaUpdate(ctx, data.data(), data.size());
    crypto::ShaFinal(ctx, digest);
    std::string hex;
    for (uint8_t b : digest) {
        hex += "0123456789abcdef"[b >> 4];
        hex += "0123456789abcdef"[b & 15];
    }
    return hex;
}

// R13: an XBE is a few MB (this title's is under 8); a wrong or multi-GB file in its place is
// refused by size before anything is read or hashed
constexpr uintmax_t kMaxHostXbe = 256ull << 20;

bool ReadHostFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::error_code ec;
    uintmax_t size = fs::file_size(path, ec);
    if (ec) return false;
    if (size > kMaxHostXbe) {
        LOGE("game: %s is %llu bytes, over the %llu-byte limit for an XBE; ignoring it", path.string().c_str(),
             (unsigned long long)size, (unsigned long long)kMaxHostXbe);
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.resize(size_t(size));
    if (size && !f.read(reinterpret_cast<char*>(out.data()), std::streamsize(size))) {
        out.clear();
        return false;
    }
    return true;
}

// Host artefacts that may sit beside (or inside) an assets folder but are never game data:
// log and crash folders, the shader cache, the hard-disk image folder, settings and the boot
// splash. A folder is only a usable extracted game copy when, besides the XBE, it holds at
// least one real game data file; the XBE alone (plus any disc image or host file) is the
// normal state of a development tree, because the build needs assets/<xbe> there, and mounting
// such a folder as D: leaves every game data open to fail, which the title reports as a dirty
// or damaged disc.
bool IsHostArtefactDir(const fs::path& part) {
    std::string name = Fold(part.filename().string());
    return name == "logs" || name == "log" || name == "shadercache" || name == "shader_cache" ||
           name == "hdd" || name == "crashdumps" || name == "settings";
}

bool IsHostArtefactFile(const fs::path& path) {
    std::string name = Fold(path.filename().string());
    std::string ext = Fold(path.extension().string());
    if (ext == ".log" || ext == ".dmp") return true;
    if (kGame.settings && name == Fold(std::string(kGame.settings))) return true;
    if (name == "settings.toml") return true;
    std::string stem = Fold(path.stem().string());
    if (stem == "splash") return true;
    if (kGame.splash && stem == Fold(WideToUtf8(kGame.splash))) return true;
    return false;
}

// Real game data files in an extracted folder, counted recursively: every regular file that
// is not the XBE, not a disc image and not a host artefact (above). This is a count of what
// is actually on disk, never a list of names this particular game is expected to have.
uint64_t CountGameFiles(const fs::path& assets) {
    uint64_t count = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(assets, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const fs::directory_entry& e = *it;
        std::error_code ec2;
        if (e.is_directory(ec2)) {
            if (IsHostArtefactDir(e.path())) it.disable_recursion_pending();
            continue;
        }
        if (!e.is_regular_file(ec2)) continue;
        // Belt and braces for artefact folders the iterator was already inside. Folders only: the
        // file's own name is not a folder, so a data file named "log" or "settings" still counts.
        bool in_artefact = false;
        for (const auto& part : e.path().lexically_relative(assets).parent_path())
            if (IsHostArtefactDir(part)) in_artefact = true;
        if (in_artefact || IsHostArtefactFile(e.path())) continue;
        if (FoldEqual(e.path().filename().string(), IMAGE_XBE)) continue;
        if (Fold(e.path().extension().string()) == ".iso") continue;
        count++;
    }
    // the count is advisory, but a walk cut short by an I/O error undercounts; say so
    if (ec) LOGW("game: counting files in %s stopped early (%s)", assets.string().c_str(), ec.message().c_str());
    return count;
}

// Files held by a disc image, counted from its own directory tree (folders are not files).
uint64_t CountDiscFiles(const kernel::Disc::Entry* entry) {
    if (!entry) return 0;
    if (!entry->dir) return 1;
    uint64_t count = 0;
    for (const auto& child : entry->children) count += CountDiscFiles(&child);
    return count;
}


// The two prompts below are the only platform-specific code in this file: a modal box is a
// Win32 window on Windows. On Linux there is no window -- or even an SDL video subsystem --
// yet at the point main() calls FindGame, so the Linux branch initialises SDL video itself
// (main's later SDL_Init simply raises the refcount) and asks with SDL's message box; the
// log and stderr copy stays either way, which is where a headless start surfaces it.
void ShowMissingGame() {
#if defined(_WIN32)
    MessageBoxW(nullptr, kGame.missing, kGame.short_title, MB_ICONERROR);
#else
    std::string title = WideToUtf8(kGame.short_title), text = WideToUtf8(kGame.missing);
    LOGE("game: %s", text.c_str());
    std::fprintf(stderr, "%s: %s\n", title.c_str(), text.c_str());
    if (SDL_InitSubSystem(SDL_INIT_VIDEO))
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), text.c_str(), nullptr);
#endif
}

// true: start the different release anyway. Both boxes default to No (MB_DEFBUTTON2 on
// Windows, the escape/return-key button here), and when no box can be shown at all the
// Linux path keeps that safe answer rather than silently starting a release this build
// was not recompiled from.
bool ConfirmDifferentRelease(const fs::path& name) {
#if defined(_WIN32)
    std::wstring text = name.filename().wstring() +
                        L" is a different release of the game than this build was made from, and will most likely "
                        L"crash.\n\nStart it anyway?";
    return MessageBoxW(nullptr, text.c_str(), kGame.short_title, MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES;
#else
    std::string title = WideToUtf8(kGame.short_title);
    std::string text = name.filename().string() +
                       " is a different release of the game than this build was made from, and will most likely "
                       "crash.\n\nStart it anyway?";
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "No"},
            {0, 1, "Yes"}};
        const SDL_MessageBoxData data{SDL_MESSAGEBOX_WARNING, nullptr, title.c_str(), text.c_str(),
                                      int(std::size(buttons)), buttons, nullptr};
        int button = 0;
        if (SDL_ShowMessageBox(&data, &button)) return button == 1;
    }
    LOGE("game: %s Not starting it (no message box available: %s)", text.c_str(), SDL_GetError());
    std::fprintf(stderr, "%s: %s\n", title.c_str(), text.c_str());
    return false;
#endif
}

}  // namespace

bool FindGame(GameFiles& game) {
    fs::path assets = FindAssets();
    std::vector<fs::path> isos;
    std::error_code ec;
    if (!assets.empty())
        for (auto& e : fs::directory_iterator(assets, ec)) {
            if (Fold(e.path().extension().string()) == ".iso" && e.is_regular_file(ec)) isos.push_back(e.path());
        }
    std::sort(isos.begin(), isos.end());

    // The first copy with this xbe wins, a complete extracted folder before any image; an
    // incomplete extracted folder (the XBE and nothing else game-shaped) loses to a disc image
    // carrying the right XBE, because mounting it would leave the title with no data files.
    // Another release is kept to offer, exactly as before.
    GameFiles other;
    std::unique_ptr<kernel::Disc> other_disc;
    fs::path other_name;
    GameFiles incomplete;
    uint64_t incomplete_files = 0;
    bool have_incomplete = false;
    auto consider = [&](const fs::path& name, std::vector<uint8_t> xbe, std::unique_ptr<kernel::Disc> disc,
                        bool defer_match) {
        std::string sha = Sha1(xbe);
        bool match = sha == kImageSha1;
        LOGR("Game: %s in %s, sha1 %s%s", IMAGE_XBE, name.string().c_str(), sha.c_str(), match ? "" : ", another release");
        if (match) {
            if (defer_match) {
                incomplete = {assets, std::move(xbe)};
                have_incomplete = true;
                return false;
            }
            if (have_incomplete)
                LOGR("game: %s is incomplete: it holds %llu game data file(s) besides %s (disc images and host files not counted), so disc image %s is being used instead",
                     assets.string().c_str(), (unsigned long long)incomplete_files, IMAGE_XBE, name.string().c_str());
            game = {assets, std::move(xbe)};
            kernel::MountDisc(std::move(disc));
            return true;
        }
        if (other_name.empty()) {
            other = {assets, std::move(xbe)};
            other_disc = std::move(disc);
            other_name = name;
        }
        return false;
    };

    std::vector<uint8_t> xbe;
    fs::path host_xbe = assets.empty() ? fs::path{} : FindXbe(assets);
    uint64_t folder_files = host_xbe.empty() ? 0 : CountGameFiles(assets);
    bool folder_complete = !host_xbe.empty() && folder_files > 0;
    if (!host_xbe.empty()) incomplete_files = folder_files;
    if (!host_xbe.empty() && ReadHostFile(host_xbe, xbe) &&
        consider(assets, std::move(xbe), nullptr, !folder_complete))
        return true;
    for (const fs::path& iso : isos) {
        auto disc = std::make_unique<kernel::Disc>();
        std::string error;
        if (!disc->Open(iso, error)) {
            LOGW("game: %s: %s", iso.string().c_str(), error.c_str());
            continue;
        }
        LOGR("game: %s holds %llu file(s)", iso.string().c_str(),
             (unsigned long long)CountDiscFiles(disc->Find(std::vector<std::string>{})));
        if (!disc->Load(IMAGE_XBE, xbe)) {
            LOGW("game: %s has no %s", iso.string().c_str(), IMAGE_XBE);
            continue;
        }
        if (consider(iso, std::move(xbe), std::move(disc), false)) return true;
    }

    if (have_incomplete) {
        LOGR("game: using incomplete extracted folder %s anyway: it holds %llu game data file(s) besides %s (disc images and host files not counted) and no disc image with this %s was found",
             assets.string().c_str(), (unsigned long long)incomplete_files, IMAGE_XBE, IMAGE_XBE);
        LOGR("game: the game's data files are missing from %s, so the title will report a disc error (dirty or damaged disc)",
             assets.string().c_str());
        LOGR("game: fix: run python3 tools/xiso_extract.py extract <image> assets, or put the .iso in assets/ and leave it there");
        game = std::move(incomplete);
        kernel::MountDisc(nullptr);
        return true;
    }

    if (other_name.empty()) {
        ShowMissingGame();
        return false;
    }
    if (!ConfirmDifferentRelease(other_name)) return false;
    game = std::move(other);
    kernel::MountDisc(std::move(other_disc));
    return true;
}

}  // namespace app

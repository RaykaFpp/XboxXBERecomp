// A real PNG decoder for the Linux boot splash, on top of zlib's inflate.
// SDL3 alone loads BMP but not PNG and SDL3_image is not available, so the
// PNG beside the exe is decoded here. Windows uses WIC (app/splash.cpp) and
// this translation unit is empty there.
//
// Supported: bit depth 8; colour types 0 (greyscale), 2 (RGB), 3 (palette,
// with tRNS), 4 (greyscale + alpha) and 6 (RGBA); all five scanline filters
// (None, Sub, Up, Average, Paeth). Interlaced (Adam7) files and any other
// bit depth are rejected explicitly, logged, and the caller runs with no
// splash rather than a mis-decoded picture.
#if !defined(_WIN32)

#include "app/image_decode.h"

#include <zlib.h>

#include <cstdlib>
#include <cstring>
#include <fstream>

#include "core/log.h"

namespace app {

namespace {

// Splash pictures are a few hundred pixels across; these caps only exist so
// a corrupt or hostile file is rejected before anything big is allocated.
constexpr uint32_t kMaxSide = 16384;
constexpr uint64_t kMaxPixels = uint64_t{1} << 25;  // 33.5M pixels, 128 MiB as RGBA
constexpr uint64_t kMaxFileBytes = uint64_t{64} << 20;

uint32_t Be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint16_t Be16(const uint8_t* p) { return uint16_t((uint32_t(p[0]) << 8) | uint32_t(p[1])); }

uint8_t Paeth(uint8_t a, uint8_t b, uint8_t c) {
    int p = int(a) + int(b) - int(c);
    int pa = std::abs(p - int(a)), pb = std::abs(p - int(b)), pc = std::abs(p - int(c));
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

}  // namespace

bool DecodePng(const std::filesystem::path& path, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
    rgba.clear();
    width = height = 0;
    const std::string where = path.string();

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        LOGW("splash: %s could not be opened", where.c_str());
        return false;
    }
    std::streamoff size = in.tellg();
    if (size < 8 || uint64_t(size) > kMaxFileBytes) {
        LOGW("splash: %s: implausible file size %lld", where.c_str(), (long long)size);
        return false;
    }
    size_t file_bytes = size_t(size);
    std::vector<uint8_t> file(file_bytes);
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(file.data()), size)) {
        LOGW("splash: %s could not be read", where.c_str());
        return false;
    }

    static constexpr uint8_t kSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (std::memcmp(file.data(), kSignature, 8) != 0) {
        LOGW("splash: %s is not a PNG (bad signature)", where.c_str());
        return false;
    }

    uint32_t w = 0, h = 0;
    uint8_t bit_depth = 0, colour_type = 0, interlace = 0;
    bool have_ihdr = false, have_iend = false;
    std::vector<uint8_t> palette;   // 3 bytes per entry
    std::vector<uint8_t> trns;      // raw tRNS payload, interpreted per colour type below
    std::vector<uint8_t> idat;      // concatenated IDAT payloads

    size_t pos = 8;
    while (pos < file.size()) {
        if (file.size() - pos < 12) {
            LOGW("splash: %s: truncated chunk at %zu", where.c_str(), pos);
            return false;
        }
        uint32_t length = Be32(&file[pos]);
        const uint8_t* type = &file[pos + 4];
        if (uint64_t(length) + 12 > file.size() - pos) {
            LOGW("splash: %s: chunk %.4s length %u runs past the file", where.c_str(), (const char*)type, length);
            return false;
        }
        const uint8_t* data = &file[pos + 8];
        // Every chunk's CRC is checked before its payload is trusted.
        uLong crc = crc32(crc32(0L, Z_NULL, 0), type, 4);
        crc = crc32(crc, data, length);
        if (crc != Be32(&file[pos + 8 + length])) {
            LOGW("splash: %s: bad CRC in chunk %.4s", where.c_str(), (const char*)type);
            return false;
        }
        auto is = [&](const char* t) { return std::memcmp(type, t, 4) == 0; };

        if (!have_ihdr && !is("IHDR")) {
            LOGW("splash: %s: first chunk is %.4s, not IHDR", where.c_str(), (const char*)type);
            return false;
        }
        if (is("IHDR")) {
            if (have_ihdr || length != 13) {
                LOGW("splash: %s: malformed IHDR", where.c_str());
                return false;
            }
            have_ihdr = true;
            w = Be32(data);
            h = Be32(data + 4);
            bit_depth = data[8];
            colour_type = data[9];
            interlace = data[12];
            // Dimensions are validated before anything sized by them exists.
            if (!w || !h || w > kMaxSide || h > kMaxSide || uint64_t(w) * h > kMaxPixels) {
                LOGW("splash: %s: implausible dimensions %ux%u", where.c_str(), w, h);
                return false;
            }
            if (data[10] != 0 || data[11] != 0) {
                LOGW("splash: %s: unknown compression/filter method %u/%u", where.c_str(), data[10], data[11]);
                return false;
            }
            if (interlace != 0) {
                LOGW("splash: %s: interlaced PNG is not supported", where.c_str());
                return false;
            }
            if (bit_depth != 8) {
                LOGW("splash: %s: bit depth %u is not supported (8 only)", where.c_str(), bit_depth);
                return false;
            }
            if (colour_type != 0 && colour_type != 2 && colour_type != 3 && colour_type != 4 && colour_type != 6) {
                LOGW("splash: %s: colour type %u is not supported", where.c_str(), colour_type);
                return false;
            }
        } else if (is("PLTE")) {
            if (length % 3 != 0 || length == 0 || length > 3 * 256) {
                LOGW("splash: %s: malformed PLTE (%u bytes)", where.c_str(), length);
                return false;
            }
            palette.assign(data, data + length);
        } else if (is("tRNS")) {
            trns.assign(data, data + length);
        } else if (is("IDAT")) {
            idat.insert(idat.end(), data, data + length);
        } else if (is("IEND")) {
            have_iend = true;
            break;
        } else if (type[0] >= 'A' && type[0] <= 'Z') {
            // An unknown critical chunk must not be silently skipped.
            LOGW("splash: %s: unsupported critical chunk %.4s", where.c_str(), (const char*)type);
            return false;
        }
        pos += 12 + length;
    }
    if (!have_ihdr || !have_iend || idat.empty()) {
        LOGW("splash: %s: missing IHDR, IDAT or IEND", where.c_str());
        return false;
    }
    if (colour_type == 3 && palette.empty()) {
        LOGW("splash: %s: palette image without PLTE", where.c_str());
        return false;
    }

    const uint32_t channels = colour_type == 0 || colour_type == 3 ? 1 : colour_type == 4 ? 2 : colour_type == 2 ? 3 : 4;
    const uint64_t row_bytes = uint64_t(w) * channels;  // 8-bit: one byte per sample
    const uint64_t raw_size = uint64_t(h) * (row_bytes + 1);
    size_t raw_bytes = size_t(raw_size);
    std::vector<uint8_t> raw(raw_bytes);

    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) {
        LOGW("splash: %s: inflateInit failed", where.c_str());
        return false;
    }
    zs.next_in = idat.data();
    zs.avail_in = uInt(idat.size());
    zs.next_out = raw.data();
    zs.avail_out = uInt(raw.size());
    int zret = inflate(&zs, Z_FINISH);
    uint64_t produced = zs.total_out;
    inflateEnd(&zs);
    if (zret != Z_STREAM_END || produced != raw_size) {
        LOGW("splash: %s: corrupt deflate stream (%d, %llu of %llu bytes)", where.c_str(), zret,
             (unsigned long long)produced, (unsigned long long)raw_size);
        return false;
    }

    // Undo the per-scanline filters in place. bpp is the filter's pixel unit:
    // whole samples for truecolour/alpha, single bytes for grey and indices.
    const uint32_t bpp = channels;
    const size_t stride = size_t(row_bytes) + 1;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t* row = raw.data() + size_t(y) * stride;
        uint8_t filter = row[0];
        uint8_t* px = row + 1;
        const uint8_t* prev = y ? raw.data() + size_t(y - 1) * stride + 1 : nullptr;
        switch (filter) {
            case 0: break;
            case 1:
                for (uint64_t x = bpp; x < row_bytes; x++) px[x] = uint8_t(px[x] + px[x - bpp]);
                break;
            case 2:
                if (prev) for (uint64_t x = 0; x < row_bytes; x++) px[x] = uint8_t(px[x] + prev[x]);
                break;
            case 3:
                for (uint64_t x = 0; x < row_bytes; x++) {
                    int left = x >= bpp ? px[x - bpp] : 0;
                    int up = prev ? prev[x] : 0;
                    px[x] = uint8_t(px[x] + (left + up) / 2);
                }
                break;
            case 4:
                for (uint64_t x = 0; x < row_bytes; x++) {
                    uint8_t left = x >= bpp ? px[x - bpp] : 0;
                    uint8_t up = prev ? prev[x] : 0;
                    uint8_t up_left = prev && x >= bpp ? prev[x - bpp] : 0;
                    px[x] = uint8_t(px[x] + Paeth(left, up, up_left));
                }
                break;
            default:
                LOGW("splash: %s: unknown filter %u on row %u", where.c_str(), filter, y);
                return false;
        }
    }

    // tRNS transparency for the colour types that carry no alpha channel.
    uint16_t trns_grey = 0, trns_r = 0, trns_g = 0, trns_b = 0;
    bool trns_simple = false;
    if (colour_type == 0 && trns.size() >= 2) {
        trns_grey = Be16(trns.data());
        trns_simple = true;
    } else if (colour_type == 2 && trns.size() >= 6) {
        trns_r = Be16(trns.data());
        trns_g = Be16(trns.data() + 2);
        trns_b = Be16(trns.data() + 4);
        trns_simple = true;
    }

    rgba.resize(size_t(w) * h * 4);
    const size_t entries = palette.size() / 3;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* px = raw.data() + size_t(y) * stride + 1;
        uint8_t* out = rgba.data() + size_t(y) * w * 4;
        for (uint32_t x = 0; x < w; x++, out += 4) {
            switch (colour_type) {
                case 0: {
                    uint8_t g = px[x];
                    out[0] = out[1] = out[2] = g;
                    out[3] = trns_simple && g == trns_grey ? 0 : 255;
                    break;
                }
                case 2: {
                    const uint8_t* s = px + size_t(x) * 3;
                    out[0] = s[0];
                    out[1] = s[1];
                    out[2] = s[2];
                    out[3] = trns_simple && s[0] == trns_r && s[1] == trns_g && s[2] == trns_b ? 0 : 255;
                    break;
                }
                case 3: {
                    uint8_t idx = px[x];
                    if (idx >= entries) {
                        LOGW("splash: %s: palette index %u out of range", where.c_str(), idx);
                        return false;
                    }
                    out[0] = palette[size_t(idx) * 3];
                    out[1] = palette[size_t(idx) * 3 + 1];
                    out[2] = palette[size_t(idx) * 3 + 2];
                    out[3] = idx < trns.size() ? trns[idx] : 255;
                    break;
                }
                case 4: {
                    const uint8_t* s = px + size_t(x) * 2;
                    out[0] = out[1] = out[2] = s[0];
                    out[3] = s[1];
                    break;
                }
                case 6: {
                    const uint8_t* s = px + size_t(x) * 4;
                    out[0] = s[0];
                    out[1] = s[1];
                    out[2] = s[2];
                    out[3] = s[3];
                    break;
                }
            }
        }
    }

    width = w;
    height = h;
    return true;
}

}  // namespace app

#endif  // !defined(_WIN32)

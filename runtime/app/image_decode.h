#pragma once
// PNG decoding for the Linux boot splash (app/splash.cpp). Windows decodes
// through WIC and never needs this; the whole decoder therefore exists only
// in the non-Windows build, where splash.cpp links zlib for inflate.
#if !defined(_WIN32)

#include <cstdint>
#include <filesystem>
#include <vector>

namespace app {

// Decodes the splash PNG at `path` to straight (non-premultiplied) RGBA8.
// Supported subset: bit depth 8, colour types 0 (greyscale), 2 (RGB),
// 3 (palette, with tRNS), 4 (greyscale+alpha) and 6 (RGBA), all five filters,
// non-interlaced only. Anything else is rejected with a logged reason and
// false is returned (the caller then falls back to no splash).
bool DecodePng(const std::filesystem::path& path, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);

}  // namespace app

#endif  // !defined(_WIN32)

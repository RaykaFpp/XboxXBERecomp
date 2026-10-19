#pragma once
// DXGI_FORMAT off Windows. The vertex and texture decoders name their results in DXGI terms because the
// Direct3D 12 backend consumes them directly; the Vulkan backend maps the same values to VkFormat. Only the
// formats the runtime actually produces are listed, with their real DXGI numbering, so a value is the same
// integer on both platforms and a capture taken on one reads correctly on the other.
#if defined(_WIN32)
#include <dxgiformat.h>
#else
enum DXGI_FORMAT : unsigned {
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
    DXGI_FORMAT_R32G32B32_FLOAT = 6,
    DXGI_FORMAT_R16G16B16A16_SNORM = 13,
    DXGI_FORMAT_R16G16B16A16_SINT = 14,
    DXGI_FORMAT_R32G32_FLOAT = 16,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R16G16_SNORM = 37,
    DXGI_FORMAT_R16G16_SINT = 38,
    DXGI_FORMAT_R32_FLOAT = 41,
    DXGI_FORMAT_R32_UINT = 42,
    DXGI_FORMAT_R8G8_UNORM = 49,
    DXGI_FORMAT_R16_SNORM = 58,
    DXGI_FORMAT_R16_SINT = 59,
    DXGI_FORMAT_R8_UNORM = 61,
    DXGI_FORMAT_B8G8R8A8_UNORM = 87,
};
#endif

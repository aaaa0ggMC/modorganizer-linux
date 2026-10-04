#pragma once
// DDS 贴图：解码（BC1–BC5、BC7 全 8 种模式、RGBA8/BGRA8）、缩放、生成 mip、编码（BC1/2/3/4/5 简单的包围盒算法，BC7 只用 mode 6）。
// 用于 Wabbajack 的 TransformedTexture。质量目标是「游戏里看起来对」，输出与作者用 DirectXTex 的结果字节不同。
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mol/error.hpp"

namespace mol::dds {

struct Image {
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba;  // w*h*4
};

enum class Format { BC1, BC2, BC3, BC4, BC5, BC7, RGBA8, BGRA8 };

// "BC7_UNORM"、"BC1_UNORM_SRGB"、"R8G8B8A8_UNORM"…（DXGI 名）；不认识 → Error{invalid_argument}。
Format format_from_name(std::string_view dxgi_name);

// 解出顶层 mip 的 RGBA。不支持的格式/损坏 → Error{invalid_argument}。
Image decode(std::string_view dds_bytes);
// 面积平均缩放到 w×h（放大时退化为最近邻式）。
Image resize(const Image& src, int w, int h);
// 编码成 DDS 文件字节。mips<=0 → 生成完整 mip 链；mips>=1 → 精确这么多层。
std::string encode(const Image& img, Format fmt, int mips);

}  // namespace mol::dds

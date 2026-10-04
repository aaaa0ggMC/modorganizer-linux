#include <cmath>
#include <cstring>

#include "minitest.hpp"
#include "mol/dds.hpp"

using namespace mol::dds;

namespace {
// BC7 解码向量：每个模式一个随机块 + Pillow（独立实现）解出的 16 个 RGBA 像素
struct Bc7Vec { unsigned char block[16]; unsigned char rgba[64]; };
static const Bc7Vec kBc7Vectors[8] = {
  {{115,221,143,219,236,199,119,115,130,218,150,48,47,205,131,121}, {197,101,174,255,215,104,167,255,181,99,181,255,231,106,160,255,167,136,33,255,115,49,16,255,204,197,45,255,204,197,45,255,187,168,40,255,167,136,33,255,115,49,16,255,239,255,57,255,215,210,160,255,208,232,95,255,222,189,222,255,208,232,95,255}},
  {{162,157,203,47,24,114,77,36,23,137,207,227,177,162,10,152}, {147,71,133,255,129,85,110,255,160,87,99,255,215,92,80,255,177,43,119,255,147,71,133,255,129,85,110,255,101,83,120,255,188,90,89,255,129,85,110,255,137,80,138,255,168,52,123,255,243,94,70,255,243,94,70,255,74,80,129,255,158,61,128,255}},
  {{252,101,246,115,167,189,157,166,40,159,3,212,135,16,15,9}, {148,222,206,255,247,115,57,255,186,182,19,255,247,115,57,255,222,165,82,255,187,222,106,255,247,115,57,255,186,182,19,255,173,133,139,255,74,66,255,255,148,222,206,255,247,115,57,255,173,133,139,255,123,98,198,255,222,165,82,255,148,222,206,255}},
  {{56,225,61,153,7,199,118,83,112,151,215,50,132,59,163,75}, {181,73,173,255,50,110,174,255,50,110,174,255,37,50,193,255,60,108,150,255,119,91,161,255,30,20,202,255,50,110,174,255,60,108,150,255,240,56,184,255,119,91,161,255,37,50,193,255,60,108,150,255,119,91,161,255,240,56,184,255,181,73,173,255}},
  {{112,1,169,21,117,167,71,104,255,141,254,238,215,21,181,65}, {27,85,167,164,8,82,235,140,47,87,167,190,8,82,235,140,8,82,218,140,27,85,235,164,66,90,202,214,47,87,218,190,66,90,202,214,66,90,150,214,66,90,185,214,66,90,150,214,47,87,167,190,27,85,167,164,8,82,117,140,27,85,150,164}},
  {{96,194,58,131,73,7,17,144,196,27,102,27,216,74,98,17}, {4,66,178,166,26,24,233,133,15,111,119,202,36,153,64,235,26,66,178,166,26,153,64,235,4,24,233,133,15,24,233,133,26,153,64,235,4,24,233,133,26,153,64,235,15,111,119,202,15,66,178,166,4,153,64,235,15,24,233,133,4,24,233,133}},
  {{192,4,216,175,0,54,53,237,233,13,120,96,250,181,101,107}, {65,191,136,97,182,36,153,209,169,54,151,196,19,253,129,53,111,130,143,141,101,144,141,131,19,253,129,53,90,159,140,120,136,97,146,165,193,21,155,219,76,177,138,107,147,83,148,175,76,177,138,107,90,159,140,120,147,83,148,175,90,159,140,120}},
  {{128,144,161,50,199,172,69,86,22,79,85,3,246,104,194,236}, {85,159,163,130,125,182,121,105,162,203,81,81,85,159,163,130,146,211,178,170,85,159,163,130,162,203,81,81,48,138,203,154,166,148,185,118,146,211,178,170,187,83,192,64,85,159,163,130,187,83,192,64,166,148,185,118,207,20,199,12,166,148,185,118}},
};

std::string dds_with_block(const unsigned char* block) {
    std::string h = "DDS ";
    auto w32 = [&](unsigned v) { for (int i = 0; i < 4; ++i) h.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); };
    w32(124); w32(0x1 | 0x2 | 0x4 | 0x1000 | 0x80000); w32(4); w32(4); w32(16); w32(0); w32(1);
    h.append(44, '\0');
    w32(32); w32(4); h += "DX10"; for (int i = 0; i < 5; ++i) w32(0);
    w32(0x1000); for (int i = 0; i < 4; ++i) w32(0);
    w32(98); w32(3); w32(0); w32(1); w32(0);
    h.append(reinterpret_cast<const char*>(block), 16);
    return h;
}

Image gradient(int w, int h) {
    Image im;
    im.w = w; im.h = h;
    im.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto* p = &im.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4];
            p[0] = static_cast<std::uint8_t>(x * 255 / std::max(1, w - 1));
            p[1] = static_cast<std::uint8_t>(y * 255 / std::max(1, h - 1));
            p[2] = static_cast<std::uint8_t>(((x / 8 + y / 8) & 1) ? 200 : 40);
            p[3] = static_cast<std::uint8_t>(255 - (x + y) * 100 / (w + h));
        }
    return im;
}
double psnr(const Image& a, const Image& b, int channels = 4) {
    double se = 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.rgba.size(); i += 4)
        for (int c = 0; c < channels; ++c) { const double d = static_cast<double>(a.rgba[i + static_cast<std::size_t>(c)]) - b.rgba[i + static_cast<std::size_t>(c)]; se += d * d; ++n; }
    return se == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / (se / static_cast<double>(n)));
}
}  // namespace

TEST(bc7_decoder_matches_independent_reference_for_all_modes) {
    for (int m = 0; m < 8; ++m) {
        const Image img = decode(dds_with_block(kBc7Vectors[m].block));
        CHECK_EQ(img.w, 4);
        CHECK_EQ(img.h, 4);
        CHECK(std::memcmp(img.rgba.data(), kBc7Vectors[m].rgba, 64) == 0);
    }
}

TEST(encode_decode_roundtrip_quality_for_every_format) {
    const Image src = gradient(64, 48);
    struct Case { const char* name; double min_psnr; int channels; };
    for (const Case& c : {Case{"BC1_UNORM", 28, 3}, Case{"BC2_UNORM", 28, 4}, Case{"BC3_UNORM", 28, 4}, Case{"BC4_UNORM", 30, 1},
                          Case{"BC5_UNORM", 30, 2}, Case{"BC7_UNORM", 35, 4}, Case{"R8G8B8A8_UNORM", 90, 4}, Case{"B8G8R8A8_UNORM", 90, 4}}) {
        const std::string file = encode(src, format_from_name(c.name), 1);
        const Image back = decode(file);
        CHECK_EQ(back.w, 64);
        CHECK_EQ(back.h, 48);
        CHECK(psnr(src, back, c.channels) >= c.min_psnr);
    }
}

TEST(mip_chain_and_header) {
    const Image src = gradient(64, 32);
    const std::string full = encode(src, Format::BC7, 0);
    CHECK_EQ(static_cast<unsigned char>(full[28]), 7);  // 64x32 → 7 层（64..1）
    const std::string three = encode(src, Format::BC1, 3);
    CHECK_EQ(static_cast<unsigned char>(three[28]), 3);
    CHECK(std::memcmp(three.data() + 84, "DXT1", 4) == 0);
    const std::string bc7 = encode(src, Format::BC7, 1);
    CHECK(std::memcmp(bc7.data() + 84, "DX10", 4) == 0);
    // 解码总是取顶层 mip
    CHECK_EQ(decode(full).w, 64);
}

TEST(resize_and_non_multiple_of_four_sizes) {
    const Image src = gradient(100, 60);
    const Image half = resize(src, 50, 30);
    CHECK_EQ(half.w, 50);
    CHECK_EQ(half.h, 30);
    // 均匀色缩放后仍是同一个颜色
    Image flat;
    flat.w = 8; flat.h = 8; flat.rgba.assign(8 * 8 * 4, 0);
    for (std::size_t i = 0; i < flat.rgba.size(); i += 4) { flat.rgba[i] = 10; flat.rgba[i + 1] = 200; flat.rgba[i + 2] = 99; flat.rgba[i + 3] = 255; }
    const Image small = resize(flat, 3, 5);
    for (std::size_t i = 0; i < small.rgba.size(); i += 4) { CHECK_EQ(int(small.rgba[i]), 10); CHECK_EQ(int(small.rgba[i + 1]), 200); CHECK_EQ(int(small.rgba[i + 2]), 99); }
    // 非 4 的倍数尺寸也能编解码
    const Image odd = gradient(37, 21);
    const Image back = decode(encode(odd, Format::BC3, 1));
    CHECK_EQ(back.w, 37);
    CHECK_EQ(back.h, 21);
    CHECK(psnr(odd, back) >= 28);
}

TEST(bad_input_is_rejected) {
    bool threw = false;
    try { decode("not a dds"); } catch (const mol::Error&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { format_from_name("R32G32B32A32_FLOAT"); } catch (const mol::Error&) { threw = true; }
    CHECK(threw);
}

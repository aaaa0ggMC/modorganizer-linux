#include "mol/dds.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "bptc_tables.inc"

namespace mol::dds {
namespace {

[[noreturn]] void bad(const std::string& m) { throw Error("invalid_argument", "dds: " + m); }

std::uint32_t rd32(const std::string_view s, std::size_t at) {
    if (at + 4 > s.size()) bad("truncated file");
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + static_cast<std::size_t>(i)])) << (8 * i);
    return v;
}
void wr32(std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

struct Rgba { int r, g, b, a; };

// ---------------- BC1 / BC4 构件 ----------------
Rgba rgb565(unsigned c) { return {static_cast<int>(((c >> 11) & 31) * 255 / 31), static_cast<int>(((c >> 5) & 63) * 255 / 63), static_cast<int>((c & 31) * 255 / 31), 255}; }

void decode_bc1_color(const std::uint8_t* b, Rgba out[16], bool allow_alpha) {
    const unsigned c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    Rgba p[4];
    p[0] = rgb565(c0);
    p[1] = rgb565(c1);
    if (c0 > c1 || !allow_alpha) {
        p[2] = {(2 * p[0].r + p[1].r) / 3, (2 * p[0].g + p[1].g) / 3, (2 * p[0].b + p[1].b) / 3, 255};
        p[3] = {(p[0].r + 2 * p[1].r) / 3, (p[0].g + 2 * p[1].g) / 3, (p[0].b + 2 * p[1].b) / 3, 255};
    } else {
        p[2] = {(p[0].r + p[1].r) / 2, (p[0].g + p[1].g) / 2, (p[0].b + p[1].b) / 2, 255};
        p[3] = {0, 0, 0, 0};
    }
    std::uint32_t bits = b[4] | (b[5] << 8) | (b[6] << 16) | (static_cast<std::uint32_t>(b[7]) << 24);
    for (int i = 0; i < 16; ++i) out[i] = p[(bits >> (2 * i)) & 3];
}

void decode_bc4(const std::uint8_t* b, int out[16]) {
    int pal[8];
    pal[0] = b[0];
    pal[1] = b[1];
    if (pal[0] > pal[1]) {
        for (int i = 1; i <= 6; ++i) pal[1 + i] = ((7 - i) * pal[0] + i * pal[1]) / 7;
    } else {
        for (int i = 1; i <= 4; ++i) pal[1 + i] = ((5 - i) * pal[0] + i * pal[1]) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) out[i] = pal[(bits >> (3 * i)) & 7];
}

// ---------------- BC7 解码 ----------------
struct Bc7Mode { int subsets, part_bits, rot_bits, idxsel_bits, color_bits, alpha_bits, pbits_per_endpoint, shared_pbit, idx_bits, aidx_bits; };
constexpr Bc7Mode kModes[8] = {{3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0}, {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
                               {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2}, {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}};
constexpr int kW2[4] = {0, 21, 43, 64};
constexpr int kW3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr int kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
const int* weights(int bits) { return bits == 2 ? kW2 : (bits == 3 ? kW3 : kW4); }

struct BitReader {
    const std::uint8_t* d;
    int pos = 0;
    unsigned get(int n) {
        unsigned v = 0;
        for (int i = 0; i < n; ++i, ++pos) v |= ((d[pos >> 3] >> (pos & 7)) & 1u) << i;
        return v;
    }
};

int expand(int v, int bits) {  // bits 位 → 8 位（高位复制）
    v <<= (8 - bits);
    return v | (v >> bits);
}

bool decode_bc7(const std::uint8_t* blk, Rgba out[16]) {
    BitReader br{blk};
    int mode = 0;
    while (mode < 8 && !br.get(1)) ++mode;
    if (mode >= 8) { for (int i = 0; i < 16; ++i) out[i] = {0, 0, 0, 0}; return false; }
    const Bc7Mode& m = kModes[mode];
    const int part = static_cast<int>(br.get(m.part_bits));
    const int rot = static_cast<int>(br.get(m.rot_bits));
    const int idxsel = static_cast<int>(br.get(m.idxsel_bits));
    const int ne = m.subsets * 2;
    int ep[6][4] = {};
    for (int c = 0; c < 3; ++c) for (int e = 0; e < ne; ++e) ep[e][c] = static_cast<int>(br.get(m.color_bits));
    if (m.alpha_bits) for (int e = 0; e < ne; ++e) ep[e][3] = static_cast<int>(br.get(m.alpha_bits));
    int pb[6] = {};
    if (m.pbits_per_endpoint) for (int e = 0; e < ne; ++e) pb[e] = static_cast<int>(br.get(1));
    else if (m.shared_pbit) for (int s = 0; s < m.subsets; ++s) { const int v = static_cast<int>(br.get(1)); pb[2 * s] = pb[2 * s + 1] = v; }
    const bool has_p = m.pbits_per_endpoint || m.shared_pbit;
    int e8[6][4];
    for (int e = 0; e < ne; ++e) {
        for (int c = 0; c < 3; ++c) {
            int v = ep[e][c], bits = m.color_bits;
            if (has_p) { v = (v << 1) | pb[e]; ++bits; }
            e8[e][c] = expand(v, bits);
        }
        if (m.alpha_bits) {
            int v = ep[e][3], bits = m.alpha_bits;
            if (has_p) { v = (v << 1) | pb[e]; ++bits; }
            e8[e][3] = expand(v, bits);
        } else e8[e][3] = 255;
    }
    auto subset_of = [&](int i) { return m.subsets == 1 ? 0 : (m.subsets == 2 ? bptc::kPart2[part][i] : bptc::kPart3[part][i]); };
    auto is_anchor = [&](int i) {
        if (i == 0) return true;
        if (m.subsets == 2) return i == bptc::kAnchor2[part];
        if (m.subsets == 3) return i == bptc::kAnchor3[part][0] || i == bptc::kAnchor3[part][1];
        return false;
    };
    int idx[16], aidx[16];
    const int ib = m.idx_bits, ab = m.aidx_bits;
    for (int i = 0; i < 16; ++i) idx[i] = static_cast<int>(br.get(is_anchor(i) ? ib - 1 : ib));
    if (ab) for (int i = 0; i < 16; ++i) aidx[i] = static_cast<int>(br.get(i == 0 ? ab - 1 : ab));
    for (int i = 0; i < 16; ++i) {
        const int s = subset_of(i);
        const int* e0 = e8[2 * s];
        const int* e1 = e8[2 * s + 1];
        int col_bits = ib, a_bits = ib;
        int ci = idx[i], ai = idx[i];
        if (ab) {
            if (idxsel) { col_bits = ab; a_bits = ib; ci = aidx[i]; ai = idx[i]; }
            else { col_bits = ib; a_bits = ab; ci = idx[i]; ai = aidx[i]; }
        }
        const int wc = weights(col_bits)[ci], wa = weights(a_bits)[ai];
        Rgba px;
        px.r = ((64 - wc) * e0[0] + wc * e1[0] + 32) >> 6;
        px.g = ((64 - wc) * e0[1] + wc * e1[1] + 32) >> 6;
        px.b = ((64 - wc) * e0[2] + wc * e1[2] + 32) >> 6;
        px.a = m.alpha_bits ? ((64 - wa) * e0[3] + wa * e1[3] + 32) >> 6 : 255;
        if (rot == 1) std::swap(px.r, px.a);
        else if (rot == 2) std::swap(px.g, px.a);
        else if (rot == 3) std::swap(px.b, px.a);
        out[i] = px;
    }
    return true;
}

// ---------------- 容器 ----------------
struct Parsed {
    int w = 0, h = 0, mips = 1;
    int dxgi = 0;      // DX10 头里的格式；0 = 传统头
    std::string fourcc;
    std::uint32_t bitcount = 0, rmask = 0, gmask = 0, bmask = 0, amask = 0;
    std::size_t data_off = 0;
};

Parsed parse(std::string_view s) {
    if (s.size() < 128 || std::memcmp(s.data(), "DDS ", 4) != 0) bad("not a DDS file");
    Parsed p;
    if (rd32(s, 4) != 124) bad("bad header size");
    p.h = static_cast<int>(rd32(s, 12));
    p.w = static_cast<int>(rd32(s, 16));
    p.mips = std::max(1, static_cast<int>(rd32(s, 28)));
    const std::uint32_t pf_flags = rd32(s, 80);
    p.fourcc.assign(s.data() + 84, 4);
    p.bitcount = rd32(s, 88);
    p.rmask = rd32(s, 92); p.gmask = rd32(s, 96); p.bmask = rd32(s, 100); p.amask = rd32(s, 104);
    p.data_off = 128;
    if (!(pf_flags & 4)) p.fourcc.clear();
    if (p.fourcc == "DX10") {
        if (s.size() < 148) bad("truncated DX10 header");
        p.dxgi = static_cast<int>(rd32(s, 128));
        p.data_off = 148;
    }
    if (p.w <= 0 || p.h <= 0 || p.w > 16384 || p.h > 16384) bad("implausible size");
    return p;
}

Format format_of(const Parsed& p) {
    if (p.dxgi) {
        switch (p.dxgi) {
            case 70: case 71: case 72: return Format::BC1;
            case 73: case 74: case 75: return Format::BC2;
            case 76: case 77: case 78: return Format::BC3;
            case 79: case 80: case 81: return Format::BC4;
            case 82: case 83: case 84: return Format::BC5;
            case 97: case 98: case 99: return Format::BC7;
            case 27: case 28: case 29: return Format::RGBA8;
            case 86: case 87: case 91: return Format::BGRA8;
            default: bad("unsupported DXGI format " + std::to_string(p.dxgi));
        }
    }
    if (p.fourcc == "DXT1") return Format::BC1;
    if (p.fourcc == "DXT2" || p.fourcc == "DXT3") return Format::BC2;
    if (p.fourcc == "DXT4" || p.fourcc == "DXT5") return Format::BC3;
    if (p.fourcc == "ATI1" || p.fourcc == "BC4U") return Format::BC4;
    if (p.fourcc == "ATI2" || p.fourcc == "BC5U") return Format::BC5;
    if (p.fourcc.empty() && p.bitcount == 32) return (p.rmask == 0xFF) ? Format::RGBA8 : Format::BGRA8;
    bad("unsupported pixel format");
}

}  // namespace

Format format_from_name(std::string_view n) {
    std::string s(n);
    for (const char* suffix : {"_SRGB", "_UNORM", "_TYPELESS"}) {
        const std::string sf(suffix);
        if (s.size() > sf.size() && s.compare(s.size() - sf.size(), sf.size(), sf) == 0) { s.resize(s.size() - sf.size()); break; }
    }
    if (s == "BC1") return Format::BC1;
    if (s == "BC2") return Format::BC2;
    if (s == "BC3") return Format::BC3;
    if (s == "BC4") return Format::BC4;
    if (s == "BC5") return Format::BC5;
    if (s == "BC7") return Format::BC7;
    if (s == "R8G8B8A8") return Format::RGBA8;
    if (s == "B8G8R8A8" || s == "B8G8R8X8") return Format::BGRA8;
    bad("unsupported texture format " + std::string(n));
}

Image decode(std::string_view s) {
    const Parsed p = parse(s);
    const Format f = format_of(p);
    Image img;
    img.w = p.w;
    img.h = p.h;
    img.rgba.assign(static_cast<std::size_t>(p.w) * static_cast<std::size_t>(p.h) * 4, 0);
    const auto* d = reinterpret_cast<const std::uint8_t*>(s.data()) + p.data_off;
    const std::size_t avail = s.size() - p.data_off;
    auto put = [&](int x, int y, const Rgba& c) {
        if (x >= p.w || y >= p.h) return;
        std::uint8_t* o = &img.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(p.w) + static_cast<std::size_t>(x)) * 4];
        o[0] = static_cast<std::uint8_t>(clampi(c.r, 0, 255));
        o[1] = static_cast<std::uint8_t>(clampi(c.g, 0, 255));
        o[2] = static_cast<std::uint8_t>(clampi(c.b, 0, 255));
        o[3] = static_cast<std::uint8_t>(clampi(c.a, 0, 255));
    };
    if (f == Format::RGBA8 || f == Format::BGRA8) {
        if (avail < static_cast<std::size_t>(p.w) * static_cast<std::size_t>(p.h) * 4) bad("truncated pixel data");
        for (int y = 0; y < p.h; ++y)
            for (int x = 0; x < p.w; ++x) {
                const std::uint8_t* q = d + (static_cast<std::size_t>(y) * static_cast<std::size_t>(p.w) + static_cast<std::size_t>(x)) * 4;
                if (f == Format::RGBA8) put(x, y, {q[0], q[1], q[2], q[3]});
                else put(x, y, {q[2], q[1], q[0], q[3]});
            }
        return img;
    }
    const int bw = (p.w + 3) / 4, bh = (p.h + 3) / 4;
    const std::size_t bsz = (f == Format::BC1 || f == Format::BC4) ? 8 : 16;
    if (avail < static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh) * bsz) bad("truncated block data");
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const std::uint8_t* b = d + (static_cast<std::size_t>(by) * static_cast<std::size_t>(bw) + static_cast<std::size_t>(bx)) * bsz;
            Rgba px[16];
            switch (f) {
                case Format::BC1: decode_bc1_color(b, px, true); break;
                case Format::BC2: {
                    decode_bc1_color(b + 8, px, false);
                    for (int i = 0; i < 16; ++i) { const int a4 = (b[i / 2] >> ((i & 1) * 4)) & 15; px[i].a = a4 * 17; }
                    break;
                }
                case Format::BC3: {
                    decode_bc1_color(b + 8, px, false);
                    int a[16];
                    decode_bc4(b, a);
                    for (int i = 0; i < 16; ++i) px[i].a = a[i];
                    break;
                }
                case Format::BC4: {
                    int r[16];
                    decode_bc4(b, r);
                    for (int i = 0; i < 16; ++i) px[i] = {r[i], r[i], r[i], 255};
                    break;
                }
                case Format::BC5: {
                    int r[16], g[16];
                    decode_bc4(b, r);
                    decode_bc4(b + 8, g);
                    for (int i = 0; i < 16; ++i) {
                        // 法线图：重建 Z
                        const float x = r[i] / 127.5f - 1.f, y = g[i] / 127.5f - 1.f;
                        const float z2 = std::max(0.f, 1.f - x * x - y * y);
                        px[i] = {r[i], g[i], static_cast<int>((std::sqrt(z2) * 0.5f + 0.5f) * 255.f + 0.5f), 255};
                    }
                    break;
                }
                case Format::BC7: decode_bc7(b, px); break;
                default: break;
            }
            for (int i = 0; i < 16; ++i) put(bx * 4 + (i & 3), by * 4 + (i >> 2), px[i]);
        }
    return img;
}

Image resize(const Image& src, int w, int h) {
    if (w <= 0 || h <= 0) bad("bad target size");
    Image out;
    out.w = w;
    out.h = h;
    out.rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
    if (w == src.w && h == src.h) { out.rgba = src.rgba; return out; }
    for (int y = 0; y < h; ++y) {
        const double y0 = static_cast<double>(y) * src.h / h, y1 = static_cast<double>(y + 1) * src.h / h;
        for (int x = 0; x < w; ++x) {
            const double x0 = static_cast<double>(x) * src.w / w, x1 = static_cast<double>(x + 1) * src.w / w;
            double acc[4] = {0, 0, 0, 0}, wsum = 0;
            for (int sy = static_cast<int>(y0); sy < std::min(src.h, static_cast<int>(std::ceil(y1))); ++sy) {
                const double wy = std::min<double>(sy + 1, y1) - std::max<double>(sy, y0);
                for (int sx = static_cast<int>(x0); sx < std::min(src.w, static_cast<int>(std::ceil(x1))); ++sx) {
                    const double wx = std::min<double>(sx + 1, x1) - std::max<double>(sx, x0);
                    const double wgt = wx * wy;
                    const std::uint8_t* q = &src.rgba[(static_cast<std::size_t>(sy) * static_cast<std::size_t>(src.w) + static_cast<std::size_t>(sx)) * 4];
                    const double a = q[3] / 255.0;  // 按 alpha 加权，避免透明像素的颜色渗出
                    acc[0] += q[0] * a * wgt; acc[1] += q[1] * a * wgt; acc[2] += q[2] * a * wgt; acc[3] += q[3] * wgt;
                    wsum += wgt;
                }
            }
            std::uint8_t* o = &out.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4];
            if (wsum > 0) {
                const double alpha = acc[3] / wsum;
                const double inv = alpha > 0 ? 1.0 / (alpha / 255.0 * wsum) : 0.0;
                o[0] = static_cast<std::uint8_t>(clampi(static_cast<int>(acc[0] * inv + 0.5), 0, 255));
                o[1] = static_cast<std::uint8_t>(clampi(static_cast<int>(acc[1] * inv + 0.5), 0, 255));
                o[2] = static_cast<std::uint8_t>(clampi(static_cast<int>(acc[2] * inv + 0.5), 0, 255));
                o[3] = static_cast<std::uint8_t>(clampi(static_cast<int>(alpha + 0.5), 0, 255));
            }
        }
    }
    return out;
}

// ---------------- 编码 ----------------
namespace {

unsigned to565(int r, int g, int b) { return static_cast<unsigned>(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255)); }

void encode_bc1_color(const Rgba px[16], std::uint8_t* out, bool opaque_only) {
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    bool any_transparent = false;
    for (int i = 0; i < 16; ++i) {
        if (!opaque_only && px[i].a < 128) { any_transparent = true; continue; }
        lo[0] = std::min(lo[0], px[i].r); lo[1] = std::min(lo[1], px[i].g); lo[2] = std::min(lo[2], px[i].b);
        hi[0] = std::max(hi[0], px[i].r); hi[1] = std::max(hi[1], px[i].g); hi[2] = std::max(hi[2], px[i].b);
    }
    if (lo[0] > hi[0]) { std::memset(out, 0, 8); if (any_transparent) { out[0] = 0; out[1] = 0; out[2] = 1; out[4] = out[5] = out[6] = out[7] = 0xFF; } return; }
    // 稍微内缩包围盒，减小极端值误差
    for (int c = 0; c < 3; ++c) { const int inset = (hi[c] - lo[c]) / 16; lo[c] += inset; hi[c] -= inset; }
    unsigned c0 = to565(hi[0], hi[1], hi[2]), c1 = to565(lo[0], lo[1], lo[2]);
    if (any_transparent && !opaque_only) { if (c0 > c1) std::swap(c0, c1); }
    else if (c0 < c1) std::swap(c0, c1);
    if (c0 == c1 && !(any_transparent && !opaque_only)) { if (c1 > 0) --c1; else ++c0; if (c0 < c1) std::swap(c0, c1); }
    out[0] = static_cast<std::uint8_t>(c0 & 255); out[1] = static_cast<std::uint8_t>(c0 >> 8);
    out[2] = static_cast<std::uint8_t>(c1 & 255); out[3] = static_cast<std::uint8_t>(c1 >> 8);
    Rgba p[4];
    p[0] = rgb565(c0); p[1] = rgb565(c1);
    const bool four = c0 > c1;
    if (four) {
        p[2] = {(2 * p[0].r + p[1].r) / 3, (2 * p[0].g + p[1].g) / 3, (2 * p[0].b + p[1].b) / 3, 255};
        p[3] = {(p[0].r + 2 * p[1].r) / 3, (p[0].g + 2 * p[1].g) / 3, (p[0].b + 2 * p[1].b) / 3, 255};
    } else {
        p[2] = {(p[0].r + p[1].r) / 2, (p[0].g + p[1].g) / 2, (p[0].b + p[1].b) / 2, 255};
        p[3] = {0, 0, 0, 0};
    }
    std::uint32_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0;
        if (!four && !opaque_only && px[i].a < 128) best = 3;
        else {
            long bd = 1L << 40;
            for (int k = 0; k < (four ? 4 : 3); ++k) {
                const long dr = px[i].r - p[k].r, dg = px[i].g - p[k].g, db = px[i].b - p[k].b;
                const long dd = dr * dr + dg * dg + db * db;
                if (dd < bd) { bd = dd; best = k; }
            }
        }
        bits |= static_cast<std::uint32_t>(best) << (2 * i);
    }
    out[4] = static_cast<std::uint8_t>(bits & 255); out[5] = static_cast<std::uint8_t>((bits >> 8) & 255);
    out[6] = static_cast<std::uint8_t>((bits >> 16) & 255); out[7] = static_cast<std::uint8_t>(bits >> 24);
}

void encode_bc4(const int v[16], std::uint8_t* out) {
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; ++i) { lo = std::min(lo, v[i]); hi = std::max(hi, v[i]); }
    // 使用 8 级模式（e0 > e1）
    out[0] = static_cast<std::uint8_t>(hi);
    out[1] = static_cast<std::uint8_t>(lo);
    if (hi == lo) { std::memset(out + 2, 0, 6); return; }
    int pal[8];
    pal[0] = hi; pal[1] = lo;
    for (int i = 1; i <= 6; ++i) pal[1 + i] = ((7 - i) * hi + i * lo) / 7;
    std::uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0, bd = 1 << 30;
        for (int k = 0; k < 8; ++k) { const int dd = std::abs(v[i] - pal[k]); if (dd < bd) { bd = dd; best = k; } }
        bits |= static_cast<std::uint64_t>(best) << (3 * i);
    }
    for (int i = 0; i < 6; ++i) out[2 + i] = static_cast<std::uint8_t>((bits >> (8 * i)) & 255);
}

struct BitWriter {
    std::uint8_t d[16] = {};
    int pos = 0;
    void put(unsigned v, int n) { for (int i = 0; i < n; ++i, ++pos) if ((v >> i) & 1u) d[pos >> 3] |= static_cast<std::uint8_t>(1u << (pos & 7)); }
};

// BC7 mode 6：单子集、RGBA 各 7 位 + 每端点 1 个 p-bit、4 位索引
void encode_bc7_mode6(const Rgba px[16], std::uint8_t* out) {
    // 以颜色空间主轴取两端点：用包围盒对角线的近似
    int lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        const int c[4] = {px[i].r, px[i].g, px[i].b, px[i].a};
        for (int k = 0; k < 4; ++k) { lo[k] = std::min(lo[k], c[k]); hi[k] = std::max(hi[k], c[k]); }
    }
    // 主轴：对 (lo→hi) 的方向做投影，若某通道范围为 0 则忽略
    auto quant = [](int v, int p) { // 8 位值 → (7 位, 端点 p-bit)：重建值 = (q<<1|p) 扩展
        int q = clampi((v * 127 + 127) / 255, 0, 127);
        // 选择使 (q<<1|p) 最接近 v/255*255 的 q
        int best = q, bd = 1 << 30;
        for (int cq = std::max(0, q - 1); cq <= std::min(127, q + 1); ++cq) {
            const int rec = ((cq << 1) | p);
            const int d = std::abs(rec - v);
            if (d < bd) { bd = d; best = cq; }
        }
        return best;
    };
    int bestp0 = 0, bestp1 = 0;
    long best_err = 1L << 60;
    int q0[4], q1[4], bq0[4] = {}, bq1[4] = {};
    for (int p0 = 0; p0 < 2; ++p0)
        for (int p1 = 0; p1 < 2; ++p1) {
            long err = 0;
            for (int k = 0; k < 4; ++k) {
                q0[k] = quant(lo[k], p0); q1[k] = quant(hi[k], p1);
                const int r0 = ((q0[k] << 1) | p0), r1 = ((q1[k] << 1) | p1);
                err += static_cast<long>(std::abs(r0 - lo[k])) + std::abs(r1 - hi[k]);
            }
            if (err < best_err) { best_err = err; bestp0 = p0; bestp1 = p1; std::copy(q0, q0 + 4, bq0); std::copy(q1, q1 + 4, bq1); }
        }
    int e0[4], e1[4];
    for (int k = 0; k < 4; ++k) { e0[k] = expand((bq0[k] << 1) | bestp0, 8); e1[k] = expand((bq1[k] << 1) | bestp1, 8); }
    auto indices = [&](const int a[4], const int b[4], int idx[16]) {
        long total = 0;
        int dir[4], len2 = 0;
        for (int k = 0; k < 4; ++k) { dir[k] = b[k] - a[k]; len2 += dir[k] * dir[k]; }
        for (int i = 0; i < 16; ++i) {
            const int c[4] = {px[i].r, px[i].g, px[i].b, px[i].a};
            int t = 0;
            if (len2 > 0) { long dot = 0; for (int k = 0; k < 4; ++k) dot += static_cast<long>(c[k] - a[k]) * dir[k]; t = static_cast<int>(std::lround(15.0 * static_cast<double>(dot) / len2)); }
            idx[i] = clampi(t, 0, 15);
            // 误差（用来比较是否交换端点；这里只关心结果）
            long e = 0;
            for (int k = 0; k < 4; ++k) { const int rec = ((64 - kW4[idx[i]]) * a[k] + kW4[idx[i]] * b[k] + 32) >> 6; e += static_cast<long>(rec - c[k]) * (rec - c[k]); }
            total += e;
        }
        return total;
    };
    int idx[16];
    indices(e0, e1, idx);
    int s0[4], s1[4], sp0 = bestp0, sp1 = bestp1;
    std::copy(bq0, bq0 + 4, s0); std::copy(bq1, bq1 + 4, s1);
    if (idx[0] >= 8) {  // 锚点（像素 0）的索引最高位必须为 0：交换端点并翻转索引
        std::swap_ranges(s0, s0 + 4, s1);
        std::swap(sp0, sp1);
        for (int i = 0; i < 16; ++i) idx[i] = 15 - idx[i];
    }
    BitWriter bw;
    bw.put(1u << 6, 7);  // mode 6：6 个 0 再一个 1 → 位 6 置 1
    for (int k = 0; k < 3; ++k) { bw.put(static_cast<unsigned>(s0[k]), 7); bw.put(static_cast<unsigned>(s1[k]), 7); }
    bw.put(static_cast<unsigned>(s0[3]), 7); bw.put(static_cast<unsigned>(s1[3]), 7);
    bw.put(static_cast<unsigned>(sp0), 1); bw.put(static_cast<unsigned>(sp1), 1);
    for (int i = 0; i < 16; ++i) bw.put(static_cast<unsigned>(idx[i]), i == 0 ? 3 : 4);
    std::memcpy(out, bw.d, 16);
}

int dxgi_of(Format f) {
    switch (f) {
        case Format::BC1: return 71; case Format::BC2: return 74; case Format::BC3: return 77; case Format::BC4: return 80;
        case Format::BC5: return 83; case Format::BC7: return 98; case Format::RGBA8: return 28; case Format::BGRA8: return 87;
    }
    return 0;
}

std::string encode_level(const Image& im, Format f) {
    std::string out;
    if (f == Format::RGBA8 || f == Format::BGRA8) {
        out.resize(im.rgba.size());
        for (std::size_t i = 0; i < im.rgba.size(); i += 4) {
            out[i] = static_cast<char>(f == Format::RGBA8 ? im.rgba[i] : im.rgba[i + 2]);
            out[i + 1] = static_cast<char>(im.rgba[i + 1]);
            out[i + 2] = static_cast<char>(f == Format::RGBA8 ? im.rgba[i + 2] : im.rgba[i]);
            out[i + 3] = static_cast<char>(im.rgba[i + 3]);
        }
        return out;
    }
    const int bw = (im.w + 3) / 4, bh = (im.h + 3) / 4;
    const std::size_t bsz = (f == Format::BC1 || f == Format::BC4) ? 8 : 16;
    out.assign(static_cast<std::size_t>(bw) * static_cast<std::size_t>(bh) * bsz, '\0');
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            Rgba px[16];
            for (int i = 0; i < 16; ++i) {
                const int x = std::min(im.w - 1, bx * 4 + (i & 3)), y = std::min(im.h - 1, by * 4 + (i >> 2));
                const std::uint8_t* q = &im.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(im.w) + static_cast<std::size_t>(x)) * 4];
                px[i] = {q[0], q[1], q[2], q[3]};
            }
            auto* o = reinterpret_cast<std::uint8_t*>(&out[(static_cast<std::size_t>(by) * static_cast<std::size_t>(bw) + static_cast<std::size_t>(bx)) * bsz]);
            switch (f) {
                case Format::BC1: encode_bc1_color(px, o, false); break;
                case Format::BC2: {
                    for (int i = 0; i < 16; i += 2) o[i / 2] = static_cast<std::uint8_t>((px[i].a / 17) | ((px[i + 1].a / 17) << 4));
                    encode_bc1_color(px, o + 8, true);
                    break;
                }
                case Format::BC3: {
                    int a[16];
                    for (int i = 0; i < 16; ++i) a[i] = px[i].a;
                    encode_bc4(a, o);
                    encode_bc1_color(px, o + 8, true);
                    break;
                }
                case Format::BC4: { int r[16]; for (int i = 0; i < 16; ++i) r[i] = px[i].r; encode_bc4(r, o); break; }
                case Format::BC5: {
                    int r[16], g[16];
                    for (int i = 0; i < 16; ++i) { r[i] = px[i].r; g[i] = px[i].g; }
                    encode_bc4(r, o);
                    encode_bc4(g, o + 8);
                    break;
                }
                case Format::BC7: encode_bc7_mode6(px, o); break;
                default: break;
            }
        }
    return out;
}

}  // namespace

std::string encode(const Image& img, Format fmt, int mips) {
    if (img.w <= 0 || img.h <= 0) bad("empty image");
    int full = 1;
    for (int w = img.w, h = img.h; w > 1 || h > 1; w = std::max(1, w / 2), h = std::max(1, h / 2)) ++full;
    const int levels = mips <= 0 ? full : std::min(mips, full);
    std::vector<std::string> data;
    Image cur = img;
    for (int l = 0; l < levels; ++l) {
        data.push_back(encode_level(cur, fmt));
        if (l + 1 < levels) cur = resize(cur, std::max(1, cur.w / 2), std::max(1, cur.h / 2));
    }
    std::string out = "DDS ";
    wr32(out, 124);
    wr32(out, 0x1 | 0x2 | 0x4 | 0x1000 | (levels > 1 ? 0x20000u : 0u) | ((fmt == Format::RGBA8 || fmt == Format::BGRA8) ? 0x8u : 0x80000u));
    wr32(out, static_cast<std::uint32_t>(img.h));
    wr32(out, static_cast<std::uint32_t>(img.w));
    wr32(out, static_cast<std::uint32_t>(data[0].size()));
    wr32(out, 0);
    wr32(out, static_cast<std::uint32_t>(levels));
    out.append(44, '\0');
    // 像素格式：统一用 DX10 头（BC1/2/3 也可用，游戏与工具都认）；传统 FourCC 只在 BC1/2/3 时用以获得最大兼容
    const bool legacy = fmt == Format::BC1 || fmt == Format::BC2 || fmt == Format::BC3;
    wr32(out, 32);
    if (legacy) {
        wr32(out, 4);
        out.append(fmt == Format::BC1 ? "DXT1" : (fmt == Format::BC2 ? "DXT3" : "DXT5"), 4);
        for (int i = 0; i < 5; ++i) wr32(out, 0);
    } else if (fmt == Format::RGBA8 || fmt == Format::BGRA8) {
        wr32(out, 0x41);  // RGB | ALPHAPIXELS
        wr32(out, 0);
        wr32(out, 32);
        if (fmt == Format::RGBA8) { wr32(out, 0xFF); wr32(out, 0xFF00); wr32(out, 0xFF0000); wr32(out, 0xFF000000u); }
        else { wr32(out, 0xFF0000); wr32(out, 0xFF00); wr32(out, 0xFF); wr32(out, 0xFF000000u); }
    } else {
        wr32(out, 4);
        out.append("DX10", 4);
        for (int i = 0; i < 5; ++i) wr32(out, 0);
    }
    wr32(out, 0x1000 | (levels > 1 ? 0x400008u : 0u));
    for (int i = 0; i < 4; ++i) wr32(out, 0);
    if (!legacy && fmt != Format::RGBA8 && fmt != Format::BGRA8) {
        wr32(out, static_cast<std::uint32_t>(dxgi_of(fmt)));
        wr32(out, 3);
        wr32(out, 0);
        wr32(out, 1);
        wr32(out, 0);
    }
    for (const auto& d : data) out += d;
    return out;
}

}  // namespace mol::dds

#include "mol/md5.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>

#include "mol/error.hpp"

namespace mol {
namespace {

struct Ctx {
    std::uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    std::uint64_t len = 0;
    std::array<unsigned char, 64> buf{};
    std::size_t fill = 0;
};

constexpr std::uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
    0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
    0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
    0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
    0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
    0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
constexpr int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14,
                       20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10,
                       15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

std::uint32_t rol(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void block(Ctx& c, const unsigned char* p) {
    std::uint32_t m[16];
    for (int i = 0; i < 16; ++i) m[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | (static_cast<std::uint32_t>(p[i * 4 + 3]) << 24);
    std::uint32_t a = c.a, b = c.b, cc = c.c, d = c.d;
    for (int i = 0; i < 64; ++i) {
        std::uint32_t f;
        int g;
        if (i < 16) { f = (b & cc) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & cc); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ cc ^ d; g = (3 * i + 5) % 16; }
        else { f = cc ^ (b | ~d); g = (7 * i) % 16; }
        const std::uint32_t tmp = d;
        d = cc;
        cc = b;
        b = b + rol(a + f + K[i] + m[g], S[i]);
        a = tmp;
    }
    c.a += a; c.b += b; c.c += cc; c.d += d;
}

void update(Ctx& c, const unsigned char* p, std::size_t n) {
    c.len += n;
    while (n > 0) {
        const std::size_t take = std::min(n, c.buf.size() - c.fill);
        std::memcpy(c.buf.data() + c.fill, p, take);
        c.fill += take; p += take; n -= take;
        if (c.fill == 64) { block(c, c.buf.data()); c.fill = 0; }
    }
}

std::string finish(Ctx& c) {
    const std::uint64_t bits = c.len * 8;
    const unsigned char pad = 0x80;
    update(c, &pad, 1);
    const unsigned char zero = 0;
    while (c.fill != 56) update(c, &zero, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; ++i) lenb[i] = static_cast<unsigned char>((bits >> (8 * i)) & 0xFF);
    update(c, lenb, 8);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (std::uint32_t v : {c.a, c.b, c.c, c.d})
        for (int i = 0; i < 4; ++i) {
            const unsigned char byte = static_cast<unsigned char>((v >> (8 * i)) & 0xFF);
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 15]);
        }
    return out;
}
}  // namespace

std::string md5_hex(std::string_view data) {
    Ctx c;
    update(c, reinterpret_cast<const unsigned char*>(data.data()), data.size());
    return finish(c);
}

std::string md5_file(std::string_view path) {
    std::ifstream in{std::string(path), std::ios::binary};
    if (!in) throw Error("io_error", "cannot open file for hashing", std::string(path));
    Ctx c;
    std::array<char, 1 << 16> buf;
    while (in) {
        in.read(buf.data(), buf.size());
        update(c, reinterpret_cast<const unsigned char*>(buf.data()), static_cast<std::size_t>(in.gcount()));
    }
    return finish(c);
}

}  // namespace mol

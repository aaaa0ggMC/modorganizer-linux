#include "mol/xxh64.hpp"

#include <array>
#include <cstring>
#include <fstream>

#include "mol/error.hpp"

namespace mol {
namespace {
constexpr std::uint64_t P1 = 11400714785074694791ULL, P2 = 14029467366897019727ULL, P3 = 1609587929392839161ULL, P4 = 9650029242287828579ULL,
                        P5 = 2870177450012600261ULL;
std::uint64_t rotl(std::uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
std::uint64_t rd64(const unsigned char* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);  // 小端主机；本项目只面向 x86-64/aarch64 小端
    return v;
}
std::uint32_t rd32(const unsigned char* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
std::uint64_t round1(std::uint64_t acc, std::uint64_t in) { return rotl(acc + in * P2, 31) * P1; }
std::uint64_t merge(std::uint64_t acc, std::uint64_t v) { return (acc ^ round1(0, v)) * P1 + P4; }

struct State {
    std::uint64_t v1, v2, v3, v4, total = 0;
    std::array<unsigned char, 32> buf{};
    std::size_t fill = 0;
    std::uint64_t seed;
    explicit State(std::uint64_t s) : v1(s + P1 + P2), v2(s + P2), v3(s), v4(s - P1), seed(s) {}
    void stripe(const unsigned char* p) {
        v1 = round1(v1, rd64(p));
        v2 = round1(v2, rd64(p + 8));
        v3 = round1(v3, rd64(p + 16));
        v4 = round1(v4, rd64(p + 24));
    }
    void update(const unsigned char* p, std::size_t n) {
        total += n;
        if (fill + n < 32) { std::memcpy(buf.data() + fill, p, n); fill += n; return; }
        if (fill) {
            const std::size_t take = 32 - fill;
            std::memcpy(buf.data() + fill, p, take);
            stripe(buf.data());
            p += take; n -= take; fill = 0;
        }
        while (n >= 32) { stripe(p); p += 32; n -= 32; }
        if (n) { std::memcpy(buf.data(), p, n); fill = n; }
    }
    std::uint64_t digest() const {
        std::uint64_t h;
        if (total >= 32) {
            h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
            h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4);
        } else {
            h = seed + P5;
        }
        h += total;
        const unsigned char* p = buf.data();
        std::size_t n = fill;
        while (n >= 8) { h ^= round1(0, rd64(p)); h = rotl(h, 27) * P1 + P4; p += 8; n -= 8; }
        if (n >= 4) { h ^= static_cast<std::uint64_t>(rd32(p)) * P1; h = rotl(h, 23) * P2 + P3; p += 4; n -= 4; }
        while (n > 0) { h ^= (*p++) * P5; h = rotl(h, 11) * P1; --n; }
        h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
        return h;
    }
};
}  // namespace

std::uint64_t xxh64(std::string_view data, std::uint64_t seed) {
    State s(seed);
    s.update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
    return s.digest();
}

std::uint64_t xxh64_file(std::string_view path) {
    std::ifstream in{std::string(path), std::ios::binary};
    if (!in) throw Error("io_error", "cannot open file for hashing", std::string(path));
    State s(0);
    std::array<char, 1 << 16> buf;
    while (in) {
        in.read(buf.data(), buf.size());
        s.update(reinterpret_cast<const unsigned char*>(buf.data()), static_cast<std::size_t>(in.gcount()));
    }
    return s.digest();
}

std::string wj_hash_string(std::uint64_t h) {
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<unsigned char>((h >> (8 * i)) & 0xFF);
    std::string out;
    for (int i = 0; i < 8; i += 3) {
        const std::uint32_t v = (b[i] << 16) | (i + 1 < 8 ? b[i + 1] << 8 : 0) | (i + 2 < 8 ? b[i + 2] : 0);
        out.push_back(b64[(v >> 18) & 63]);
        out.push_back(b64[(v >> 12) & 63]);
        out.push_back(i + 1 < 8 ? b64[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < 8 ? b64[v & 63] : '=');
    }
    return out;
}

std::string wj_file_hash(std::string_view path) { return wj_hash_string(xxh64_file(path)); }

}  // namespace mol

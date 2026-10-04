#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "minitest.hpp"
#include "mol/xxh64.hpp"

TEST(xxh64_known_vectors) {
    CHECK_EQ(mol::xxh64(""), std::uint64_t{0xEF46DB3751D8E999ULL});
    CHECK_EQ(mol::xxh64("a"), std::uint64_t{0xD24EC4F1A98C6E5BULL});
    CHECK_EQ(mol::xxh64("abc"), std::uint64_t{0x44BC2CF5AD770999ULL});
    CHECK_EQ(mol::xxh64("Nobody inspects the spammish repetition"), std::uint64_t{0xFBCEA83C8A378BF1ULL});
}

TEST(xxh64_streaming_matches_oneshot_and_wabbajack_string) {
    std::string data;
    for (int i = 0; i < 100003; ++i) data.push_back(static_cast<char>(i * 131 + 7));
    const auto p = std::filesystem::temp_directory_path() / ("mol_xxh_" + std::to_string(::getpid()));
    { std::ofstream(p, std::ios::binary) << data; }
    CHECK_EQ(mol::xxh64_file(p.string()), mol::xxh64(data));
    std::filesystem::remove(p);
    // 8 字节小端 base64：0 → "AAAAAAAAAAA="；已知的 Wabbajack 哈希往返
    CHECK_EQ(mol::wj_hash_string(0), std::string("AAAAAAAAAAA="));
    CHECK_EQ(mol::wj_hash_string(0x0807060504030201ULL), std::string("AQIDBAUGBwg="));
}

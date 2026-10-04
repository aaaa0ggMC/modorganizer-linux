#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "minitest.hpp"
#include "mol/error.hpp"
#include "mol/md5.hpp"

TEST(md5_known_vectors) {
    CHECK_EQ(mol::md5_hex(""), std::string("d41d8cd98f00b204e9800998ecf8427e"));
    CHECK_EQ(mol::md5_hex("abc"), std::string("900150983cd24fb0d6963f7d28e17f72"));
    CHECK_EQ(mol::md5_hex("The quick brown fox jumps over the lazy dog"), std::string("9e107d9d372bb6826bd81d3542a419d6"));
    CHECK_EQ(mol::md5_hex(std::string(1000, 'a')), std::string("cabe45dcc9ae5b66ba86600cca6b8ba8"));
}

TEST(md5_file_matches_memory_hash_across_block_boundaries) {
    const auto p = std::filesystem::temp_directory_path() / ("mol_md5_" + std::to_string(::getpid()));
    std::string data;
    for (int i = 0; i < 200000; ++i) data.push_back(static_cast<char>(i * 31 + 7));
    { std::ofstream(p, std::ios::binary) << data; }
    CHECK_EQ(mol::md5_file(p.string()), mol::md5_hex(data));
    std::filesystem::remove(p);
    bool threw = false;
    try { mol::md5_file("/nonexistent/file"); } catch (const mol::Error&) { threw = true; }
    CHECK(threw);
}

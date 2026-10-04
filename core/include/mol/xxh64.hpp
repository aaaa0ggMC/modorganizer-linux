#pragma once
// xxHash64（seed 0）与 Wabbajack 的哈希表示：8 字节小端 → base64。
#include <cstdint>
#include <string>
#include <string_view>

namespace mol {

std::uint64_t xxh64(std::string_view data, std::uint64_t seed = 0);
// 流式计算文件的 xxh64；打不开 → Error{io_error}。
std::uint64_t xxh64_file(std::string_view path);
// Wabbajack 的 Hash 字符串（8 字节小端的 base64，如 "LqKnmSUrXac="）。
std::string wj_hash_string(std::uint64_t h);
std::string wj_file_hash(std::string_view path);

}  // namespace mol

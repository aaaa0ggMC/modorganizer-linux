#pragma once
// MD5（RFC 1321）：仅用于校验 Nexus 文件/集合清单里的 md5，不用于任何安全用途。
#include <string>
#include <string_view>

namespace mol {

// 十六进制小写。
std::string md5_hex(std::string_view data);
// 流式计算文件的 md5；打不开 → Error{io_error}。
std::string md5_file(std::string_view path);

}  // namespace mol

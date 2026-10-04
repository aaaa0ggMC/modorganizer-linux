#pragma once
// Bethesda BSA 归档（Skyrim SE：版本 105；也能读 104）：读取索引/文件、按给定标志写出。
// 用于 Wabbajack 的 CreateBSA 指令：把散文件按原作者的标志重新打包成 BSA。
//
// 布局（v105）：头 36 字节 | 文件夹记录(24B×n，按哈希升序) | 每个文件夹：bzstring 名 + 文件记录(16B×m，按哈希升序) |
//              文件名块（NUL 结尾，顺序同文件记录）| 文件数据。
// archive_flags：0x1 含目录名 0x2 含文件名 0x4 默认压缩 0x100 数据前内嵌文件名；每个文件可用 flip 翻转压缩。
// 压缩用 LZ4 帧格式：数据 = u32 原始大小 + LZ4 帧。
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "mol/error.hpp"

namespace mol::bsa {

struct Header {
    std::uint32_t version = 105;
    std::uint32_t archive_flags = 0;
    std::uint16_t file_flags = 0;
};

// TES4 系列的 64 位路径哈希。name 须已小写、用反斜杠。is_folder=true 时把整个字符串当作无扩展名的目录路径。
std::uint64_t hash_name(std::string_view name, bool is_folder = false);

struct InputFile {
    std::string path;    // 归档内路径（任意大小写，/ 或 \ 均可）
    std::string source;  // 磁盘上的文件
    bool flip_compression = false;
};

// 写出 BSA。version 必须是 104 或 105。失败 → Error{invalid_argument|io_error}。返回写出的字节数。
std::uint64_t write(std::string_view out_path, const Header& h, const std::vector<InputFile>& files);

struct Entry {
    std::string path;  // 小写、反斜杠
    std::uint64_t hash = 0;
    std::uint32_t size = 0;    // 记录里的大小（含压缩前缀、内嵌名；已去掉 flip 位）
    std::uint32_t offset = 0;
    bool flip = false;
};
struct Index {
    Header header;
    std::vector<Entry> files;
};
// 读索引（需要归档带文件名与目录名标志，否则 Error{invalid_argument}）。
Index read_index(std::string_view bsa_path);
// 读出（并按需解压）一个文件的内容。
std::string read_file(std::string_view bsa_path, const Index& idx, const Entry& e);

}  // namespace mol::bsa

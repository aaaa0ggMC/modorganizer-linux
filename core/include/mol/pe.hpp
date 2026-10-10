#pragma once
// 最小 PE 读取器：machine / 导入表 / 导出表 / 节表。只读、有界、畸形文件返回 nullopt
// （绝不因为一个坏 DLL 让整轮分析失败）。用于模组影响面分析（docs/DESIGN-mod-impact.md）。
// 只支持「文件内偏移即数据目录 RVA 所在节」的常规 PE；加壳/畸形的一律 nullopt，由调用方降级。
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mol/pmr.hpp"

namespace mol::pe {

struct Import {
    std::string dll;             // "kernel32.dll"（保留原始大小写）
    std::string name;            // 按名字导入的函数名；by_ordinal 时为空
    std::uint16_t ordinal = 0;   // 按序号导入时的序号
    bool by_ordinal = false;
};

struct Export {
    std::string name;
    std::uint32_t ordinal = 0;  // 真实序号（base + 表内下标）
    std::uint32_t rva = 0;
};

struct Section {
    std::string name;
    std::uint64_t virtual_size = 0, raw_size = 0;
    bool writable = false, executable = false;
};

struct Info {
    std::string machine;  // "i386" | "amd64" | "arm64" | "unknown"
    std::vector<Import> imports;
    std::vector<Export> exports;
    std::vector<Section> sections;
};

// 解析一个 PE 文件。文件不存在/不是 PE/目录越界/条目数超限 → nullopt。
std::optional<Info> parse(std::string_view path);

}  // namespace mol::pe

#pragma once
// 按 Wabbajack 清单重建实例目录（即 MO2 便携实例：mods/、profiles/、ModOrganizer.ini …）。
// 可续跑、幂等：每个压缩包处理完就记入 <输出>/.mol-wabbajack/state.json；需要人介入的项记为 pending 并继续其它项。
// 暂不支持的指令（CreateBSA / TransformedTexture / MergedPatch / 其它）不会被执行，只计数并作为 pending(kind=unsupported) 报告。
#include <functional>
#include <string>
#include <vector>

#include "mol/nexus.hpp"
#include "mol/wabbajack.hpp"

namespace mol::wabbajack {

struct InstallOptions {
    std::string output_dir;     // 实例目录（Directive.To 相对它）
    std::string downloads_dir;  // 空 → <output>/downloads
    std::string game_dir;       // 真实游戏目录（GameFileSource 与路径占位符用）
    unsigned jobs = 0;  // 并行下载数（0 = MOL_JOBS 环境变量或默认 4）
    const NexusClient* client = nullptr;  // 空 → Nexus 来源的压缩包只能用本地已有文件
    // stage ∈ "download"（所有并行下载的合计字节）| "downloaded"（第几个下载完）| "extract" | "archive"（第几个/总数）
    std::function<void(std::string_view stage, std::string_view name, std::uint64_t done, std::uint64_t total)> progress;
};

struct Pending {
    std::string kind;    // manual_download | unsupported | game_file_missing
    std::string name;    // 压缩包名或指令类型
    std::string detail;
    std::string url;
    std::int64_t count = 0;  // unsupported：受影响的指令数
};

struct Report {
    std::int64_t archives_total = 0, archives_done = 0, files_written = 0, files_failed = 0;
    std::vector<Pending> pending;
    std::vector<std::string> failures;  // 非致命失败（hash 不符、缺文件……），每条一句话
    bool complete() const { return pending.empty() && failures.empty(); }
};

// wabbajack_file：.wabbajack 路径（内联数据与补丁从中取）。输出目录不存在会创建。
Report install_modlist(const Modlist& list, const std::string& wabbajack_file, const InstallOptions& opt);

// 路径占位符替换（RemappedInlineFile 用）。unix_path 是本机路径；返回替换后的文本。
std::string remap_placeholders(std::string_view text, std::string_view game_dir, std::string_view install_dir, std::string_view downloads_dir);

}  // namespace mol::wabbajack

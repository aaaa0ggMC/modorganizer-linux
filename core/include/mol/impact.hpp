#pragma once
// 模组影响面分析：注入地点 → 作用域 + 能力面（docs/DESIGN-mod-impact.md）。
// 纯只读静态分析；回答「这个模组*能*碰什么」，**不**回答「谁是肇事者」——
// 影响 ≠ 责任，结果永远不作为自动停用模组的依据。
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mol/instance.hpp"
#include "mol/pe.hpp"
#include "mol/pmr.hpp"

namespace mol::impact {

// 注入地点：一个模组文件以什么方式进入游戏/系统。
struct InjectionPoint {
    string kind;        // skse_plugin | engine_dll | proxy_dll | exe_tool | papyrus | content | config
    string path;        // 相对 mod 根的路径（'/')
    string loaded_by;   // skse | windows_loader | engine | user | game_vm | game
    string reach;       // game-process | all-processes | offline | game-logic | game-content
};

// 能力面：从 PE 导入表归类出的**上界**（GetProcAddress/加壳会隐藏真实行为）。
struct Capabilities {
    bool writes_files = false;
    bool spawns_processes = false;
    bool network = false;
    bool registry = false;
    bool memory_patch = false;
    bool chain_loads = false;  // LoadLibrary：可能把别的 DLL 带进来
    bool unknown = false;      // 没有导入表 / 疑似加壳：能力不可静态判断
    std::vector<std::string> evidence;  // "kernel32!WriteFile" 这样的证据（≤16 条）
};

struct ModImpact {
    string mod;
    std::vector<InjectionPoint> injections;
    Capabilities caps;
    bool packed_suspect = false;  // 可写+可执行节 / 无导入表 / 已知加壳节名
    string summary;               // 一句人话
};

// 分析一个模组（按名字，大小写不敏感）。不在实例里 → Error{mod_not_found}；
// 目录为空 → 空结果（不是错误）。
ModImpact analyze_mod(const Instance& inst, std::string_view mod, mr* mem = default_mr());
// 批量；顺序与入参一致。单个文件解析失败只降级该条目，不影响其它。
std::vector<ModImpact> analyze_mods(const Instance& inst, std::span<const string> mods, mr* mem = default_mr());

}  // namespace mol::impact

// mods impact：每个模组的注入地点与影响范围（docs/DESIGN-mod-impact.md）。
// 纯只读静态分析。影响 ≠ 责任：结果只说明「能碰什么」，永远不据此自动停用模组。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "mol/impact.hpp"
#include "mol/instance.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {
Result run_mods_impact(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    if (auto bad = check_positionals(ctx, 1, "NAME")) return *bad;
    const mol::Instance inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const std::string name(ctx.args.positionals.at(0));
    const auto m = mol::impact::analyze_mod(inst, name, ctx.mem);

    ImpactData d{.mod = mol::string(m.mod, ctx.mem),
                 .summary = mol::string(m.summary, ctx.mem),
                 .packed_suspect = m.packed_suspect,
                 .injections = std::pmr::vector<ImpactInjectionRow>(ctx.mem),
                 .caps = ImpactCaps{},
                 .evidence = std::pmr::vector<std::pmr::string>(ctx.mem)};
    for (const auto& i : m.injections)
        d.injections.push_back(ImpactInjectionRow{.kind = mol::string(i.kind, ctx.mem),
                                                  .path = mol::string(i.path, ctx.mem),
                                                  .loaded_by = mol::string(i.loaded_by, ctx.mem),
                                                  .reach = mol::string(i.reach, ctx.mem)});
    d.caps.writes_files = m.caps.writes_files;
    d.caps.spawns_processes = m.caps.spawns_processes;
    d.caps.network = m.caps.network;
    d.caps.registry = m.caps.registry;
    d.caps.memory_patch = m.caps.memory_patch;
    d.caps.chain_loads = m.caps.chain_loads;
    d.caps.unknown = m.caps.unknown;
    for (const auto& e : m.caps.evidence) d.evidence.push_back(std::pmr::string(e, ctx.mem));

    Result r(ctx.mem);
    r.ok = true;
    r.command = ctx.command;
    r.set_data(std::move(d));
    return r;
}

}  // namespace cli

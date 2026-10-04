// envelope 序列化（alib6 反射 + 紧凑 JSON）与文本渲染。
//
// 本文件不 #include 任何标准库头：import std / import alib6 之后即可（混用约束见 cmd_common.hpp）。
#include "output.hpp"

import alib6;
import std;

namespace cli {

using namespace alib6;

namespace {

// 文本渲染用：AData 里按 key 取字符串（缺失 → 空）
std::string S(const alib6::AData& node, std::string_view key) {
    return std::string(adata::str(node, key));
}

void add(std::pmr::vector<std::pmr::string>& lines, const std::string& text, mol::mr* mem) {
    lines.push_back(mol::string(std::string_view(text), mem));
}

void render_instance_show(const alib6::AData& d, std::pmr::vector<std::pmr::string>& lines,
                          mol::mr* mem) {
    add(lines, "instance " + S(d, "root"), mem);
    for (const char* key : {"mods_dir", "profiles_dir", "downloads_dir", "overwrite_dir",
                            "farm_path"}) {
        add(lines, std::string(key) + ": " + S(d, key), mem);
    }
    add(lines, "config:", mem);
    if (const alib6::AData* cfg = adata::field(d, "config")) {
        if (cfg->is_object()) {
            for (auto it : cfg->object()) {
                add(lines, "  " + std::string(it.first()) + ": " + S(*cfg, it.first()), mem);
            }
        }
    }
}

void render_mods_list(const alib6::AData& d, std::pmr::vector<std::pmr::string>& lines, mol::mr* mem) {
    const alib6::AData* mods = adata::field(d, "mods");
    const std::size_t n = mods == nullptr ? 0 : adata::size(*mods);
    add(lines, "profile " + S(d, "profile") + ": " + std::to_string(n) + " mod(s)", mem);
    if (mods == nullptr) return;
    for (std::size_t i = 0; i < n; ++i) {
        const alib6::AData* m = adata::at(*mods, i);
        if (m == nullptr) continue;
        const bool enabled = adata::boolean(*m, "enabled");
        const bool separator = adata::boolean(*m, "separator");
        const bool exists = adata::boolean(*m, "exists");
        const long long priority = adata::integer(*m, "priority");
        char tag[16];
        std::snprintf(tag, sizeof(tag), "%03lld", priority);
        std::string line = "  [" + std::string(enabled ? "x" : " ") + "] " + tag + " " + S(*m, "name");
        if (separator) line += " (separator)";
        if (!exists && !separator) line += " (missing)";
        const std::string path = S(*m, "path");
        if (!path.empty()) line += " -> " + path;
        add(lines, line, mem);
    }
}

void render_conflicts(const alib6::AData& d, std::pmr::vector<std::pmr::string>& lines, mol::mr* mem) {
    const alib6::AData* list = adata::field(d, "conflicts");
    const std::size_t n = list == nullptr ? 0 : adata::size(*list);
    add(lines, std::to_string(adata::integer(d, "count")) + " conflict(s) over " +
                   std::to_string(n) + " path(s)",
        mem);
    if (list == nullptr) return;
    std::size_t shown = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (shown++ >= 20) {
            add(lines, "  ... (truncated)", mem);
            break;
        }
        const alib6::AData* c = adata::at(*list, i);
        if (c == nullptr) continue;
        std::string line = "  " + S(*c, "path") + " -> " + S(*c, "winner") + " (over:";
        if (const alib6::AData* losers = adata::field(*c, "losers")) {
            const std::size_t ln = adata::size(*losers);
            for (std::size_t j = 0; j < ln; ++j) {
                const alib6::AData* l = adata::at(*losers, j);
                line += (j == 0 ? " " : ",");
                if (l != nullptr && l->is_value()) line += std::string(l->value().to<std::string_view>());
            }
        }
        line += ")";
        add(lines, line, mem);
    }
}

}  // namespace

Envelope make_envelope(const Result& r, mol::mr* mem) {
    Envelope env{mol::allocator_type(mem)};
    env.schema_version = 1;
    env.ok = r.ok;
    env.command = r.command;
    env.data = r.data;  // AData 拷贝（ok=false 时 r.data 为 null）
    env.errors = r.errors;
    env.warnings = r.warnings;
    return env;
}

std::pmr::string serialize_envelope(const Result& r, mol::mr* mem) {
    const Envelope env = make_envelope(r, mem);
    AData doc = to_adata(env, mem);

    JSONConfig cfg;
    cfg.compact_lines = true;   // 单行
    cfg.compact_spaces = true;  // 去掉 ": " / ", " 的空格
    cfg.sort_object = JSONConfig::sort_asc;  // 键按字典序 ⇒ 输出确定
    JSON json(cfg);
    std::string out;
    json.dump(out, doc);
    return mol::string(out, mem);
}

std::pmr::vector<std::pmr::string> render_text(const Result& r, mol::mr* mem) {
    std::pmr::vector<std::pmr::string> lines(mem);
    if (!r.ok) return lines;  // 失败只在 stderr 输出诊断（render_diagnostics）

    const std::string cmd = std::string(r.command);
    if (cmd == "version") {
        add(lines, S(r.data, "name") + " " + S(r.data, "version"), mem);
    } else if (cmd == "game info") {
        add(lines, S(r.data, "name") + " " + S(r.data, "version"), mem);
        add(lines, "game " + S(r.data, "gameDirectory"), mem);
        add(lines, "data " + S(r.data, "dataDirectory"), mem);
        add(lines, "binary " + S(r.data, "binaryName"), mem);
    } else if (cmd == "plugins sync") {
        add(lines, std::string("plugins sync ") + (adata::boolean(r.data, "changed") ? "(changed)" : "(unchanged)"), mem);
    } else if (cmd == "instance init") {
        const bool changed = adata::boolean(r.data, "changed");
        add(lines, "instance initialized at " + S(r.data, "root") +
                       (changed ? " (changed)" : " (unchanged)"),
            mem);
    } else if (cmd == "instance show") {
        render_instance_show(r.data, lines, mem);
    } else if (cmd == "mods list") {
        render_mods_list(r.data, lines, mem);
    } else if (cmd == "mods enable" || cmd == "mods disable") {
        const bool enabled = adata::boolean(r.data, "enabled");
        const bool changed = adata::boolean(r.data, "changed");
        add(lines, "mod '" + S(r.data, "name") + "' " + (enabled ? "enabled" : "disabled") +
                       (changed ? " (changed)" : " (unchanged)"),
            mem);
    } else if (cmd == "mods move") {
        const bool changed = adata::boolean(r.data, "changed");
        add(lines, "mod '" + S(r.data, "name") + "' moved to priority " +
                       std::to_string(adata::integer(r.data, "priority")) +
                       (changed ? " (changed)" : " (unchanged)"),
            mem);
    } else if (cmd == "conflicts") {
        render_conflicts(r.data, lines, mem);
    } else if (cmd == "plan") {
        std::string line = "plan: " + std::to_string(adata::integer(r.data, "count")) + " op(s) [";
        if (const alib6::AData* counts = adata::field(r.data, "counts")) {
            line += "mkdir=" + std::to_string(adata::integer(*counts, "mkdir")) +
                    " link=" + std::to_string(adata::integer(*counts, "link")) +
                    " relink=" + std::to_string(adata::integer(*counts, "relink")) +
                    " remove=" + std::to_string(adata::integer(*counts, "remove")) +
                    " rmdir=" + std::to_string(adata::integer(*counts, "rmdir"));
        }
        line += "], warnings=" + std::to_string(adata::integer(r.data, "warnings"));
        add(lines, line, mem);
        if (const alib6::AData* ops = adata::field(r.data, "ops")) {
            const std::size_t n = adata::size(*ops);
            std::size_t shown = 0;
            for (std::size_t i = 0; i < n; ++i) {
                if (shown++ >= 20) {
                    add(lines, "  ... (truncated)", mem);
                    break;
                }
                const alib6::AData* op = adata::at(*ops, i);
                if (op == nullptr) continue;
                std::string line2 = "  " + S(*op, "kind") + " " + S(*op, "path");
                const std::string target = S(*op, "target");
                if (!target.empty()) line2 += " -> " + target;
                add(lines, line2, mem);
            }
        }
    } else if (cmd == "status") {
        if (adata::boolean(r.data, "in_sync")) {
            add(lines, "in sync", mem);
        } else {
            add(lines, "drift: " + std::to_string(adata::integer(r.data, "pending")) +
                           " pending op(s)",
                mem);
        }
        add(lines, "farm " + S(r.data, "farm_path") +
                       (adata::boolean(r.data, "farm_exists") ? " (present)" : " (absent)"),
            mem);
    } else if (cmd == "apply") {
        const bool changed = adata::boolean(r.data, "changed");
        add(lines, "applied " + std::to_string(adata::integer(r.data, "applied")) + " op(s)" +
                       (changed ? " (changed)" : " (nothing changed)"),
            mem);
        add(lines, "farm " + S(r.data, "farm_path"), mem);
    } else if (cmd == "unlink") {
        add(lines, adata::boolean(r.data, "removed") ? "farm removed" : "farm already absent", mem);
        add(lines, "farm " + S(r.data, "farm_path"), mem);
    } else {
        add(lines, cmd + ": ok", mem);
    }
    return lines;
}

std::pmr::vector<std::pmr::string> render_diagnostics(const Result& r, mol::mr* mem) {
    std::pmr::vector<std::pmr::string> lines(mem);
    for (const Err& w : r.warnings) {
        std::string line = "warning[" + std::string(w.code) + "]: " + std::string(w.message);
        if (!w.path.empty()) line += " (" + std::string(w.path) + ")";
        add(lines, line, mem);
    }
    for (const Err& e : r.errors) {
        std::string line = "error[" + std::string(e.code) + "]: " + std::string(e.message);
        if (!e.path.empty()) line += " (" + std::string(e.path) + ")";
        add(lines, line, mem);
    }
    return lines;
}

}  // namespace cli

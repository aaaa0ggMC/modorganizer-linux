// alib6 CommandInput → 内部类型的适配器实现。
//
// 所有 #include 必须排在 import 之前（GCC 16：import std/alib6 与文本 include 标准库头
// 在同一 TU 会互相重定义，详见 cli/cmd_common.hpp 文件头）。
#include <string>
#include <string_view>

#include "parse.hpp"

import std;

namespace cli {

namespace {

// 一个选项在 CommandInput 的 found_options 层里的出现情况
struct Found {
    bool present = false;   // 键出现在某一层（哪怕值无效）
    bool valid = false;     // 值有效（不是「缺值」）
    std::string_view value;
};

// found_options 用于区分“未提供”和“提供但缺值”；值由 CommandInput::get
// 选择，保持 alib6 的层级/别名优先级，不依赖 unordered_map 遍历顺序。
template <class Spec>
Found find_option(const alib6::Command::CommandInput& in, const Spec& spec) {
    Found f;
    for (const auto& layer : in.found_options) {
        if (layer.contains(spec.name) ||
            (!spec.long_name.empty() && layer.contains(spec.long_name)) ||
            (!spec.short_name.empty() && layer.contains(spec.short_name))) {
            f.present = true;
            break;
        }
    }
    const auto value = in.get(spec.name);
    f.valid = static_cast<bool>(value);
    f.value = value.view();
    return f;
}

// 值看起来是另一个选项（`-i -j` 这种缺值写法）
bool looks_like_option(std::string_view v) { return v.size() > 1 && v[0] == '-'; }

// alib6 的 remains 保留原 parser.args 字符串的视图。只用地址身份定位
// 终止符前后，不解析选项或值；后面的 -- 不能放行它前面的未知选项。
bool after_dashdash(const alib6::Command::CommandInput& in, std::string_view token) {
    bool terminated = false;
    for (const auto& raw : in.cmd.parser.args) {
        if (raw.data() == token.data() && raw.size() == token.size()) return terminated;
        if (raw == "--") terminated = true;
    }
    return false;
}

bool toggle_present(const alib6::Command::CommandInput& in, std::string_view canonical_name) {
    // has() 会按注册表的别名扫描所有层（含根层预扫描的结果）
    return in.has(canonical_name);
}

// 规范名是否「本命令可用」：全局选项 + 全局开关 + 该命令的专有选项
bool name_allowed(std::string_view canonical_name, std::span<const OptionSpec> extra) {
    for (const auto& s : kTogGlobal) {
        if (s.name == canonical_name) return true;
    }
    for (const auto& s : kOptGlobal) {
        if (s.name == canonical_name) return true;
    }
    for (const auto& s : extra) {
        if (s.name == canonical_name) return true;
    }
    return false;
}

std::string join_routes(const alib6::Command::CommandInput& in) {
    std::string out;
    for (const auto r : in.routes) {
        if (!out.empty()) out += " ";
        out += std::string(r);
    }
    return out;
}

}  // namespace

mol::string build_global_options(const alib6::Command::CommandInput& in, GlobalOptions& g,
                                 mol::mr* mem) {
    g.json = toggle_present(in, "json");
    g.quiet = toggle_present(in, "quiet");
    for (const auto& spec : kOptGlobal) {
        const Found f = find_option(in, spec);
        if (!f.present) continue;
        if (!f.valid || f.value.empty() || looks_like_option(f.value)) {
            return mol::string("missing value for option " + std::string(spec.long_name), mem);
        }
        if (spec.name == "instance") {
            g.instance = mol::string(f.value, mem);
            g.has_instance = true;
        } else if (spec.name == "profile") {
            g.profile = mol::string(f.value, mem);
            g.has_profile = true;
        } else if (spec.name == "events") {
            g.events = mol::string(f.value, mem);
            g.has_events = true;
        }
    }
    return {};
}

void build_parsed_args(const alib6::Command::CommandInput& in, std::span<const OptionSpec> specs,
                       ParsedArgs& out, mol::mr* mem) {
    out = ParsedArgs{mol::allocator_type(mem)};
    const std::string cmd = join_routes(in);

    // 1. 位置参数：remains 里已没有注册选项（alib6 提取掉了）。剩下的 "-x" 只可能是
    //    未注册的选项 → 用法错误（而不是当成 mod 名之类的静默执行）。
    //    -- 后的 token 才是字面位置参数。
    for (const auto tok : in.args()) {
        if (looks_like_option(tok) && !after_dashdash(in, tok)) {
            out.error = mol::string("unknown option " + std::string(tok), mem);
            return;
        }
        out.positionals.push_back(mol::string(tok, mem));
    }

    // 2. 本命令允许的选项：缺值检查 + 收进 options
    for (const auto& spec : specs) {
        if (!spec.takes_value) {  // 开关：alib6 按注册别名判断是否出现
            if (toggle_present(in, spec.name))
                out.options.emplace_back(mol::string(spec.long_name, mem), mol::string(mem));
            continue;
        }
        const Found f = find_option(in, spec);
        if (!f.present) continue;
        if (spec.takes_value) {
            if (!f.valid || f.value.empty() || looks_like_option(f.value)) {
                out.error = mol::string("missing value for option " + std::string(spec.long_name),
                                        mem);
                return;
            }
        }
        out.options.emplace_back(mol::string(spec.long_name, mem),
                                 mol::string(spec.takes_value ? f.value : std::string_view(), mem));
    }

    // 3. 注册过、但不属于本命令的选项/开关 → 用法错误
    //    （例如 `mods list --game-dir /x`：值已被 alib6 提取，不会留在 remains 里）
    for (const auto& tog : in.cmd.registered_toggles) {
        if (name_allowed(tog.name, specs)) continue;
        if (toggle_present(in, tog.name)) {
            out.error = mol::string("unknown option " + std::string(tog.long_name) + " for " + cmd, mem);
            return;
        }
    }
    for (const auto& opt : in.cmd.registered_options) {
        if (name_allowed(opt.name, specs)) continue;
        if (find_option(in, opt).present) {
            out.error = mol::string("unknown option " + std::string(opt.long_name) + " for " + cmd,
                                    mem);
            return;
        }
    }
    for (const auto& tog : in.cmd.registered_toggles) {
        if (name_allowed(tog.name, specs)) continue;
        if (toggle_present(in, tog.name)) {
            out.error = mol::string("unknown option " + std::string(tog.long_name) + " for " + cmd,
                                    mem);
            return;
        }
    }
}

}  // namespace cli

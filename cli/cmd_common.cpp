// cmd_common 的实现：Result 构造、mol::Error → envelope errors 映射、AData 只读访问。
//
// 注意混用约束（见 cmd_common.hpp 文件头）：所有 #include 在 import 之前。
#include <exception>
#include <string>

#include "cmd_common.hpp"

namespace cli {

std::string_view default_hint(std::string_view code) {
    struct H { std::string_view code, hint; };
    static constexpr H table[] = {
        {"instance_not_found", "create one with `mo-linux instance init` (Steam paths are auto-detected), or pass -i DIR / set MOL_INSTANCE"},
        {"config_invalid", "fix or delete the named file; `mo-linux doctor` shows what is wrong"},
        {"profile_not_found", "create it with `mo-linux instance init -p NAME`"},
        {"mod_not_found", "list what exists with `mods list` / `plugins list`"},
        {"invalid_argument", "see the command's options with `mo-linux schema`"},
        {"farm_not_owned", "the farm directory holds files mo-linux did not create; empty it or choose another farm_dir"},
        {"farm_conflict", "a real file sits where a link must go; run `mo-linux overwrite capture`, or remove the file"},
        {"farm_busy", "the game (or a leftover Wine process) is using the farm: ask the user to close the game, or run `mo-linux terminate` to end everything using this instance, then retry"},
        {"wine_busy", "another Wine program in this prefix is still running (or just exited); wait a few seconds and retry, or end it with `mo-linux terminate`. `--no-cow` runs without protection (writes change the original files)"},
        {"io_error", "check the path named in the error (permissions, disk space)"},
        {"game_unavailable", "libmo-game.so was not found or failed to load: set MOL_GAME_LIB or build with MOL_BUILD_HOST=ON"},
        {"network_error", "check the network (set https_proxy if needed) and retry"},
        {"nexus_auth", "ask the user for a Nexus personal API key (nexusmods.com/users/myaccount?tab=api) and run `mo-linux nexus login`; never print or store the key anywhere else"},
        {"nexus_premium", "a free Nexus account cannot download directly: ask the user for an nxm:// link and use `nexus download --nxm LINK`"},
        {"nexus_not_found", "check the id/slug with `nexus search` / `collection search`"},
        {"nexus_rate_limited", "wait before retrying (see the message); do not loop"},
        {"skse_mismatch", "the newest SKSE64 does not support this game version yet; wait for an update or ask the user to downgrade the game"},
        {"prefix_unhealthy", "run `mo-linux doctor` and apply the fix it names (e.g. `fix vcrun`, `fix content-catalog`); `run --force` launches anyway"},
        {"fomod_choices_required", "run `mo-linux fomod inspect ARCHIVE`, decide the choices, then pass --fomod FILE (or --fomod-defaults / --no-fomod)"},
    };
    for (const auto& h : table)
        if (h.code == code) return h.hint;
    return {};
}

void Result::add_error(std::string_view code, std::string_view message, std::string_view path) {
    auto* mr = errors.get_allocator().resource();
    errors.push_back(Err{mol::string(code, mr), mol::string(message, mr), mol::string(path, mr), mol::string(default_hint(code), mr)});
}

void Result::add_warning(std::string_view code, std::string_view message, std::string_view path) {
    auto* mr = warnings.get_allocator().resource();
    warnings.push_back(Err{mol::string(code, mr), mol::string(message, mr), mol::string(path, mr), mol::string(mr)});
}

Result make_ok(Context& ctx) {
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.data.set_null();
    return r;
}

Result make_usage_error(std::string_view message, Context& ctx) {
    Result r(ctx.mem);
    r.ok = false;
    r.exit_code = 2;
    r.command = ctx.command;
    r.add_error("invalid_argument", message, "");
    return r;
}

Result result_from_mol_error(const mol::Error& e, Context& ctx) {
    Result r(ctx.mem);
    r.ok = false;
    r.command = ctx.command;
    r.add_error(e.code, e.what(), e.path);
    // 只有 invalid_argument 对应退出码 2，其它运行期错误都是 1
    r.exit_code = 2;
    for (const auto& err : r.errors) {
        if (err.code != "invalid_argument") {
            r.exit_code = 1;
            break;
        }
    }
    return r;
}

Result result_from_std_exception(const std::exception& e, Context& ctx) {
    Result r(ctx.mem);
    r.ok = false;
    r.exit_code = 1;
    r.command = ctx.command;
    r.add_error("io_error", e.what(), "");
    return r;
}

int exit_code_from_issues(const std::pmr::vector<Err>& errors) {
    if (errors.empty()) return 0;
    for (const auto& e : errors) {
        if (e.code == "invalid_argument") return 2;
    }
    return 1;
}

std::optional<Result> check_positionals(Context& ctx, std::size_t expected,
                                        std::string_view arg_name) {
    const std::size_t n = ctx.args.positionals.size();
    if (n == expected) return std::nullopt;
    if (n == 0 && arg_name.starts_with('[')) return std::nullopt;  // "[NAME]" = 可省略
    if (n > expected) {
        const std::string extra(ctx.args.positionals[expected]);
        return make_usage_error(std::string(ctx.command) + ": unexpected argument '" + extra + "'",
                                ctx);
    }
    if (expected == 1) {
        return make_usage_error(std::string(ctx.command) + ": " + std::string(arg_name) +
                                    " is required",
                                ctx);
    }
    return make_usage_error(std::string(ctx.command) + ": unexpected argument", ctx);
}

Result result_from_unknown_command(Context& ctx, std::string_view line) {
    Result r(ctx.mem);
    r.ok = false;
    r.exit_code = 2;
    r.command = ctx.command;
    r.add_error("invalid_argument", "unknown command: " + std::string(line), "");
    return r;
}

// ---------------------------------------------------------------------------
// AData 只读访问
// ---------------------------------------------------------------------------
namespace adata {

const alib6::AData* field(const alib6::AData& node, std::string_view key) {
    if (!node.is_object()) return nullptr;
    return node.object().at_ptr(key);
}

const alib6::AData* at(const alib6::AData& arr, std::size_t index) {
    if (!arr.is_array()) return nullptr;
    return arr.array().at_ptr(static_cast<std::ptrdiff_t>(index));
}

std::string_view str(const alib6::AData& node, std::string_view key) {
    const alib6::AData* v = field(node, key);
    if (v == nullptr || !v->is_value()) return {};
    const auto& val = v->value();
    if (val.get_type() != alib6::Value::STRING) return {};
    return val.to<std::string_view>();
}

long long integer(const alib6::AData& node, std::string_view key) {
    const alib6::AData* v = field(node, key);
    if (v == nullptr || !v->is_value()) return 0;
    const auto& val = v->value();
    if (val.get_type() == alib6::Value::BOOL) return val.to<bool>() ? 1 : 0;
    if (val.get_type() != alib6::Value::INT) return 0;
    return val.to<long long>();
}

bool boolean(const alib6::AData& node, std::string_view key) {
    const alib6::AData* v = field(node, key);
    if (v == nullptr || !v->is_value()) return false;
    const auto& val = v->value();
    if (val.get_type() == alib6::Value::BOOL) return val.to<bool>();
    if (val.get_type() == alib6::Value::INT) return val.to<long long>() != 0;
    return false;
}

std::size_t size(const alib6::AData& node) {
    if (!node.is_array()) return 0;
    return node.array().size();
}

}  // namespace adata

}  // namespace cli

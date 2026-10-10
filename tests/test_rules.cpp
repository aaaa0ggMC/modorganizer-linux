// Lua 规则层运行时 API 的离线测试（tests/test_rules.cpp）。
// 只测 mol/rules.hpp 的公共契约：evaluate / describe / collect_context。
// 所有夹具除 collect_context 一项外全部在内存里构造；collect_context 用 /tmp 下的
// mkdtemp 临时实例，结束即清理，不访问真实 HOME 或用户配置。
// 运行期行为（sandbox 边界、限额、动作门控）以 core/src/rules.cpp 的当前实现为准。
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "minitest.hpp"
#include "mol/rules.hpp"

namespace fs = std::filesystem;
using namespace mol;
namespace rules = mol::rules;

namespace {
// ---- 只读断言辅助 -----------------------------------------------------------
const Check* find_check(const vector<Check>& cs, std::string_view id) {
    for (const auto& c : cs)
        if (c.id == id) return &c;
    return nullptr;
}
bool has_check(const vector<Check>& cs, std::string_view id) { return find_check(cs, id) != nullptr; }
// 运行期把失败/超额的规则折叠成一条 id="lua.runtime"、level="warn" 的检查。
bool any_runtime_warn(const vector<Check>& cs) {
    for (const auto& c : cs)
        if (c.id == "lua.runtime" && c.level == "warn") return true;
    return false;
}
std::size_t count_prefix(const vector<Check>& cs, const std::string& pfx) {
    std::size_t n = 0;
    for (const auto& c : cs) {
        std::string id(c.id);
        if (id.size() >= pfx.size() && id.compare(0, pfx.size(), pfx) == 0) ++n;
    }
    return n;
}
// ---- 内存夹具：Context 的 facts / mods --------------------------------------
void add_fact(rules::Context& ctx, std::string_view k, std::string_view v) {
    rules::Fact f;
    f.key.assign(k);
    f.value.assign(v);
    ctx.facts.push_back(std::move(f));
}
void add_mod(rules::Context& ctx, std::string_view name, bool enabled, bool exists,
             std::int64_t nexus = 0, std::string_view version = {}) {
    ModInfo m;
    m.name.assign(name);
    m.enabled = enabled;
    m.exists = exists;
    m.nexus_id = nexus;
    m.version.assign(version);
    ctx.mods.push_back(std::move(m));
}
}  // namespace

// 合法脚本产出一条检查，id 规整为 lua.<ruleid>.<id>。
TEST(evaluate_emits_valid_check_with_prefixed_id) {
    rules::Context ctx;
    ctx.game = "skyrimse";
    ctx.game_version = "1.6.1170.0";
    const std::string text = R"(return {
        api_version = 1,
        id = 'demo.rule',
        check = function(c)
            return {{ id = 'hello', level = 'ok', message = 'all good' }}
        end,
    })";
    rules::Source srcs[] = {{"demo", text}};
    auto rows = rules::evaluate(srcs, ctx);
    CHECK_EQ(rows.size(), std::size_t{1});
    CHECK(rows[0].id == "lua.demo.rule.hello");
    CHECK(rows[0].level == "ok");
    CHECK_EQ(std::string(rows[0].message), std::string("all good"));
}

// ctx 暴露 game / game_version / facts[key] / mods[i]{name,version,enabled,exists,nexus_id}。
TEST(context_exposes_game_version_facts_and_mods) {
    rules::Context ctx;
    ctx.game = "skyrimse";
    ctx.game_version = "1.6.1170.0";
    add_fact(ctx, "preferences.allow_disable_mod", "true");
    add_mod(ctx, "Get Lost", /*enabled=*/true, /*exists=*/true, /*nexus=*/119736, "1.2");
    const std::string text = R"(return {
        api_version = 1, id = 'ctx',
        check = function(c)
            local f = c.facts['preferences.allow_disable_mod']
            local m = c.mods[1]
            local s = c.game .. '|' .. c.game_version .. '|' .. tostring(f) .. '|'
                   .. m.name .. '|' .. tostring(m.enabled) .. '|' .. tostring(m.exists)
                   .. '|' .. tostring(m.nexus_id) .. '|' .. m.version
            return {{ id = 'snap', level = 'ok', message = s }}
        end,
    })";
    rules::Source srcs[] = {{"ctx", text}};
    auto rows = rules::evaluate(srcs, ctx);
    CHECK_EQ(rows.size(), std::size_t{1});
    CHECK_EQ(std::string(rows[0].message),
             std::string("skyrimse|1.6.1170.0|true|Get Lost|true|true|119736|1.2"));
}

// 非法 API/schema（版本号、返回类型、level、缺 check）都折叠成单条 lua.runtime 警告。
TEST(invalid_schema_or_api_returns_runtime_warning) {
    rules::Context ctx;
    const char* bad[] = {
        R"(return {api_version = 2, id = 'x', check = function(c) return {} end})",
        R"(return {api_version = 1, id = 'y', check = function(c) return 42 end})",
        R"(return {api_version = 1, id = 'z', check = function(c) return {{id='a',level='critical',message='m'}} end})",
        R"(return {api_version = 1, id = 'w'})",
    };
    for (const char* text : bad) {
        rules::Source srcs[] = {{"bad", text}};
        auto rows = rules::evaluate(srcs, ctx);
        CHECK_EQ(rows.size(), std::size_t{1});
        CHECK(rows[0].id == "lua.runtime");
        CHECK(rows[0].level == "warn");
    }
}

// 无限循环被指令预算截断 → 警告，不挂死。
TEST(infinite_loop_is_bounded_to_a_runtime_warning) {
    rules::Context ctx;
    rules::Limits lim;
    lim.instructions = 100000;
    const std::string text = R"(return {
        api_version = 1, id = 'loop',
        check = function(c) local i = 0 while true do i = i + 1 end end,
    })";
    rules::Source srcs[] = {{"loop", text}};
    auto rows = rules::evaluate(srcs, ctx, lim);
    CHECK(any_runtime_warn(rows));
    CHECK_EQ(count_prefix(rows, "lua.loop."), std::size_t{0});
}

// 内存炸弹被分配器上限截断 → 警告，不 OOM 崩溃。
TEST(memory_bomb_is_bounded_to_a_runtime_warning) {
    rules::Context ctx;
    rules::Limits lim;
    lim.memory_bytes = 1024 * 1024;
    lim.instructions = 4000000;  // 让内存上限先于指令预算触发
    const std::string text = R"(return {
        api_version = 1, id = 'bomb',
        check = function(c) local t = {} while true do t[#t + 1] = string.rep('x', 4096) end end,
    })";
    rules::Source srcs[] = {{"bomb", text}};
    auto rows = rules::evaluate(srcs, ctx, lim);
    CHECK(any_runtime_warn(rows));
    CHECK_EQ(count_prefix(rows, "lua.bomb."), std::size_t{0});
}

// 隔离环境不含 io/os/package/debug，也不含 load/dofile/require。
TEST(sandbox_has_no_io_os_package_debug_or_loaders) {
    rules::Context ctx;
    const std::string text = R"(return {
        api_version = 1, id = 'sandbox',
        check = function(c)
            local names = {'io', 'os', 'package', 'debug', 'load', 'dofile', 'require'}
            local out = {}
            for i, n in ipairs(names) do out[i] = n .. '=' .. tostring(_G[n] == nil) end
            return {{ id = 'absent', level = 'ok', message = table.concat(out, ',') }}
        end,
    })";
    rules::Source srcs[] = {{"sandbox", text}};
    auto rows = rules::evaluate(srcs, ctx);
    CHECK_EQ(rows.size(), std::size_t{1});
    CHECK_EQ(std::string(rows[0].message),
             std::string("io=true,os=true,package=true,debug=true,load=true,dofile=true,require=true"));
}

// 超过 source_bytes 上限的脚本被拒 → 警告，不产出检查。
TEST(oversized_source_is_rejected) {
    rules::Context ctx;
    rules::Limits lim;
    lim.source_bytes = 64;
    const std::string big =
        R"(return {api_version=1,id='big',check=function(c) return {{id='ok',level='ok',message='this message is comfortably longer than the sixty four byte source limit'}} end})";
    CHECK(big.size() > lim.source_bytes);
    rules::Source srcs[] = {{"big", big}};
    auto rows = rules::evaluate(srcs, ctx, lim);
    CHECK(any_runtime_warn(rows));
    CHECK_EQ(count_prefix(rows, "lua.big."), std::size_t{0});
}

// 单条规则的检查数超过 checks 上限 → 整条折叠成警告；恰好等于上限则全部保留。
TEST(check_count_limit_is_enforced) {
    rules::Context ctx;
    rules::Limits lim;
    lim.checks = 2;
    const std::string over = R"(return {api_version=1,id='many',check=function(c)
        return {{id='a',level='ok',message='1'},{id='b',level='ok',message='2'},{id='c',level='ok',message='3'}} end})";
    rules::Source over_srcs[] = {{"many", over}};
    auto rows = rules::evaluate(over_srcs, ctx, lim);
    CHECK(any_runtime_warn(rows));
    CHECK_EQ(count_prefix(rows, "lua.many."), std::size_t{0});

    const std::string exact = R"(return {api_version=1,id='two',check=function(c)
        return {{id='a',level='ok',message='1'},{id='b',level='ok',message='2'}} end})";
    rules::Source exact_srcs[] = {{"two", exact}};
    auto rows2 = rules::evaluate(exact_srcs, ctx, lim);
    CHECK(!any_runtime_warn(rows2));
    CHECK_EQ(count_prefix(rows2, "lua.two."), std::size_t{2});
}

// 任意 action 类型（非 disable_mod）被拒 → 警告，不产出修复 argv。
TEST(arbitrary_action_is_rejected) {
    rules::Context ctx;
    add_fact(ctx, "preferences.allow_disable_mod", "true");
    add_mod(ctx, "Get Lost", /*enabled=*/true, /*exists=*/true);
    const std::string text = R"(return {api_version=1,id='arb',check=function(c)
        return {{id='x',level='warn',message='m',action={type='execute_shell',name='rm -rf /'}}} end})";
    rules::Source srcs[] = {{"arb", text}};
    auto rows = rules::evaluate(srcs, ctx);
    CHECK(any_runtime_warn(rows));
    CHECK(!has_check(rows, "lua.arb.x"));
}

// disable_mod 只有在「偏好允许 + mod 存在且已启用 + 名称精确匹配」时才生成修复 argv。
TEST(disable_mod_action_requires_enabled_exact_mod_and_preference) {
    const std::string text = R"(return {api_version=1,id='dis',check=function(c)
        return {{id='x',level='warn',message='m',action={type='disable_mod',name='Get Lost'}}} end})";
    rules::Source srcs[] = {{"dis", text}};

    // 允许 + 存在且启用 → fix = {mods, disable, Get Lost}
    {
        rules::Context ctx;
        add_fact(ctx, "preferences.allow_disable_mod", "true");
        add_mod(ctx, "Get Lost", /*enabled=*/true, /*exists=*/true);
        auto rows = rules::evaluate(srcs, ctx);
        const Check* c = find_check(rows, "lua.dis.x");
        CHECK(c != nullptr);
        if (c) {
            CHECK_EQ(c->fix.size(), std::size_t{3});
            CHECK(c->fix[0] == "mods");
            CHECK(c->fix[1] == "disable");
            CHECK(c->fix[2] == "Get Lost");
        }
    }
    // 无偏好 → 非法 action 警告，无检查
    {
        rules::Context ctx;
        add_mod(ctx, "Get Lost", /*enabled=*/true, /*exists=*/true);
        auto rows = rules::evaluate(srcs, ctx);
        CHECK(!has_check(rows, "lua.dis.x"));
        CHECK(any_runtime_warn(rows));
    }
    // mod 存在但未启用 → 非法 action 警告
    {
        rules::Context ctx;
        add_fact(ctx, "preferences.allow_disable_mod", "true");
        add_mod(ctx, "Get Lost", /*enabled=*/false, /*exists=*/true);
        auto rows = rules::evaluate(srcs, ctx);
        CHECK(!has_check(rows, "lua.dis.x"));
        CHECK(any_runtime_warn(rows));
    }
    // 名称不精确匹配 → 非法 action 警告
    {
        rules::Context ctx;
        add_fact(ctx, "preferences.allow_disable_mod", "true");
        add_mod(ctx, "Get Lost Deluxe", /*enabled=*/true, /*exists=*/true);
        auto rows = rules::evaluate(srcs, ctx);
        CHECK(!has_check(rows, "lua.dis.x"));
        CHECK(any_runtime_warn(rows));
    }
}

// 一条坏脚本只折叠成它自己的警告，不影响后面的合法脚本。
TEST(one_bad_rule_does_not_suppress_the_next) {
    rules::Context ctx;
    const std::string bad = R"(return {api_version = 9, id = 'bad', check = function(c) return {} end})";
    const std::string good =
        R"(return {api_version = 1, id = 'good', check = function(c) return {{id = 'ok', level = 'ok', message = 'fine'}} end})";
    rules::Source srcs[] = {{"bad", bad}, {"good", good}};
    auto rows = rules::evaluate(srcs, ctx);
    CHECK(any_runtime_warn(rows));
    CHECK(rows[0].id == "lua.runtime");
    CHECK(has_check(rows, "lua.good.ok"));
}

// describe 返回独立（非 MO2）游戏描述符：id/executable/data_directory/plugin_format。
TEST(describe_returns_independent_game_descriptor) {
    rules::Source tes{"g", R"(return {api_version=1,id='fake.game',executable='bin/Game.exe',data_directory='data',plugin_format='tes'})"};
    auto d = rules::describe(tes);
    CHECK(d.id == "fake.game");
    CHECK(d.executable == "bin/Game.exe");
    CHECK(d.data_directory == "data");
    CHECK(d.plugin_format == "tes");

    rules::Source none{"g", R"(return {api_version=1,id='fake.game2',executable='Game.exe',data_directory='Data',plugin_format='none'})"};
    auto d2 = rules::describe(none);
    CHECK(d2.id == "fake.game2");
    CHECK(d2.plugin_format == "none");
}

// 绝对/穿越路径、盘符、未知 plugin_format、错版本、缺字段都抛 std::runtime_error。
TEST(describe_rejects_bad_paths_and_formats_with_runtime_error) {
    auto throws = [](std::string_view text) {
        rules::Source s{"g", text};
        try {
            rules::describe(s);
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    CHECK(throws(R"(return {api_version=1,id='g',executable='/usr/bin/Game.exe',data_directory='data',plugin_format='tes'})"));  // 绝对
    CHECK(throws(R"(return {api_version=1,id='g',executable='../Game.exe',data_directory='data',plugin_format='tes'})"));        // 穿越
    CHECK(throws(R"(return {api_version=1,id='g',executable='Game.exe',data_directory='../../etc',plugin_format='tes'})"));      // 穿越(data)
    CHECK(throws(R"(return {api_version=1,id='g',executable='C:\\Games\\Game.exe',data_directory='data',plugin_format='tes'})"));  // 盘符
    CHECK(throws(R"(return {api_version=1,id='g',executable='Game.exe',data_directory='data',plugin_format='foobar'})"));       // 未知格式
    CHECK(throws(R"(return {api_version=2,id='g',executable='Game.exe',data_directory='data',plugin_format='tes'})"));          // 错版本
    CHECK(throws(R"(return {api_version=1,id='g',data_directory='data',plugin_format='tes'})"));                                // 缺字段
    CHECK(!throws(R"(return {api_version=1,id='g',executable='Game.exe',data_directory='data',plugin_format='tes'})"));         // 合法不抛
}

// collect_context 把实例快照成内存 Context；唯一触碰磁盘的测试，用 mkdtemp 且即用即清。
TEST(collect_context_snapshots_instance_from_a_temp_dir) {
    char tmpl[] = "/tmp/mol_rules_ctx.XXXXXX";
    if (char* dir = ::mkdtemp(tmpl)) {
        const fs::path root(dir);
        std::error_code ec;
        fs::create_directories(root / "profiles/Default", ec);
        fs::create_directories(root / "mods/MyMod", ec);
        fs::create_directories(root / "game", ec);
        { std::ofstream(root / "profiles/Default/modlist.txt") << "+MyMod\n"; }
        Instance inst;
        inst.root.assign(root.string());
        inst.profiles_dir.assign((root / "profiles").string());
        inst.mods_dir.assign((root / "mods").string());
        inst.overwrite_dir.assign((root / "overwrite").string());
        inst.cfg.profile = "Default";
        inst.cfg.game = "skyrimse";
        inst.cfg.game_dir.assign((root / "game").string());
        inst.cfg.prefix.assign((root / "prefix").string());
        inst.cfg.prefix_user = "steamuser";
        try {
            auto ctx = rules::collect_context(inst, "9.9.9.9");
            CHECK(ctx.game == "skyrimse");
            CHECK(ctx.game_version == "9.9.9.9");
            bool found = false;
            for (const auto& m : ctx.mods)
                if (m.name == "MyMod") found = true;
            CHECK(found);
        } catch (const std::exception&) {
            CHECK(false);  // 最小实例上 collect_context 不应抛
        }
        fs::remove_all(root, ec);
    } else {
        CHECK(false);  // 无法创建临时目录
    }
}

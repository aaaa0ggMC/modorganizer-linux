#pragma once
// 子命令公共契约：Context（全局选项 + 子命令参数 + 事件出口）与 Result（envelope 原料）。
//
// ⚠⚠ 混用约束（GCC 16 实测，踩过坑，后人务必保留）⚠⚠
//   同一 TU 内，`import alib6;` / `import std;` 与「文本 include 标准库头」不能共存：
//   libstdc++ 的模块视图与文本视图会互相重定义（直接编译报错）。
//   解决办法：**所有 #include 都排在所有 import 之前**（即先把标准库/core/纯 CLI 头
//   包含完，最后才是 import）。因此本头文件（以及包含它的 cli/output.hpp）必须是被
//   include 的最后一个头文件。
//
// 命令的 data 结构体定义在 cli/results.hpp（字段名对齐 docs/CLI.md），
// 各 handler 用 alib6 反射 to_adata() 转成 AData 后放进 Result::data。
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "args.hpp"
#include "events.hpp"
#include "results.hpp"
#include "mol/error.hpp"
#include "mol/instance.hpp"

// 见文件头的混用约束：import 放在所有 #include 之后。
import alib6;

namespace cli {

// ---------------------------------------------------------------------------
// Context：一次子命令执行的全部输入
// ---------------------------------------------------------------------------
struct Context {
    using allocator_type = mol::allocator_type;

    const GlobalOptions* globals = nullptr;
    ParsedArgs args;      // 本子命令的已解析参数（error 非空 = 用法错误）
    std::pmr::string command;  // 规范命令名，如 "mods move"
    std::pmr::string instance_dir;  // 已绝对化
    EventSink* sink = nullptr;
    mol::mr* mem = mol::default_mr();

    Context() = default;
    explicit Context(allocator_type a) : args(a), command(a), instance_dir(a) {}
    Context(const Context& o, allocator_type a)
        : globals(o.globals), args(o.args, a), command(o.command, a),
          instance_dir(o.instance_dir, a), sink(o.sink), mem(o.mem) {}
    Context(Context&& o, allocator_type a)
        : globals(o.globals), args(std::move(o.args), a), command(std::move(o.command), a),
          instance_dir(std::move(o.instance_dir), a), sink(o.sink), mem(o.mem) {}
    Context(const Context&) = default;
    Context(Context&&) = default;
    Context& operator=(const Context&) = default;
    Context& operator=(Context&&) = default;

    // -p/--profile → load_instance 的 profile_override（未指定时为空）
    [[nodiscard]] std::string_view profile_override() const {
        if (globals != nullptr && globals->has_profile) return globals->profile;
        return {};
    }
};

// ---------------------------------------------------------------------------
// Result：envelope 的原料（data 已经是 alib6::AData，由各命令用反射填充）
// ---------------------------------------------------------------------------
struct Result {
    using allocator_type = mol::allocator_type;

    bool ok = true;
    int exit_code = 0;  // 0/1/2/3（3 仅 status 漂移）
    std::pmr::string command;
    alib6::AData data;  // ok=false 时保持 null
    std::pmr::vector<Err> warnings;
    std::pmr::vector<Err> errors;

    explicit Result(allocator_type a = {})
        : command(a), data(a.resource()), warnings(a), errors(a) {}
    Result(const Result& o, allocator_type a)
        : ok(o.ok), exit_code(o.exit_code), command(o.command, a), data(o.data, a.resource()),
          warnings(o.warnings, a), errors(o.errors, a) {}
    Result(Result&& o, allocator_type a)
        : ok(o.ok), exit_code(o.exit_code), command(std::move(o.command), a),
          data(std::move(o.data), a.resource()), warnings(std::move(o.warnings), a),
          errors(std::move(o.errors), a) {}
    Result(const Result&) = default;
    Result(Result&&) = default;
    Result& operator=(const Result&) = default;
    Result& operator=(Result&&) = default;

    void add_error(std::string_view code, std::string_view message, std::string_view path);
    void add_warning(std::string_view code, std::string_view message, std::string_view path);
    // 把 data 设成某个结果结构体（走反射）
    template <class T>
    void set_data(const T& value) {
        data = alib6::to_adata(value, data.get_allocator());
    }
};

// 所有子命令的统一签名
using CommandFn = Result (*)(Context&);

// ---- 结果构造 --------------------------------------------------------------
Result make_ok(Context& ctx);                                            // data=空对象
Result make_usage_error(std::string_view message, Context& ctx);
// 游戏层 info 里的字符串字段（sub_key 非空则取其子对象里的字段）；host 缺失/字段缺失返回空串。
std::string game_info_string(Context& ctx, const mol::Instance& inst, std::string_view key, std::string_view sub_key = {});          // invalid_argument/2
Result result_from_mol_error(const mol::Error& e, Context& ctx);
Result result_from_std_exception(const std::exception& e, Context& ctx);  // 归一 io_error/1
Result result_from_unknown_command(Context& ctx, std::string_view line);  // invalid_argument/2

// 位置参数个数校验（0 或 1）；arg_name 用于「NAME is required」这类提示。
// 返回非空 = 用法错误 Result，调用方应直接 `return *bad;`（绝不带着坏参数去执行变更）。
[[nodiscard]] std::optional<Result> check_positionals(Context& ctx, std::size_t expected,
                                                       std::string_view arg_name);

// 退出码映射：errors 含 invalid_argument → 2，其它非空 → 1，空 → 0。
// （status 的漂移 ok=true/exit 3 由命令自己设置，不走这里。）
int exit_code_from_issues(const std::pmr::vector<Err>& errors);

// ---------------------------------------------------------------------------
// AData 只读访问（文本渲染用；缺失/类型不匹配返回零值，绝不抛异常）
// ---------------------------------------------------------------------------
namespace adata {

const alib6::AData* field(const alib6::AData& node, std::string_view key);
const alib6::AData* at(const alib6::AData& arr, std::size_t index);
std::string_view str(const alib6::AData& node, std::string_view key);
long long integer(const alib6::AData& node, std::string_view key);
bool boolean(const alib6::AData& node, std::string_view key);
std::size_t size(const alib6::AData& node);

}  // namespace adata

}  // namespace cli

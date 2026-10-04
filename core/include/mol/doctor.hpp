#pragma once
// doctor：只读体检。每项检查给出 level（ok|warn|error）、说明与修复提示。不改任何文件、不启动任何进程。
#include <string_view>

#include "mol/instance.hpp"
#include "mol/pmr.hpp"

namespace mol {

struct Check {
    using allocator_type = mol::allocator_type;
    string id;       // 稳定的检查名，如 "skse.version"
    string level;    // ok | warn | error
    string message;
    string hint;     // 可空：给人看的建议
    vector<string> fix;  // 可空：mo-linux 子命令 argv（不含程序名），执行它就能修好（如 {"apply"}）
    explicit Check(allocator_type a = {}) : id(a), level(a), message(a), hint(a), fix(a) {}
    Check(const Check& o, allocator_type a) : id(o.id, a), level(o.level, a), message(o.message, a), hint(o.hint, a), fix(o.fix, a) {}
    Check(Check&& o, allocator_type a) : id(std::move(o.id), a), level(std::move(o.level), a), message(std::move(o.message), a), hint(std::move(o.hint), a), fix(std::move(o.fix), a) {}
    Check(const Check&) = default;
    Check(Check&&) = default;
    Check& operator=(const Check&) = default;
    Check& operator=(Check&&) = default;
};

// game_version：游戏层报告的版本（如 "1.7.104.0"）；空表示无法取得（host 库缺失等），相关检查降级为 warn。
vector<Check> run_doctor(const Instance& inst, std::string_view game_version, mr* mem = default_mr());

// "skse64_<a>_<b>_<c>.dll"：由 "a.b.c.d" 推出的 SKSE 运行时 dll 名；格式不对返回空。
string skse_dll_name(std::string_view game_version, mr* mem = default_mr());

// 游戏目录或任一已启用的根目录型 mod 的顶层是否有该文件（大小写不敏感）。
bool root_provides(const Instance& inst, std::string_view name, mr* mem = default_mr());

}  // namespace mol

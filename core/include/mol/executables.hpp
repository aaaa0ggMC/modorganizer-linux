#pragma once
// MO2 实例里登记的可执行文件（ModOrganizer.ini 的 [customExecutables]）：整合包通常登记 SKSE、xEdit、Synthesis、BodySlide 等工具。
// 提供：列出它们、把 MO2 里的路径换算成「在农场里怎么找到它」。
#include <string_view>

#include "mol/instance.hpp"
#include "mol/pmr.hpp"

namespace mol {

struct Executable {
    using allocator_type = mol::allocator_type;
    string title;
    string binary;       // 解析成 Unix 路径（Wine 路径经前缀换算；%BASE_DIR% 换成实例根）
    string arguments;
    string working_dir;  // 可空
    bool hide = false;
    string farm_path;    // 相对农场根（binary 在游戏目录或某个 mod 里时）；否则为空，此时直接用 binary 的绝对路径运行
    explicit Executable(allocator_type a = {}) : title(a), binary(a), arguments(a), working_dir(a), farm_path(a) {}
    Executable(const Executable& o, allocator_type a) : title(o.title, a), binary(o.binary, a), arguments(o.arguments, a), working_dir(o.working_dir, a), hide(o.hide), farm_path(o.farm_path, a) {}
    Executable(Executable&& o, allocator_type a) : title(std::move(o.title), a), binary(std::move(o.binary), a), arguments(std::move(o.arguments), a), working_dir(std::move(o.working_dir), a), hide(o.hide), farm_path(std::move(o.farm_path), a) {}
    Executable(const Executable&) = default;
    Executable(Executable&&) = default;
    Executable& operator=(const Executable&) = default;
    Executable& operator=(Executable&&) = default;
};

vector<Executable> list_executables(const Instance& inst, mr* mem = default_mr());

// 把一个 Unix 绝对路径换算成相对农场根的路径：在游戏目录下 → 原样相对；在 mods/<mod>/ 下 → 普通 mod 前面加 "Data/"、
// 根目录型 mod 不加。其它位置返回空串。
string farm_relative(const Instance& inst, std::string_view unix_path, mr* mem = default_mr());

// 按 shell 规则（空格分隔、单双引号、反斜杠转义）拆参数。
vector<string> split_arguments(std::string_view s, mr* mem = default_mr());

}  // namespace mol

#pragma once
#include <string_view>

#include "mol/pmr.hpp"

namespace mol {

// ASCII 小写化；非 ASCII 字节原样保留（Skyrim 资源路径实际都是 ASCII）。
string casefold(std::string_view s, mr* mem = default_mr());

}  // namespace mol

#pragma once
// alib6 CommandInput → 内部类型（GlobalOptions / ParsedArgs）的适配器。
//
// 依赖 alib6（`Command::CommandInput` 是模块实体，无法前向声明），因此本头文件内含
// `import alib6;`：**它必须是 TU 中最后一个 #include**（混用约束见 cmd_common.hpp）。
//
// 语义（与 alib6 修复后的行为对齐）：
//   * 选项可出现在命令前、命令后、位置参数前后（alib6 根层预扫描 + judge_fn + remains 提取）；
//   * `--name=value` 与 `--name value` 都支持；
//   * `--` 之后一律是位置参数；
//   * 未注册的选项、注册了但不属于本命令的选项、缺值 → 全部报 invalid_argument。
#include <span>
#include <string_view>

#include "args.hpp"

import alib6;

namespace cli {

// 从 CommandInput 构造全局选项（-i/-p/-j/-q/--events）。
// 返回空字符串 = 成功；否则是用法错误描述（→ invalid_argument / exit 2）。
mol::string build_global_options(const alib6::Command::CommandInput& in, GlobalOptions& g,
                                  mol::mr* mem);

// 从 CommandInput 构造子命令参数。specs = 该命令的专有选项（全局选项自动允许）。
// 成功时 out.error 为空；失败时 out.error 非空（用法错误描述）。
void build_parsed_args(const alib6::Command::CommandInput& in, std::span<const OptionSpec> specs,
                       ParsedArgs& out, mol::mr* mem);

}  // namespace cli

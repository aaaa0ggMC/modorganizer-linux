#pragma once
// overwrite 捕获：游戏在农场里新建的真实文件（不是符号链接）移回 <overwrite>/（与 MO2 的 overwrite 语义一致）。
// 农场 Data/ 下的文件 → <overwrite>/<相对 Data 的路径>；Data/ 之外的 → <实例根>/overwrite-root/<相对农场根的路径>
// （不参与合并，仅避免它挡住下一次 apply）。已存在同名目标时覆盖（游戏的较新版本为准）。
#include <cstddef>

#include "mol/instance.hpp"

namespace mol {

// 目标已存在同名文件时，旧文件先移到 <实例根>/overwrite-backup/<相对路径>（再旧的备份被替换），不会静默丢失。
// 调用方应先 require_farm_idle。
// 返回移动的文件数。农场不存在 → 0。io 失败 → Error{io_error}。
std::size_t capture_overwrite(const Instance& inst);

// 是否有进程在使用农场（命令行含农场路径——正斜杠或 Wine 的反斜杠形式——或 cwd 在农场内）。
bool farm_in_use(const Instance& inst);
// farm_in_use → 抛 Error{farm_busy}。
void require_farm_idle(const Instance& inst);

}  // namespace mol

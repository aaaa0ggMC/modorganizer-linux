#pragma once
// overwrite 捕获：游戏在农场里新建的真实文件（不是符号链接）移回 <overwrite>/（与 MO2 的 overwrite 语义一致）。
// 农场 Data/ 下的文件 → <overwrite>/<相对 Data 的路径>；Data/ 之外的 → <实例根>/overwrite-root/<相对农场根的路径>
// （不参与合并，仅避免它挡住下一次 apply）。已存在同名目标时覆盖（游戏的较新版本为准）。
#include <cstddef>

#include "mol/instance.hpp"

namespace mol {

// 返回移动的文件数。农场不存在 → 0。io 失败 → Error{io_error}。
std::size_t capture_overwrite(const Instance& inst);

}  // namespace mol

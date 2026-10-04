// libwinshim 内部共享设施（不对外暴露）。
#pragma once
#include <string>
#include <string_view>

#include "windows.h"

namespace mol_shim {

// UTF-32(wchar_t) <-> UTF-8。非法序列用 U+FFFD / '?' 替代，不抛异常。
std::string wide_to_utf8(const wchar_t* s, std::size_t n = static_cast<std::size_t>(-1));
std::wstring utf8_to_wide(std::string_view s);

// Wine 前缀与用户名。来源优先级：mol_shim_configure() > 环境变量 MOL_WINEPREFIX / MOL_WINEUSER >
// 默认（前缀为空串，用户 "steamuser"）。线程安全。
const std::string& prefix();
const std::string& user();

// Windows 路径 → Unix 路径：
//  * 去掉 "\\?\" 前缀；'\' 当分隔符，合并连续分隔符；
//  * "Z:" → "/"；"C:" → <prefix>/drive_c；其它盘符 X: → <prefix>/dosdevices/x:；
//  * 已是 '/' 开头或无盘符的相对路径：只做分隔符转换；
//  * 不折叠 ".."；不做大小写修正。
std::string to_unix_path(const wchar_t* winpath);
std::string to_unix_path_utf8(std::string_view winpath);

// 反向：Unix 绝对路径 → Windows 路径（<prefix>/drive_c/... → C:\...，其余 → Z:\...）。
std::wstring to_windows_path(std::string_view unix_path);

// 大小写不敏感路径解析：先试原路径；不存在则逐级在父目录里做 ASCII 大小写不敏感匹配（多个候选时取
// 字典序最小的，保证确定性）。找不到的尾部成分原样保留（便于创建新文件）。Windows 语义的 API
// （CreateFileW/GetFileAttributesW/FindFirstFileW/ini/版本资源…）打开已有文件前都应先过它。
std::string resolve_ci(const std::string& unix_path);
// to_unix_path + resolve_ci 的便捷组合。
std::string native_path(const wchar_t* winpath);

// errno → Win32 错误码（ENOENT→2, EACCES→5, EEXIST→183, ENOTDIR→3, ENOSPC→112, EXDEV→17 ... 其它 → 31）。
DWORD errno_to_win(int e);
void set_last_error_errno(int e);
// 线程局部 LastError（GetLastError/SetLastError 的后端，由 internal.cpp 提供）。
DWORD last_error_get();
void last_error_set(DWORD v);

}  // namespace mol_shim

extern "C" {
// 显式配置 Wine 前缀（优先于环境变量）。prefix 可为 nullptr（保持不变）；user 可为 nullptr。
void mol_shim_configure(const char* prefix, const char* user);
}

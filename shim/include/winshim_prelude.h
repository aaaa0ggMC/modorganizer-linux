// 强制包含（-include）：抹平 MSVC 专有关键字，使上游源码零修改编译。
#pragma once
#ifndef _WIN32
#define __declspec(x)
#define __stdcall
#define __cdecl
#define __fastcall
#define __forceinline inline
#define __pragma(x)
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

// spdlog 的 wincolor sink 在 Linux 不存在：用 ansicolor sink 顶替（颜色参数忽略）。
#if defined(__cplusplus) && __has_include(<spdlog/sinks/ansicolor_sink.h>)
#include <spdlog/sinks/ansicolor_sink.h>
#include <mutex>
namespace spdlog::sinks {
class wincolor_stderr_sink_mt : public ansicolor_stderr_sink_mt {
public:
    void set_color(level::level_enum, unsigned short) {}
};
}  // namespace spdlog::sinks
#endif

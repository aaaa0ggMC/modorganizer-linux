#pragma once
// mo-linux CLI 进度事件（--events）。
//
// 规格（docs/CLI.md §进度事件）：
//   每行一个 JSON：{"event":"progress","op":"apply","done":120,"total":5000}
//   开始 {"event":"start","op":"apply"}；结束 {"event":"done","op":"apply","ok":true}
//   progress 至多每 50ms 或每 1% 发一次；start/done 必发。
//   对端关闭（EPIPE）静默停止发送，命令照常完成。
//   fifo:PATH  不存在则 mkfifo，非阻塞写打开，无读端则放弃（不阻塞命令）。
//   unix:PATH  SOCK_STREAM connect，失败则放弃。
//   fd:N       直接 write。
//   用 MSG_NOSIGNAL + 忽略 SIGPIPE 保证进程不被信号杀死。
//
// 公共头不 import；实现使用 alib6 JSON，POSIX 细节藏在 events.cpp 里。
// 节流参数（时钟 / 间隔 / 百分比）可注入，便于单测。
#include <cstddef>
#include <string>
#include <string_view>

#include "mol/pmr.hpp"

namespace cli {

class EventSink {
public:
    EventSink();
    ~EventSink();
    EventSink(const EventSink&) = delete;
    EventSink& operator=(const EventSink&) = delete;

    // 解析并打开 target（fd:N | fifo:PATH | unix:PATH）。
    // 返回空字符串 = 成功（或按规格静默放弃，此时 !active()）；
    // 返回非空 = 目标格式非法（调用方应报 invalid_argument，退出码 2）。
    mol::string open(std::string_view target, mol::mr* mem);
    void close();

    // ---- 测试注入 ---------------------------------------------------------
    using ClockFn = unsigned long long (*)();  // 毫秒单调时钟
    void set_clock(ClockFn fn);
    void set_min_interval_ms(long long ms);
    void set_percent(long long pct);

    // ---- 事件 -------------------------------------------------------------
    void start(std::string_view op);
    void progress(std::string_view op, unsigned long long done, unsigned long long total);
    void done(std::string_view op, bool ok);

    // ---- 观测 -------------------------------------------------------------
    bool active() const { return active_; }
    std::size_t emitted() const { return emitted_; }        // 成功写出的行数
    const std::string& disabled_reason() const { return disabled_reason_; }

private:
    bool emit_line(std::string_view event, std::string_view op, bool has_count,
                   unsigned long long done, unsigned long long total, bool ok_flag);
    bool should_emit(unsigned long long done, unsigned long long total);
    bool write_bytes(const char* data, std::size_t n);

    int fd_ = -1;
    bool owns_fd_ = false;
    bool is_socket_ = false;  // unix:PATH → send(MSG_NOSIGNAL)；fd/fifo → write()
    bool active_ = false;
    std::size_t emitted_ = 0;
    std::string disabled_reason_;

    ClockFn clock_ = nullptr;
    long long min_interval_ms_ = 50;
    long long percent_ = 1;
    unsigned long long last_ms_ = 0;
    unsigned long long last_done_ = 0;
};

}  // namespace cli

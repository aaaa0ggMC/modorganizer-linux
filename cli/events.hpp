#pragma once
// mo-linux CLI 进度事件（--events）。
//
// 规格（docs/CLI.md §进度事件）：
//   每行一个 JSON：{"event":"progress","op":"apply","done":120,"total":5000}
//   开始 {"event":"start","op":"apply"}；结束 {"event":"done","op":"apply","ok":true}
//   progress 至多每 50ms 或每 1% 发一次；start/done 必发。
//   op 为 "download"（字节）时，采样满 1 秒后 progress 多带 "rate"（字节/秒，最近约 5 秒的平均；两次采样之间沿用上一次的值）
//   与 "eta"（秒，有 total 时）。计数型的 op（downloaded/install/apply…）不带。
//   对端关闭（EPIPE）静默停止发送，命令照常完成。
//   fifo:PATH  不存在则 mkfifo，非阻塞写打开，无读端则放弃（不阻塞命令）。
//   unix:PATH  SOCK_STREAM connect，失败则放弃。
//   fd:N       直接 write。
//   用 MSG_NOSIGNAL + 忽略 SIGPIPE 保证进程不被信号杀死。
//
// 公共头不 import；实现使用 alib6 JSON，POSIX 细节藏在 events.cpp 里。
// 节流参数（时钟 / 间隔 / 百分比）可注入，便于单测。
#include <cstddef>
#include <deque>
#include <map>
#include <utility>
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
    // item（可空）：当前处理的条目名（如集合里正在安装的 mod），非空时事件多一个 "item" 字段；
    // item 变化的那一次不受节流限制，保证 GUI 能看到每个条目。
    void progress(std::string_view op, unsigned long long done, unsigned long long total, std::string_view item = {});
    void done(std::string_view op, bool ok);
    // 立即发出的提示（不节流）：{"event":"note","op":…,"code":…,"message":…}；例如 collection install 开始前的预检
    void note(std::string_view op, std::string_view code, std::string_view message);

    // ---- 观测 -------------------------------------------------------------
    bool active() const { return active_; }
    std::size_t emitted() const { return emitted_; }        // 成功写出的行数
    const std::string& disabled_reason() const { return disabled_reason_; }

private:
    bool emit_line(std::string_view event, std::string_view op, bool has_count,
                   unsigned long long done, unsigned long long total, bool ok_flag, std::string_view item = {},
                   double rate = 0);
    // 速率：每个 op 一个最近 ~5 秒的 (ms, done) 采样窗口；done 回退（新一轮）时清空
    void sample(std::string_view op, unsigned long long done);
    double rate_of(std::string_view op);
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
    std::string last_item_;
    std::map<std::string, std::deque<std::pair<unsigned long long, unsigned long long>>, std::less<>> samples_;
    std::map<std::string, double, std::less<>> last_rate_;  // 每个 op 最近一次算出的非零速率（新一轮时清掉）
};

}  // namespace cli

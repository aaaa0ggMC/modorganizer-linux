// CLI --events 的单测：fd/fifo/unix 目标解析、NDJSON 写出、节流、静默失败。
//
// events 使用 alib6 JSON，全部路径用 /tmp 下的独占临时目录，
// pipe / mkfifo / socketpair 风格，用完即关，不留常驻进程。
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdlib>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <string>

#include "minitest.hpp"
#include "../cli/events.cpp"

namespace {

struct TempDir {
    std::string path;
    TempDir() {
        char pattern[] = "/tmp/mol-events.XXXXXX";
        char* made = ::mkdtemp(pattern);
        if (!made) throw std::runtime_error("mkdtemp failed");
        path = made;
    }
    ~TempDir() { ::rmdir(path.c_str()); }
};
TempDir temp;
const std::string& g_dir = temp.path;
void ensure_dir() {}

// 读光 fd 上的全部数据
std::string drain(int fd) {
    std::string out;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        break;
    }
    return out;
}

std::size_t count_lines(const std::string& s) {
    std::size_t n = 0;
    for (const char c : s) {
        if (c == '\n') ++n;
    }
    return n;
}

// 冻结时钟（毫秒始终为 0）：只保留「每 1%」这条节流规则
unsigned long long frozen_clock() { return 0; }

// 每次调用前进 100ms 的时钟
unsigned long long g_clock_ms = 0;
unsigned long long advancing_clock() {
    g_clock_ms += 100;
    return g_clock_ms;
}

}  // namespace

// ---------------------------------------------------------------------------
TEST(fd_target_writes_complete_ndjson) {
    int fds[2];
    CHECK(::pipe(fds) == 0);
    cli::EventSink sink;
    const std::string path = "fd:" + std::to_string(fds[1]);
    CHECK(sink.open(path, std::pmr::get_default_resource()).empty());
    CHECK(sink.active());
    sink.set_clock(&frozen_clock);
    sink.start("apply");
    sink.progress("apply", 1, 10);
    sink.progress("apply", 5, 10);
    sink.done("apply", true);
    CHECK_EQ(sink.emitted(), std::size_t{4});
    ::close(fds[1]);
    const std::string got = drain(fds[0]);
    ::close(fds[0]);
    CHECK_EQ(count_lines(got), std::size_t{4});
    CHECK(got.find("{\"event\":\"start\",\"op\":\"apply\"}\n") != std::string::npos);
    CHECK(got.find("{\"event\":\"progress\",\"op\":\"apply\",\"done\":1,\"total\":10}\n") !=
          std::string::npos);
    CHECK(got.find("{\"event\":\"progress\",\"op\":\"apply\",\"done\":5,\"total\":10}\n") !=
          std::string::npos);
    CHECK(got.find("{\"event\":\"done\",\"op\":\"apply\",\"ok\":true}\n") != std::string::npos);
}

TEST(throttle_suppresses_small_steps_and_keeps_start_done) {
    int fds[2];
    CHECK(::pipe(fds) == 0);
    cli::EventSink sink;
    CHECK(sink.open("fd:" + std::to_string(fds[1]), std::pmr::get_default_resource()).empty());
    sink.set_clock(&frozen_clock);  // 时间维度恒定 → 只看 1%（total=1000 → 10 个）
    sink.start("apply");            // 必发
    sink.progress("apply", 5, 1000);   // 5 < 10 → 抑制
    sink.progress("apply", 9, 1000);   // 9 < 10 → 抑制
    sink.progress("apply", 12, 1000);  // 12 >= 10 → 发
    sink.progress("apply", 15, 1000);  // 15 - 12 < 10 → 抑制
    CHECK_EQ(sink.emitted(), std::size_t{2});
    sink.done("apply", true);  // 必发
    CHECK_EQ(sink.emitted(), std::size_t{3});
    ::close(fds[1]);
    (void)drain(fds[0]);
    ::close(fds[0]);
}

TEST(item_changes_bypass_the_throttle_and_are_serialized) {
    int fds[2];
    CHECK(::pipe(fds) == 0);
    cli::EventSink sink;
    CHECK(sink.open("fd:" + std::to_string(fds[1]), std::pmr::get_default_resource()).empty());
    sink.set_clock(&frozen_clock);
    sink.progress("install", 1, 1000, "Mod A");  // 新条目 → 发（虽然 1 < 10）
    sink.progress("install", 2, 1000, "Mod A");  // 同一条目、未到阈值 → 抑制
    sink.progress("install", 3, 1000, "Mod B");  // 换条目 → 发
    CHECK_EQ(sink.emitted(), std::size_t{2});
    ::close(fds[1]);
    const std::string out = drain(fds[0]);
    ::close(fds[0]);
    CHECK(out.find("\"item\":\"Mod A\"") != std::string::npos);
    CHECK(out.find("\"item\":\"Mod B\"") != std::string::npos);
}

TEST(time_based_throttle_emits_even_when_done_barley_moves) {
    int fds[2];
    CHECK(::pipe(fds) == 0);
    g_clock_ms = 0;
    cli::EventSink sink;
    CHECK(sink.open("fd:" + std::to_string(fds[1]), std::pmr::get_default_resource()).empty());
    sink.set_clock(&advancing_clock);  // 每次调用 +100ms > 50ms 间隔
    sink.start("apply");
    sink.progress("apply", 1, 1000);  // 1% 未到，但过了 50ms → 发
    sink.progress("apply", 2, 1000);  // 同理
    CHECK_EQ(sink.emitted(), std::size_t{3});
    ::close(fds[1]);
    (void)drain(fds[0]);
    ::close(fds[0]);
}

TEST(progress_carries_rate_and_eta_after_a_second) {
    int fds[2];
    CHECK(::pipe(fds) == 0);
    g_clock_ms = 0;
    cli::EventSink sink;
    CHECK(sink.open("fd:" + std::to_string(fds[1]), std::pmr::get_default_resource()).empty());
    sink.set_clock(&advancing_clock);  // 每次调用 +100ms
    sink.start("download");
    // 每次 progress 消耗两次时钟（采样 + 节流）= 200ms，每次 +2000 → 10000/s
    for (unsigned long long d = 2000; d <= 40000; d += 2000) {
        sink.progress("download", d, 100000);
        sink.progress("downloaded", d / 2000, 20, "Mod " + std::to_string(d));  // 计数型：不带 rate/eta
    }
    ::close(fds[1]);
    const std::string out = drain(fds[0]);
    ::close(fds[0]);
    const auto first = out.find("\"rate\":");
    CHECK(first != std::string::npos);
    CHECK(out.find("{\"event\":\"progress\",\"op\":\"download\",\"done\":2000,\"total\":100000}") != std::string::npos);  // 起步还没有速率
    const auto last_dl = out.rfind("\"op\":\"download\",");
    const auto line_start = out.rfind('\n', last_dl) + 1;
    const std::string tail = out.substr(line_start, out.find('\n', last_dl) - line_start);
    CHECK(tail.find("\"done\":40000") != std::string::npos);
    CHECK(tail.find("\"rate\":") != std::string::npos);
    CHECK(tail.find("\"eta\":") != std::string::npos);
    // 一旦有了速率，之后每条 download 都带（两次采样之间沿用）；downloaded 一条都不带
    std::size_t pos = first, lines_after = 0, with_rate = 0;
    while ((pos = out.find('\n', pos)) != std::string::npos && pos + 1 < out.size()) {
        const std::string line = out.substr(pos + 1, out.find('\n', pos + 1) - pos - 1);
        ++pos;
        if (line.find("\"op\":\"downloaded\"") != std::string::npos) CHECK(line.find("rate") == std::string::npos);
        if (line.find("\"op\":\"download\"") != std::string::npos && line.find("\"event\":\"progress\"") != std::string::npos) {
            ++lines_after;
            if (line.find("\"rate\":") != std::string::npos) ++with_rate;
        }
    }
    CHECK(lines_after > 0);
    CHECK_EQ(with_rate, lines_after);
}

TEST(epipe_is_silent_and_disables_the_sink) {
    int fds[2];
    CHECK(::pipe(fds) == 0);
    ::close(fds[0]);  // 没有读端
    cli::EventSink sink;
    CHECK(sink.open("fd:" + std::to_string(fds[1]), std::pmr::get_default_resource()).empty());
    CHECK(sink.active());
    sink.start("apply");  // write → EPIPE（SIGPIPE 已被忽略，进程不死）
    CHECK(!sink.active());
    sink.progress("apply", 1, 10);  // 静默 no-op
    sink.done("apply", false);
    CHECK_EQ(sink.emitted(), std::size_t{0});
    ::close(fds[1]);
}

TEST(fifo_without_reader_gives_up_without_blocking) {
    ensure_dir();
    const std::string path = g_dir + "/evt_noreader.fifo";
    ::unlink(path.c_str());
    cli::EventSink sink;
    const mol::string open_err =
        sink.open("fifo:" + path, std::pmr::get_default_resource());
    CHECK(std::string(open_err).empty());  // 目标格式合法（放弃不算用法错误）
    CHECK(!sink.active());    // 无读端 → 放弃
    CHECK_EQ(sink.emitted(), std::size_t{0});
    CHECK(!sink.disabled_reason().empty());
    ::unlink(path.c_str());
}

TEST(fifo_with_reader_in_same_process_round_trips) {
    ensure_dir();
    const std::string path = g_dir + "/evt_reader.fifo";
    ::unlink(path.c_str());
    CHECK(::mkfifo(path.c_str(), 0600) == 0);
    const int rd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
    CHECK(rd >= 0);
    {
        cli::EventSink sink;
        CHECK(sink.open("fifo:" + path, std::pmr::get_default_resource()).empty());
        CHECK(sink.active());
        sink.set_clock(&frozen_clock);
        sink.start("apply");
        sink.progress("apply", 4, 100);  // 4 >= 1% → 发
        sink.done("apply", true);
        CHECK_EQ(sink.emitted(), std::size_t{3});
    }  // sink 在这里关闭写端
    const std::string got = drain(rd);
    ::close(rd);
    ::unlink(path.c_str());
    CHECK_EQ(count_lines(got), std::size_t{3});
    CHECK(got.find("{\"event\":\"done\",\"op\":\"apply\",\"ok\":true}\n") != std::string::npos);
}

TEST(unix_socket_peer_close_is_silent) {
    ensure_dir();
    const std::string path = g_dir + "/evt.sock";
    ::unlink(path.c_str());

    const int lst = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(lst >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    CHECK(::bind(lst, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0);
    CHECK(::listen(lst, 4) == 0);

    g_clock_ms = 0;
    {
        cli::EventSink sink;
        CHECK(sink.open("unix:" + path, std::pmr::get_default_resource()).empty());
        CHECK(sink.active());
        const int peer = ::accept(lst, nullptr, nullptr);
        CHECK(peer >= 0);
        ::close(peer);  // 对端立刻关闭

        sink.set_clock(&advancing_clock);
        sink.start("apply");
        for (int i = 0; i < 8 && sink.active(); ++i) {
            sink.progress("apply", static_cast<unsigned long long>(i) + 1, 100000);
        }
        // 写失败（EPIPE/ECONNRESET）→ 静默停止，不抛不崩
        CHECK(!sink.active());
    }
    ::close(lst);
    ::unlink(path.c_str());
}

TEST(invalid_targets_are_usage_errors) {
    cli::EventSink sink;
    CHECK(!sink.open("bogus", std::pmr::get_default_resource()).empty());
    CHECK(!sink.open("fd:", std::pmr::get_default_resource()).empty());
    CHECK(!sink.open("fd:abc", std::pmr::get_default_resource()).empty());
    CHECK(!sink.open("nope:/tmp/x", std::pmr::get_default_resource()).empty());
    CHECK(!sink.active());
}

TEST(fd_target_rejects_out_of_range_and_trailing_characters) {
    cli::EventSink sink;
    for (const char* target : {"fd:2147483648", "fd:999999999999999999999999", "fd:1x", "fd:-1", "fd: 1", "fd:+1"}) {
        CHECK(!sink.open(target, std::pmr::get_default_resource()).empty());
        CHECK(!sink.active());
    }
}

TEST(percentage_threshold_rounds_up) {
    int fds[2];
    CHECK(::pipe(fds) == 0);
    cli::EventSink sink;
    CHECK(sink.open("fd:" + std::to_string(fds[1]), std::pmr::get_default_resource()).empty());
    sink.set_clock(&frozen_clock);
    sink.start("apply");
    sink.progress("apply", 1, 199);
    CHECK_EQ(sink.emitted(), std::size_t{1});
    sink.progress("apply", 2, 199);
    CHECK_EQ(sink.emitted(), std::size_t{2});
    ::close(fds[1]);
    (void)drain(fds[0]);
    ::close(fds[0]);
}

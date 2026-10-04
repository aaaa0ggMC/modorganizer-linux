// mo-linux CLI 进度事件实现（POSIX：fifo / unix socket / 裸 fd；静默失败）。
#include "events.hpp"

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

import alib6;

namespace cli {

namespace {

unsigned long long default_clock_ms() {
    return static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

EventSink::EventSink() = default;

EventSink::~EventSink() { close(); }

void EventSink::set_clock(ClockFn fn) { clock_ = fn ? fn : &default_clock_ms; }

void EventSink::set_min_interval_ms(long long ms) { min_interval_ms_ = ms; }

void EventSink::set_percent(long long pct) { percent_ = pct; }

mol::string EventSink::open(std::string_view target, mol::mr* mem) {
    close();
    clock_ = clock_ ? clock_ : &default_clock_ms;

    const auto colon = target.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return mol::string("invalid --events target (expected fd:N | fifo:PATH | unix:PATH): " +
                               std::string(target),
                           mem);
    }
    const std::string_view scheme = target.substr(0, colon);
    const std::string_view value = target.substr(colon + 1);
    if (value.empty()) {
        return mol::string("invalid --events target (empty value for " + std::string(scheme) + ")",
                           mem);
    }

    // SIGPIPE 永不致命（幂等）
    std::signal(SIGPIPE, SIG_IGN);

    if (scheme == "fd") {
        int n = -1;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), n);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || n < 0) {
            return mol::string("invalid fd number in --events target: " + std::string(value), mem);
        }
        fd_ = static_cast<int>(n);
        owns_fd_ = false;
        is_socket_ = false;
        active_ = true;
        disabled_reason_.clear();
        return {};
    }

    if (scheme == "fifo") {
        const std::string path(value);
        struct stat st {};
        if (::stat(path.c_str(), &st) != 0) {
            if (errno == ENOENT) {
                if (::mkfifo(path.c_str(), 0644) != 0 && errno != EEXIST) {
                    disabled_reason_ = "cannot create fifo " + path + ": " + std::strerror(errno);
                    return {};
                }
            } else {
                disabled_reason_ = "cannot stat fifo " + path + ": " + std::strerror(errno);
                return {};
            }
        } else if (!S_ISFIFO(st.st_mode)) {
            disabled_reason_ = "not a fifo: " + path;
            return {};
        }
        fd_ = ::open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
            // 无读端（ENXIO）等：按规格放弃，不阻塞命令
            disabled_reason_ = "cannot open fifo for writing: " + path + ": " + std::strerror(errno);
            fd_ = -1;
            return {};
        }
        struct stat opened {};
        if (::fstat(fd_, &opened) != 0 || !S_ISFIFO(opened.st_mode)) {
            disabled_reason_ = "event target is not a fifo after opening";
            ::close(fd_);
            fd_ = -1;
            return {};
        }
        owns_fd_ = true;
        is_socket_ = false;
        active_ = true;
        disabled_reason_.clear();
        return {};
    }

    if (scheme == "unix") {
        const std::string path(value);
        if (path.size() >= sizeof(sockaddr_un::sun_path)) {
            return mol::string("unix socket path too long: " + path, mem);
        }
        const int s = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (s < 0) {
            disabled_reason_ = std::string("cannot create unix socket: ") + std::strerror(errno);
            return {};
        }
        sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
            disabled_reason_ = "cannot connect to unix socket " + path + ": " + std::strerror(errno);
            ::close(s);
            return {};
        }
        fd_ = s;
        owns_fd_ = true;
        is_socket_ = true;
        active_ = true;
        disabled_reason_.clear();
        return {};
    }

    return mol::string("unknown --events scheme '" + std::string(scheme) +
                           "' (expected fd | fifo | unix)",
                       mem);
}

void EventSink::close() {
    if (fd_ >= 0 && owns_fd_) {
        ::close(fd_);
    }
    fd_ = -1;
    owns_fd_ = false;
    is_socket_ = false;
    active_ = false;
    emitted_ = 0;
    last_ms_ = 0;
    last_done_ = 0;
}

bool EventSink::write_bytes(const char* data, std::size_t n) {
    if (fd_ < 0) return false;
    std::size_t off = 0;
    while (off < n) {
        ssize_t w = -1;
        if (is_socket_) {
            // 管道/裸 fd 上用 send(MSG_NOSIGNAL) 会 EINVAL；且 SIGPIPE 已在 open 时忽略，
            // 所以 socket 用 send(MSG_NOSIGNAL)，其它用 write()。
            w = ::send(fd_, data + off, n - off, MSG_NOSIGNAL);
        } else {
            w = ::write(fd_, data + off, n - off);
        }
        if (w > 0) {
            off += static_cast<std::size_t>(w);
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // 无字节写出时可丢弃本条；部分行已经写出时停止发送，避免后续行拼到残片。
            if (off != 0) {
                active_ = false;
                disabled_reason_ = "event stream stopped after partial write";
            }
            return false;
        }
        // EPIPE / ECONNRESET / EBADF ...：对端关闭，静默停止发送
        active_ = false;
        disabled_reason_ = std::string("write failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool EventSink::should_emit(unsigned long long done, unsigned long long total) {
    unsigned long long threshold = 1;
    if (percent_ > 0) {
        const auto pct = static_cast<unsigned long long>(std::min(percent_, 100LL));
        threshold = (total / 100ULL) * pct + (total % 100ULL * pct + 99ULL) / 100ULL;
        if (threshold == 0) threshold = 1;
    }
    if (done >= last_done_ && done - last_done_ >= threshold) return true;
    const unsigned long long now = clock_();
    if (now >= last_ms_ + static_cast<unsigned long long>(min_interval_ms_ > 0 ? min_interval_ms_ : 0)) {
        return true;
    }
    return false;
}

bool EventSink::emit_line(std::string_view event, std::string_view op, bool has_count,
                          unsigned long long done, unsigned long long total, bool ok_flag) {
    if (!active_) return false;
    struct Event {
        mol::string event;
        mol::string op;
    };
    struct Progress {
        mol::string event;
        mol::string op;
        unsigned long long done;
        unsigned long long total;
    };
    struct Done {
        mol::string event;
        mol::string op;
        bool ok;
    };
    std::pmr::monotonic_buffer_resource arena;
    alib6::AData data(&arena);
    if (has_count) {
        data = alib6::to_adata(Progress{mol::string(event, &arena), mol::string(op, &arena),
                                      done, total}, &arena);
    } else if (event == "done") {
        data = alib6::to_adata(Done{mol::string(event, &arena), mol::string(op, &arena),
                                  ok_flag}, &arena);
    } else {
        data = alib6::to_adata(Event{mol::string(event, &arena), mol::string(op, &arena)}, &arena);
    }
    alib6::JSONConfig config;
    config.compact_lines = true;
    config.compact_spaces = true;
    std::string line;
    alib6::JSON(config).dump(line, data);
    line += '\n';
    if (!write_bytes(line.data(), line.size())) return false;
    ++emitted_;
    return true;
}

void EventSink::start(std::string_view op) {
    if (!active_) return;
    if (emit_line("start", op, false, 0, 0, true)) {
        last_done_ = 0;
        last_ms_ = clock_();
    }
}

void EventSink::progress(std::string_view op, unsigned long long done, unsigned long long total) {
    if (!active_) return;
    if (!should_emit(done, total)) return;
    if (emit_line("progress", op, true, done, total, true)) {
        last_done_ = done;
        last_ms_ = clock_();
    }
}

void EventSink::done(std::string_view op, bool ok) {
    if (!active_) return;
    emit_line("done", op, false, 0, 0, ok);
}

}  // namespace cli

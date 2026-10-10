// 脚本 HTTP server：127.0.0.1 + token；/_mol/… 宿主自省，/<ns>/… 脚本 namespace。
// 只绑回环：同机其它用户/进程拿到 token 才能读写脚本状态。
#include "lua_script_http.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "lua_script_http.hpp"
#include "lua_script_json.hpp"
#include "mol/error.hpp"

namespace mol::script {
namespace {

using namespace std::chrono;

std::string percent_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Request {
    std::string method, path, query, auth;
    std::string body;
    bool ok = false;
};

// 读一个请求（Connection: close 模式，读完即回）。任意一步失败 → ok=false。
Request read_request(int fd) {
    Request r;
    std::string head;
    char buf[8192];
    const auto deadline = steady_clock::now() + seconds(10);
    while (head.find("\r\n\r\n") == std::string::npos) {
        if (head.size() > 64 * 1024) return r;
        const auto left = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
        if (left <= 0) return r;
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(left)) <= 0 || !(p.revents & POLLIN)) return r;
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) return r;
        head.append(buf, static_cast<std::size_t>(n));
    }
    const std::size_t split = head.find("\r\n\r\n");
    std::string_view lines(head.data(), split);
    r.body = head.substr(split + 4);
    std::size_t pos = 0;
    bool first = true;
    std::size_t content_length = 0;
    while (pos <= lines.size()) {
        std::size_t e = lines.find("\r\n", pos);
        if (e == std::string_view::npos) e = lines.size();
        const std::string_view line = lines.substr(pos, e - pos);
        if (first) {
            const auto s1 = line.find(' ');
            if (s1 == std::string_view::npos) return r;
            const auto s2 = line.find(' ', s1 + 1);
            if (s2 == std::string_view::npos) return r;
            r.method = std::string(line.substr(0, s1));
            std::string target(line.substr(s1 + 1, s2 - s1 - 1));
            const auto q = target.find('?');
            if (q == std::string::npos) {
                r.path = target;
            } else {
                r.path = target.substr(0, q);
                r.query = target.substr(q + 1);
            }
            first = false;
        } else {
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) return r;
            const std::string key = lower(std::string(line.substr(0, colon)));
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
            while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' '))
                value.remove_suffix(1);
            if (key == "authorization") r.auth = std::string(value);
            if (key == "content-length") {
                content_length = static_cast<std::size_t>(std::strtoull(std::string(value).c_str(), nullptr, 10));
            }
        }
        if (e == lines.size()) break;
        pos = e + 2;
    }
    if (r.method.empty() || r.path.empty()) return r;
    if (content_length > 1024 * 1024) return r;
    while (r.body.size() < content_length) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, 10000) <= 0 || !(p.revents & POLLIN)) return r;
        const ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) return r;
        r.body.append(buf, static_cast<std::size_t>(n));
    }
    r.body.resize(std::min(r.body.size(), content_length));
    r.ok = true;
    return r;
}

void write_response(int fd, int status, std::string_view status_text, std::string_view body) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + std::string(status_text) +
                      "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                      "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    out.append(body);
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t n = ::send(fd, out.data() + done, out.size() - done, MSG_NOSIGNAL);
        if (n <= 0) break;
        done += static_cast<std::size_t>(n);
    }
}

bool token_ok(const Request& r, const std::string& token) {
    if (r.auth == "Bearer " + token) return true;
    // ?token=…
    std::size_t pos = 0;
    while (pos < r.query.size()) {
        std::size_t e = r.query.find('&', pos);
        if (e == std::string::npos) e = r.query.size();
        const std::string_view kv = std::string_view(r.query).substr(pos, e - pos);  // 不能绑到 substr 的临时 string 上
        const auto eq = kv.find('=');
        if (eq != std::string_view::npos && kv.substr(0, eq) == "token" &&
            percent_decode(kv.substr(eq + 1)) == token)
            return true;
        pos = e + 1;
    }
    return false;
}

std::vector<std::string> path_parts(std::string_view path) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < path.size()) {
        if (path[i] == '/') {
            ++i;
            continue;
        }
        std::size_t j = path.find('/', i);
        if (j == std::string_view::npos) j = path.size();
        out.push_back(percent_decode(path.substr(i, j - i)));
        i = j;
    }
    return out;
}

std::string tail_lines(const std::vector<std::string>& lines, std::size_t n) {
    const std::size_t start = lines.size() > n ? lines.size() - n : 0;
    std::string out = "[";
    for (std::size_t i = start; i < lines.size(); ++i) {
        if (i > start) out.push_back(',');
        out += json_quote(lines[i]);
    }
    return out + "]";
}

// 处理一条连接；shared 让连接线程即使在本 server 析构后也安全。
struct Shared {
    Registry* reg;
    std::string token;
    std::atomic<bool>* stop;
    std::atomic<std::size_t>* inflight;
    std::condition_variable* idle_cv;
    std::mutex* idle_mu;
};

void handle(const Shared& sh, int fd) {
    const Request r = read_request(fd);
    if (!r.ok) {
        write_response(fd, 400, "Bad Request", "{\"error\":\"bad request\"}");
        ::close(fd);
        return;
    }
    if (!token_ok(r, sh.token)) {
        write_response(fd, 401, "Unauthorized", "{\"error\":\"missing or wrong token\"}");
        ::close(fd);
        return;
    }
    const auto parts = path_parts(r.path);
    if (parts.empty() || parts.front().empty()) {
        write_response(fd, 404, "Not Found", "{\"error\":\"not found\"}");
        ::close(fd);
        return;
    }
    const std::string& head = parts.front();
    if (head == "_mol") {  // 宿主自省：所有实例/所有脚本一处看全
        if (parts.size() >= 2 && parts[1] == "ping") {
            write_response(fd, 200, "OK",
                           "{\"ok\":true,\"server\":\"mo-linux script host\",\"runs\":" +
                               std::to_string(sh.reg->size()) + "}");
        } else if (parts.size() == 2 && parts[1] == "scripts") {
            std::string out = "[";
            bool first = true;
            for (const auto& run : sh.reg->runs()) {
                const Status st = run->status();
                if (!first) out.push_back(',');
                first = false;
                out += "{\"ns\":" + json_quote(st.ns) + ",\"script\":" + json_quote(st.script) +
                       ",\"instance\":" + json_quote(st.instance) + ",\"run_state\":" + json_quote(st.run_state) +
                       ",\"op\":" + json_quote(st.op.name) + ",\"op_detail\":" + json_quote(st.op.detail) +
                       ",\"op_since_ms\":" + std::to_string(st.op.since_ms) +
                       ",\"lua_line\":" + std::to_string(st.lua_line) +
                       ",\"http_url\":" + json_quote(st.http_url) + "}";
            }
            out += "]";
            write_response(fd, 200, "OK", out);
        } else if (parts.size() >= 3 && parts[1] == "scripts") {
            const std::string ns = parts[2];
            const auto run = sh.reg->find(ns);
            if (!run) {
                write_response(fd, 404, "Not Found", "{\"error\":\"no such script run\"}");
            } else if (parts.size() == 4 && parts[3] == "log") {
                std::size_t n = 200;
                if (const auto q = r.query.find("lines="); q != std::string::npos) {
                    n = static_cast<std::size_t>(
                        std::strtoull(r.query.substr(q + 6).c_str(), nullptr, 10));
                    n = std::clamp<std::size_t>(n, 1, 2000);
                }
                write_response(fd, 200, "OK",
                               "{\"ns\":" + json_quote(ns) + ",\"log\":" + tail_lines(run->status().log, n) + "}");
            } else {
                write_response(fd, 200, "OK", status_to_json(run->status()));
            }
        } else {
            write_response(fd, 404, "Not Found", "{\"error\":\"not found\"}");
        }
        ::close(fd);
        return;
    }
    // 脚本 namespace
    if (!valid_ns(head)) {
        write_response(fd, 404, "Not Found", "{\"error\":\"unknown namespace\"}");
        ::close(fd);
        return;
    }
    const auto run = sh.reg->find(head);
    if (!run) {
        write_response(fd, 404, "Not Found", "{\"error\":\"unknown namespace\"}");
        ::close(fd);
        return;
    }
    const Status st = run->status();
    if (parts.size() == 1) {  // /<ns>/ 索引
        write_response(fd, 200, "OK", status_to_json(st));
        ::close(fd);
        return;
    }
    if (parts.size() == 2 && parts[1] == "log") {  // /<ns>/log
        write_response(fd, 200, "OK", "{\"ns\":" + json_quote(head) + ",\"log\":" + tail_lines(st.log, 200) + "}");
        ::close(fd);
        return;
    }
    // 其余一律当 state key：/<ns>/<key>（也接受 /<ns>/state/<key> 与 /<ns>/state）
    std::string key;
    if (parts[1] == "state") {
        if (parts.size() == 2) {
            write_response(fd, 200, "OK", "{\"ns\":" + json_quote(head) + ",\"state\":" + state_json(st) + "}");
            ::close(fd);
            return;
        }
        for (std::size_t i = 2; i < parts.size(); ++i) key += (i > 2 ? "/" : "") + parts[i];
    } else {
        for (std::size_t i = 1; i < parts.size(); ++i) key += (i > 1 ? "/" : "") + parts[i];
    }
    if (!valid_state_key(key)) {
        write_response(fd, 400, "Bad Request", "{\"error\":\"invalid state key\"}");
        ::close(fd);
        return;
    }
    if (r.method == "GET") {
        const auto it = st.state.find(key);
        if (it == st.state.end()) {
            write_response(fd, 404, "Not Found", "{\"error\":\"no such state key\"}");
        } else {
            write_response(fd, 200, "OK", json_quote(it->second));
        }
    } else if (r.method == "POST" || r.method == "PUT") {
        const std::string value = body_to_value(r.body);
        const auto run2 = sh.reg->find(head);
        if (!run2) {
            write_response(fd, 404, "Not Found", "{\"error\":\"unknown namespace\"}");
        } else if (!run2->set_state(key, value)) {
            write_response(fd, 409, "Conflict", "{\"error\":\"the run refused the value (limit or finished)\"}");
        } else {
            write_response(fd, 200, "OK",
                           "{\"ok\":true,\"key\":" + json_quote(key) + ",\"value\":" + json_quote(value) + "}");
        }
    } else {
        write_response(fd, 405, "Method Not Allowed", "{\"error\":\"method not allowed\"}");
    }
    ::close(fd);
}

}  // namespace

std::string state_json(const Status& st) {
    std::string out = "{";
    bool first = true;
    for (const auto& [k, v] : st.state) {
        if (!first) out.push_back(',');
        first = false;
        out += json_quote(k);
        out.push_back(':');
        out += json_quote(v);
    }
    return out + "}";
}

std::string HttpServer::random_token() {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    std::FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f) {
        // 退化路径：时间 + 地址熵。仅本地回环 + token 场景下可接受。
        const auto seed = static_cast<unsigned long>(
            duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
        unsigned long s = seed ^ reinterpret_cast<std::uintptr_t>(&f);
        for (int i = 0; i < 32; ++i) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            out.push_back(hex[(s >> 33) & 15]);
        }
        return out;
    }
    unsigned char buf[16];
    const std::size_t n = std::fread(buf, 1, sizeof buf, f);
    std::fclose(f);
    if (n != sizeof buf) buf[0] ^= static_cast<unsigned char>(::getpid());
    for (unsigned char c : buf) {
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 15]);
    }
    return out;
}

struct HttpServer::Impl {
    Registry& reg;
    std::string token;
    std::uint16_t port = 0;
    int listen_fd = -1;
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> inflight{0};
    std::mutex idle_mu;
    std::condition_variable idle_cv;
    std::thread thread;

    Impl(Registry& r, std::uint16_t p, std::string tok) : reg(r), token(std::move(tok)) {
        listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd < 0) throw Error("io_error", "cannot create the HTTP socket");
        int one = 1;
        ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 只绑回环
        addr.sin_port = htons(p);
        if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            const int e = errno;
            ::close(listen_fd);
            listen_fd = -1;
            throw Error("io_error", "cannot bind 127.0.0.1:" + std::to_string(p) + ": " + std::strerror(e));
        }
        if (::listen(listen_fd, 64) != 0) {
            const int e = errno;
            ::close(listen_fd);
            listen_fd = -1;
            throw Error("io_error", std::string("listen failed: ") + std::strerror(e));
        }
        sockaddr_in got{};
        socklen_t len = sizeof got;
        if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&got), &len) == 0) port = ntohs(got.sin_port);
        thread = std::thread([this] { loop(); });
    }

    ~Impl() {
        stop = true;
        if (listen_fd >= 0) {
            ::shutdown(listen_fd, SHUT_RDWR);
            ::close(listen_fd);
        }
        if (thread.joinable()) thread.join();
        // 等在途请求结束（最多 2 秒）
        std::unique_lock lk(idle_mu);
        idle_cv.wait_for(lk, seconds(2), [this] { return inflight.load() == 0; });
    }

    void loop() {
        while (!stop) {
            pollfd p{listen_fd, POLLIN, 0};
            const int rc = ::poll(&p, 1, 200);
            if (rc <= 0) continue;
            if (!(p.revents & POLLIN)) continue;
            const int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0) continue;
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            const Shared sh{&reg, token, &stop, &inflight, &idle_cv, &idle_mu};
            ++inflight;
            std::thread([sh, fd] {
                handle(sh, fd);
                if (--(*sh.inflight) == 0) {
                    std::lock_guard lk(*sh.idle_mu);
                    sh.idle_cv->notify_all();
                }
            }).detach();
        }
    }
};

HttpServer::HttpServer(Registry& reg, std::uint16_t port, std::string token) {
    if (token.empty()) token = random_token();
    impl_ = std::make_unique<Impl>(reg, port, std::move(token));
}

HttpServer::~HttpServer() = default;

std::uint16_t HttpServer::port() const { return impl_->port; }

const std::string& HttpServer::token() const { return impl_->token; }

std::string HttpServer::url_for(std::string_view ns) const {
    return "http://127.0.0.1:" + std::to_string(impl_->port) + "/" + std::string(ns) + "/";
}

}  // namespace mol::script

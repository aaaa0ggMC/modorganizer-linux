#pragma once
// 测试用的极简本地 HTTP 服务器（每个连接一个请求，Connection: close）。
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace mockhttp {

struct Req {
    std::string method, target;
    std::map<std::string, std::string> h;  // 小写键
};
struct Resp {
    int status = 200;
    std::string body;
    std::string extra;  // 额外头，每行以 \r\n 结尾
};

// 极简本地 HTTP 服务器：每个连接一个请求，Connection: close。
class Mock {
public:
    explicit Mock(std::function<Resp(const Req&)> fn) : fn_(std::move(fn)) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a);
        socklen_t len = sizeof a;
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        ::listen(fd_, 8);
        th_ = std::thread([this] { loop(); });
    }
    ~Mock() {
        stop_ = true;
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        if (th_.joinable()) th_.join();
    }
    std::string base() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::vector<Req> seen;

private:
    void loop() {
        while (!stop_) {
            const int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) break;
            std::string buf;
            char tmp[1024];
            while (buf.find("\r\n\r\n") == std::string::npos) {
                const ssize_t n = ::recv(c, tmp, sizeof tmp, 0);
                if (n <= 0) break;
                buf.append(tmp, static_cast<size_t>(n));
            }
            Req r;
            const auto eol = buf.find("\r\n");
            const std::string line = buf.substr(0, eol);
            const auto s1 = line.find(' '), s2 = line.find(' ', s1 + 1);
            r.method = line.substr(0, s1);
            r.target = line.substr(s1 + 1, s2 - s1 - 1);
            std::size_t pos = eol + 2;
            while (pos < buf.size()) {
                const auto e = buf.find("\r\n", pos);
                if (e == std::string::npos || e == pos) break;
                const std::string hl = buf.substr(pos, e - pos);
                const auto colon = hl.find(':');
                if (colon != std::string::npos) {
                    std::string k = hl.substr(0, colon), v = hl.substr(colon + 1);
                    for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
                    r.h[k] = v;
                }
                pos = e + 2;
            }
            seen.push_back(r);
            const Resp resp = fn_(r);
            const std::string head = "HTTP/1.1 " + std::to_string(resp.status) + " X\r\nContent-Length: " + std::to_string(resp.body.size()) +
                                     "\r\nConnection: close\r\n" + resp.extra + "\r\n";
            const std::string out = head + resp.body;
            size_t off = 0;
            while (off < out.size()) {
                const ssize_t n = ::send(c, out.data() + off, out.size() - off, MSG_NOSIGNAL);
                if (n <= 0) break;
                off += static_cast<size_t>(n);
            }
            ::close(c);
        }
    }
    std::function<Resp(const Req&)> fn_;
    int fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread th_;
};

}  // namespace mockhttp

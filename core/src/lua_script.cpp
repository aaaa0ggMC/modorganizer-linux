// Lua 安装脚本运行时：沙箱 + vroot（fd 式虚拟文件系统）+ Windows exe 执行（Landlock 收容）。
// 安全模型见 docs/DESIGN-lua-scripts.md；对外 API 见 core/include/mol/lua_script.hpp。
//
// 混用约束：所有 #include 排在 import 之前（GCC 16 实测，见 cli/cmd_common.hpp）。
// 宿主调用失败一律抛异常：sol2（SOL_ALL_SAFETIES_ON）在 trampoline 里接住并转成 Lua 错误，
// 不会让 C++ 异常穿过 Lua 的 C 帧（脚本侧没有 pcall，失败即停，这是有意设计）。
// 脚本侧的校验失败用 alib6 的 panicf：自动带 file:line + 调用栈，排查省事；
// 文件系统/进程类的系统错误仍用 mol::Error（带稳定 code，CLI 要映射退出码）。
#include "mol/lua_script.hpp"

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <linux/landlock.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <set>
#include <thread>

#include <sol/sol.hpp>

#include "lua_script_json.hpp"
#include "mol/error.hpp"
#include "mol/http.hpp"
#include "mol/impact.hpp"
#include "mol/mod_install.hpp"
#include "mol/overwrite.hpp"
#include "mol/runner.hpp"

import alib6;
namespace mol::script {
namespace {

using namespace std::chrono;

// ---------------------------------------------------------------------------
// 纯函数：路径 / PE / URL / namespace / state key
// ---------------------------------------------------------------------------

bool bad_char(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return u < 0x20 || u == 0x7f || c == '\\' || c == ':';
}

std::vector<std::string> split_vpath(std::string_view p) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < p.size()) {
        const std::size_t j = p.find('/', i);
        const std::size_t end = j == std::string_view::npos ? p.size() : j;
        if (end == i) throw Error("invalid_argument", "empty path component");
        out.emplace_back(p.substr(i, end - i));
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    return out;
}

std::vector<std::string> checked_vpath(std::string_view p) {
    if (!valid_vpath(p)) throw Error("invalid_argument", "invalid virtual path");
    return split_vpath(p);
}

}  // namespace

bool valid_vpath(std::string_view p) {
    if (p.empty() || p.size() > 4096) return false;
    if (p.front() == '/' || p.back() == '/') return false;
    std::size_t start = 0;
    while (start <= p.size()) {
        const std::size_t j = p.find('/', start);
        const std::size_t end = j == std::string_view::npos ? p.size() : j;
        const std::string_view comp = p.substr(start, end - start);
        if (comp.empty() || comp.size() > 255) return false;
        if (comp == "." || comp == "..") return false;
        for (char c : comp)
            if (bad_char(c)) return false;
        if (j == std::string_view::npos) break;
        start = j + 1;
    }
    return true;
}

bool valid_ns(std::string_view ns) {
    if (ns.empty() || ns.size() > 32) return false;
    const char f = ns.front();
    if (!((f >= 'a' && f <= 'z') || (f >= '0' && f <= '9'))) return false;  // '_' 开头保留给宿主
    for (char c : ns) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool valid_state_key(std::string_view key) {
    if (key.empty() || key.size() > 128) return false;
    const char f = key.front();
    if (!((f >= 'a' && f <= 'z') || (f >= '0' && f <= '9'))) return false;
    for (char c : key) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                        c == '-' || c == '/';
        if (!ok) return false;
    }
    for (const auto& comp : split_vpath(key))
        if (comp == "." || comp == "..") return false;
    return true;
}

bool http_url_ok(std::string_view url) {
    if (url.size() < 9 || url.size() > 2048) return false;  // "http://a" 最短
    for (char c : url) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f) return false;
    }
    const auto pos = url.find("://");
    if (pos == std::string_view::npos || pos == 0) return false;
    std::string scheme(url.substr(0, pos));
    for (auto& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (scheme != "http" && scheme != "https") return false;
    const auto rest = url.substr(pos + 3);
    const auto auth_end = rest.find_first_of("/?#");
    const auto auth = rest.substr(0, auth_end == std::string_view::npos ? rest.size() : auth_end);
    if (auth.empty() || auth.find('@') != std::string_view::npos) return false;
    return true;
}

std::string pe_machine(std::string_view host_path) {
    const int fd = ::open(std::string(host_path).c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "unknown";
    unsigned char buf[4096];
    const ssize_t n = ::read(fd, buf, sizeof buf);
    ::close(fd);
    if (n < 64) return "unknown";
    if (buf[0] != 'M' || buf[1] != 'Z') return "unknown";
    std::uint32_t pe = 0;
    std::memcpy(&pe, buf + 0x3c, 4);
    if (pe == 0 || pe + 6 > static_cast<std::uint32_t>(n)) return "unknown";
    if (buf[pe] != 'P' || buf[pe + 1] != 'E' || buf[pe + 2] != 0 || buf[pe + 3] != 0) return "unknown";
    std::uint16_t machine = 0;
    std::memcpy(&machine, buf + pe + 4, 2);
    switch (machine) {
        case 0x14c: return "i386";
        case 0x8664: return "amd64";
        case 0xaa64: return "arm64";
        default: return "unknown";
    }
}

namespace {

bool v4_private(std::uint32_t a) {  // 主机字节序
    if ((a >> 24) == 0) return true;                                    // 0.0.0.0/8
    if ((a >> 24) == 10) return true;                                   // 10/8
    if ((a >> 24) == 127) return true;                                  // 127/8
    if ((a & 0xffc00000u) == 0x64400000u) return true;                  // 100.64/10 CGNAT
    if ((a & 0xffff0000u) == 0xa9fe0000u) return true;                  // 169.254/16
    if ((a & 0xfff00000u) == 0xac100000u) return true;                  // 172.16/12
    if ((a & 0xffffff00u) == 0xc0000000u) return true;                  // 192.0.0/24
    if ((a & 0xffffff00u) == 0xc0000200u) return true;                  // 192.0.2/24
    if ((a & 0xffff0000u) == 0xc0a80000u) return true;                  // 192.168/16
    if ((a & 0xfffe0000u) == 0xc6120000u) return true;                  // 198.18/15
    if ((a & 0xffffff00u) == 0xc6336400u) return true;                  // 198.51.100/24
    if ((a & 0xffffff00u) == 0xcb007100u) return true;                  // 203.0.113/24
    if ((a >> 28) == 0xe) return true;                                  // 224/4 组播
    if ((a >> 28) == 0xf) return true;                                  // 240/4 保留
    return false;
}

bool v6_private(const unsigned char* b) {
    bool zero = true;
    for (int i = 0; i < 16; ++i)
        if (b[i] != 0) zero = false;
    if (zero) return true;                                        // ::/128
    static const unsigned char lo[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (std::memcmp(b, lo, 16) == 0) return true;                 // ::1
    if (b[0] == 0xff) return true;                                // ff00::/8
    if ((b[0] & 0xfe) == 0xfc) return true;                       // fc00::/7 ULA
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return true;       // fe80::/10
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8) return true;  // 2001:db8::/32
    // IPv4-mapped / NAT64：尾 4 字节按 IPv4 判
    bool mapped = true;
    for (int i = 0; i < 10; ++i)
        if (b[i] != 0) mapped = false;
    if (mapped && b[10] == 0xff && b[11] == 0xff)
        return v4_private((std::uint32_t(b[12]) << 24) | (std::uint32_t(b[13]) << 16) |
                          (std::uint32_t(b[14]) << 8) | std::uint32_t(b[15]));
    bool nat64 = b[0] == 0 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b;
    for (int i = 4; i < 12; ++i)
        if (b[i] != 0) nat64 = false;
    if (nat64)
        return v4_private((std::uint32_t(b[12]) << 24) | (std::uint32_t(b[13]) << 16) |
                          (std::uint32_t(b[14]) << 8) | std::uint32_t(b[15]));
    return false;
}

std::string url_host(std::string_view url) {
    const auto pos = url.find("://");
    if (pos == std::string_view::npos) return {};
    auto rest = url.substr(pos + 3);
    const auto auth_end = rest.find_first_of("/?#");
    if (auth_end != std::string_view::npos) rest = rest.substr(0, auth_end);
    if (!rest.empty() && rest.front() == '[') {  // [::1]:8080
        const auto close = rest.find(']');
        if (close != std::string_view::npos) return std::string(rest.substr(1, close - 1));
        return {};
    }
    if (const auto colon = rest.rfind(':'); colon != std::string_view::npos &&
                                              rest.find(':') == colon)  // 只有一个冒号 = IPv4:port
        return std::string(rest.substr(0, colon));
    return std::string(rest);
}

}  // namespace

bool host_is_private(std::string_view host) {
    if (host.empty() || host.size() > 255) return false;
    const std::string h(host);
    in_addr a4{};
    in6_addr a6{};
    if (::inet_pton(AF_INET, h.c_str(), &a4) == 1) return v4_private(ntohl(a4.s_addr));
    if (::inet_pton(AF_INET6, h.c_str(), &a6) == 1) return v6_private(a6.s6_addr);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(h.c_str(), nullptr, &hints, &res) != 0 || !res) return false;  // 解析失败放行
    bool priv = false;
    for (addrinfo* it = res; it; it = it->ai_next) {
        if (it->ai_family == AF_INET)
            priv = priv || v4_private(ntohl(reinterpret_cast<sockaddr_in*>(it->ai_addr)->sin_addr.s_addr));
        else if (it->ai_family == AF_INET6)
            priv = priv || v6_private(reinterpret_cast<sockaddr_in6*>(it->ai_addr)->sin6_addr.s6_addr);
    }
    ::freeaddrinfo(res);
    return priv;
}

namespace {

// ---------------------------------------------------------------------------
// Landlock（内核 ≥ 5.13）：对 exe 进程树施加文件系统规则，与 32/64 位无关、后代继承、不可撤销
// ---------------------------------------------------------------------------
constexpr std::uint64_t kFsExecute = 1ULL << 0, kWriteFile = 1ULL << 1, kReadFile = 1ULL << 2,
                        kReadDir = 1ULL << 3, kRemoveDir = 1ULL << 4, kRemoveFile = 1ULL << 5,
                        kMakeChar = 1ULL << 6, kMakeDir = 1ULL << 7, kMakeReg = 1ULL << 8,
                        kMakeSock = 1ULL << 9, kMakeFifo = 1ULL << 10, kMakeBlock = 1ULL << 11,
                        kMakeSym = 1ULL << 12, kRefer = 1ULL << 13, kTruncate = 1ULL << 14;
constexpr std::uint64_t kAbi1All = kFsExecute | kWriteFile | kReadFile | kReadDir | kRemoveDir |
                                   kRemoveFile | kMakeChar | kMakeDir | kMakeReg | kMakeSock |
                                   kMakeFifo | kMakeBlock | kMakeSym | kRefer | kTruncate;
constexpr std::uint64_t kReadOnly = kFsExecute | kReadFile | kReadDir;

using Rule = std::pair<std::string, std::uint64_t>;

// 在（fork 后的）当前进程施加规则集；返回 "v1" 或 "unavailable"。
// 只调用 async-signal-safe 的 syscall/open/prctl，且参数已在外层构造好。
std::string landlock_apply(const std::vector<Rule>& rules) {
    const long abi = ::syscall(SYS_landlock_create_ruleset, nullptr, std::size_t{0},
                               LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) return "unavailable";
    struct landlock_ruleset_attr attr {};
    attr.handled_access_fs = kAbi1All;
    const int ruleset_fd =
        static_cast<int>(::syscall(SYS_landlock_create_ruleset, &attr, sizeof(attr), 0));
    if (ruleset_fd < 0) return "unavailable";
    for (const auto& [path, access] : rules) {
        const int fd = ::open(path.c_str(), O_PATH | O_CLOEXEC);
        if (fd < 0) continue;  // 路径不存在（prefix 未建等）→ 跳过
        struct landlock_path_beneath_attr beneath {};
        beneath.allowed_access = access;
        beneath.parent_fd = fd;
        const long rc = ::syscall(SYS_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
                                  &beneath, std::size_t{0});
        ::close(fd);
        if (rc != 0) {
            ::close(ruleset_fd);
            return "unavailable";
        }
    }
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0) {
        ::close(ruleset_fd);
        return "unavailable";
    }
    const long rc = ::syscall(SYS_landlock_restrict_self, ruleset_fd, std::size_t{0});
    ::close(ruleset_fd);
    return rc == 0 ? "v1" : "unavailable";
}

// ---------------------------------------------------------------------------
// vroot：fd 式虚拟文件系统。逐组件 O_NOFOLLOW，不跟随任何符号链接；
// 不依赖 weakly_canonical 做事后校验（竞态下不可靠）。
// ---------------------------------------------------------------------------
struct Fd {
    int fd = -1;
    Fd() = default;
    explicit Fd(int f) : fd(f) {}
    ~Fd() {
        if (fd >= 0) ::close(fd);
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd(o.fd) { o.fd = -1; }
    Fd& operator=(Fd&& o) noexcept {
        if (this != &o) {
            if (fd >= 0) ::close(fd);
            fd = o.fd;
            o.fd = -1;
        }
        return *this;
    }
    [[nodiscard]] bool ok() const { return fd >= 0; }
};

int dup_cloexec(int fd) {
    const int d = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (d < 0) throw Error("io_error", std::string("dup failed: ") + std::strerror(errno));
    return d;
}

// 从 base 逐组件打开目录；create=true 时补建缺失的中间目录（mkdir -p 语义）。
Fd open_below(int base, const std::vector<std::string>& comps, bool create) {
    int cur = dup_cloexec(base);
    for (const auto& c : comps) {
        int next = ::openat(cur, c.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && create && errno == ENOENT) {
            if (::mkdirat(cur, c.c_str(), 0755) != 0 && errno != EEXIST) {
                const int e = errno;
                ::close(cur);
                throw Error("io_error", "cannot create directory '" + c + "': " + std::strerror(e));
            }
            next = ::openat(cur, c.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        if (next < 0) {
            const int e = errno;
            ::close(cur);
            throw Error("io_error", "cannot open '" + c + "': " + std::strerror(e));
        }
        ::close(cur);
        cur = next;
    }
    return Fd(cur);
}

struct Parent {
    Fd fd;
    std::string leaf;
};

Parent open_parent(int base, const std::vector<std::string>& comps) {
    if (comps.empty()) throw Error("invalid_argument", "empty path");
    if (comps.size() == 1) return Parent{Fd(dup_cloexec(base)), comps.front()};
    std::vector<std::string> parents(comps.begin(), comps.end() - 1);
    return Parent{open_below(base, parents, false), comps.back()};
}

std::optional<std::string> vfs_read(int base, const std::vector<std::string>& comps, std::uint64_t cap) {
    const auto p = open_parent(base, comps);
    const int fd = ::openat(p.fd.fd, p.leaf.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR) return std::nullopt;
        throw Error("io_error", "cannot read '" + p.leaf + "': " + std::strerror(errno));
    }
    Fd guard(fd);
    struct stat st {};
    if (::fstat(fd, &st) != 0) throw Error("io_error", "fstat failed");
    if (!S_ISREG(st.st_mode)) throw Error("invalid_argument", "not a regular file");
    if (static_cast<std::uint64_t>(st.st_size) > cap)
        throw Error("limit_exceeded", "file is larger than the read limit");
    std::string out;
    out.reserve(std::min<std::uint64_t>(static_cast<std::uint64_t>(st.st_size), 1u << 20));
    char buf[65536];
    std::uint64_t total = 0;
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            throw Error("io_error", std::string("read failed: ") + std::strerror(errno));
        }
        total += static_cast<std::uint64_t>(n);
        if (total > cap) throw Error("limit_exceeded", "file grew past the read limit");
        out.append(buf, static_cast<std::size_t>(n));
    }
    return out;
}

void vfs_write(int base, const std::vector<std::string>& comps, std::string_view data, bool append,
               std::uint64_t file_cap, std::uint64_t& written, std::uint64_t vroot_cap) {
    if (data.size() > file_cap) throw Error("limit_exceeded", "write exceeds the per-file limit");
    if (written + data.size() > vroot_cap)
        throw Error("limit_exceeded", "the virtual root's total write budget is exhausted");
    const auto p = open_parent(base, comps);
    const int fd = ::openat(p.fd.fd, p.leaf.c_str(),
                            O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC) | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) throw Error("io_error", "cannot write '" + p.leaf + "': " + std::strerror(errno));
    Fd guard(fd);
    struct stat st {};
    if (::fstat(fd, &st) != 0) throw Error("io_error", "fstat failed");
    if (!S_ISREG(st.st_mode)) throw Error("invalid_argument", "not a regular file");
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw Error("io_error", std::string("write failed: ") + std::strerror(errno));
        }
        done += static_cast<std::size_t>(n);
    }
    written += data.size();
}

bool vfs_exists(int base, const std::vector<std::string>& comps) {
    const auto p = open_parent(base, comps);
    struct stat st {};
    return ::fstatat(p.fd.fd, p.leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0;
}

std::optional<std::uint64_t> vfs_size(int base, const std::vector<std::string>& comps) {
    const auto p = open_parent(base, comps);
    struct stat st {};
    if (::fstatat(p.fd.fd, p.leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) return std::nullopt;
    if (!S_ISREG(st.st_mode)) return std::nullopt;
    return static_cast<std::uint64_t>(st.st_size);
}

std::optional<std::vector<std::string>> vfs_list(int base, const std::vector<std::string>& comps) {
    const auto p = open_parent(base, comps);
    const int fd = ::openat(p.fd.fd, p.leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        // 只有 ENOENT 是「不存在」；符号链接（ELOOP）与「不是目录」（ENOTDIR）都是明确拒绝
        if (errno == ENOENT) return std::nullopt;
        throw Error("io_error", "cannot list '" + p.leaf + "': " + std::strerror(errno));
    }
    DIR* d = ::fdopendir(fd);  // 接管 fd
    if (!d) {
        ::close(fd);
        throw Error("io_error", "fdopendir failed");
    }
    std::vector<std::string> names;
    for (;;) {
        errno = 0;
        const dirent* e = ::readdir(d);
        if (!e) break;
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
        names.emplace_back(e->d_name);
        if (names.size() > 4096) {
            ::closedir(d);
            throw Error("limit_exceeded", "directory has too many entries to list");
        }
    }
    ::closedir(d);
    std::sort(names.begin(), names.end());
    return names;
}

void vfs_mkdir(int base, const std::vector<std::string>& comps) {
    if (comps.empty()) throw Error("invalid_argument", "empty path");
    std::vector<std::string> parents(comps.begin(), comps.end() - 1);
    const Fd parent = open_below(base, parents, true);
    if (::mkdirat(parent.fd, comps.back().c_str(), 0755) != 0 && errno != EEXIST)
        throw Error("io_error", "cannot create '" + comps.back() + "': " + std::strerror(errno));
}

void vfs_remove_at(int dir_fd, const std::string& name) {
    struct stat st {};
    if (::fstatat(dir_fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return;
        throw Error("io_error", "cannot stat '" + name + "': " + std::strerror(errno));
    }
    if (!S_ISDIR(st.st_mode)) {
        if (::unlinkat(dir_fd, name.c_str(), 0) != 0 && errno != ENOENT)
            throw Error("io_error", "cannot remove '" + name + "': " + std::strerror(errno));
        return;
    }
    std::vector<std::string> kids;
    {
        const int sub = ::openat(dir_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (sub < 0) {
            if (errno == ENOENT) return;
            throw Error("io_error", "cannot open '" + name + "': " + std::strerror(errno));
        }
        if (DIR* d = ::fdopendir(sub)) {  // fdopendir 接管 sub，closedir 会关掉它
            for (;;) {
                errno = 0;
                const dirent* e = ::readdir(d);
                if (!e) break;
                if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
                kids.emplace_back(e->d_name);
            }
            ::closedir(d);
        } else {
            ::close(sub);
            throw Error("io_error", "fdopendir failed");
        }
    }
    if (!kids.empty()) {  // 重新打开再递归（上面的 fd 已被 closedir 关掉）
        const int sub = ::openat(dir_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (sub < 0) throw Error("io_error", "cannot open '" + name + "': " + std::strerror(errno));
        for (const auto& k : kids) vfs_remove_at(sub, k);
        ::close(sub);
    }
    if (::unlinkat(dir_fd, name.c_str(), AT_REMOVEDIR) != 0 && errno != ENOENT)
        throw Error("io_error", "cannot remove '" + name + "': " + std::strerror(errno));
}

void vfs_remove(int base, const std::vector<std::string>& comps) {
    const auto p = open_parent(base, comps);
    vfs_remove_at(p.fd.fd, p.leaf);
}

void vfs_copy(int base, const std::vector<std::string>& src, const std::vector<std::string>& dst,
              std::uint64_t file_cap, std::uint64_t& written, std::uint64_t vroot_cap) {
    const auto sp = open_parent(base, src);
    const int in = ::openat(sp.fd.fd, sp.leaf.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (in < 0) throw Error("io_error", "cannot read '" + sp.leaf + "': " + std::strerror(errno));
    Fd in_guard(in);
    struct stat st {};
    if (::fstat(in, &st) != 0) throw Error("io_error", "fstat failed");
    if (!S_ISREG(st.st_mode)) throw Error("invalid_argument", "copy only supports regular files");
    const auto dp = open_parent(base, dst);
    const int out =
        ::openat(dp.fd.fd, dp.leaf.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out < 0) throw Error("io_error", "cannot write '" + dp.leaf + "': " + std::strerror(errno));
    Fd out_guard(out);
    char buf[65536];
    std::uint64_t total = 0;
    for (;;) {
        const ssize_t n = ::read(in, buf, sizeof buf);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            throw Error("io_error", std::string("read failed: ") + std::strerror(errno));
        }
        total += static_cast<std::uint64_t>(n);
        if (total > file_cap || written + total > vroot_cap)
            throw Error("limit_exceeded", "copy exceeds the file or virtual-root budget");
        std::size_t done = 0;
        while (done < static_cast<std::size_t>(n)) {
            const ssize_t w = ::write(out, buf + done, static_cast<std::size_t>(n) - done);
            if (w < 0) {
                if (errno == EINTR) continue;
                throw Error("io_error", std::string("write failed: ") + std::strerror(errno));
            }
            done += static_cast<std::size_t>(w);
        }
    }
    written += total;
}

void vfs_move(int base, const std::vector<std::string>& src, const std::vector<std::string>& dst) {
    const auto sp = open_parent(base, src);
    const auto dp = open_parent(base, dst);
    if (::renameat(sp.fd.fd, sp.leaf.c_str(), dp.fd.fd, dp.leaf.c_str()) != 0)
        throw Error("io_error", "cannot move '" + sp.leaf + "': " + std::strerror(errno));
}

// 解包后校验：拒绝符号链接与越界条目（与 mod_install 的 validate_tree 同语义）。
std::size_t validate_tree(const std::string& dir) {    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(fs::path(dir), ec);
    std::size_t files = 0;
    for (fs::recursive_directory_iterator it(fs::path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        const auto st = it->symlink_status(ec);
        if (fs::is_symlink(st))
            throw Error("invalid_argument", "archive contains a symbolic link; refusing", it->path().string());
        const fs::path c = fs::weakly_canonical(it->path(), ec);
        if (c.string().rfind(base.string() + "/", 0) != 0)
            throw Error("invalid_argument", "archive entry escapes the target directory", it->path().string());
        if (fs::is_regular_file(st)) ++files;
    }
    return files;
}

std::uint64_t du_bytes(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::uint64_t total = 0;
    for (fs::recursive_directory_iterator it(fs::path(dir), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) total += static_cast<std::uint64_t>(it->file_size(ec));
    }
    return total;
}

// ---------------------------------------------------------------------------
// exe 执行：Landlock 收容 + 进程组超时 + 日志落盘。没有 shell，argv 直传。
// ---------------------------------------------------------------------------
struct ProcOutcome {
    int exit_code = 0;
    bool timed_out = false;
    std::string log_vpath;
    std::string arch;
    std::string landlock;
};

std::string join_args(const std::vector<std::string>& argv) {
    std::string s;
    for (const auto& a : argv) {
        if (!s.empty()) s += ' ';
        s += a;
    }
    return s;
}

// env 过滤：只放行 [A-Za-z_][A-Za-z0-9_]*，且不许碰注入相关变量。
bool env_key_ok(const std::string& k) {
    if (k.empty() || k.size() > 128) return false;
    if (!std::isalpha(static_cast<unsigned char>(k.front())) && k.front() != '_') return false;
    for (char c : k)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    static const char* deny[] = {"LD_", "MOL_COW", "WINEPREFIX", "WINELOADER", "WINEDLL"};
    for (const char* d : deny)
        if (k.rfind(d, 0) == 0) return false;
    return true;
}

// 起 exe 并等待。argv/env 里的字符串必须在调用期间保持稳定（fork 后子进程只读它们、不分配内存：
// 多线程进程里 fork 之后 malloc 可能死锁，所以子进程只用 async-signal-safe 的调用）。
// landlock_rules 同理，预先构造好。landlock 状态经管道回传（'1' = 已收容，'0' = 不可用）。
ProcOutcome spawn_exe(const std::vector<std::string>& argv,
                      const std::vector<std::pair<std::string, std::string>>& env, const std::string& cwd,
                      const std::string& log_file, std::uint64_t timeout_ms, const std::vector<Rule>& landlock_rules) {
    ProcOutcome out;
    out.log_vpath = log_file;
    const int log_fd = ::open(log_file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (log_fd < 0)
        throw Error("io_error", "cannot open the process log: " + std::string(std::strerror(errno)));
    const int devnull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    int pipefd[2] = {-1, -1};
    if (::pipe2(pipefd, O_CLOEXEC) != 0) {
        ::close(log_fd);
        if (devnull >= 0) ::close(devnull);
        throw Error("io_error", std::string("pipe failed: ") + std::strerror(errno));
    }
    // 子进程用的 C 字符串数组（父进程构造，稳定）
    std::vector<char*> av;
    for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    std::vector<std::string> env_store;
    env_store.reserve(env.size());
    for (const auto& [k, v] : env) env_store.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& e : env_store) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(log_fd);
        if (devnull >= 0) ::close(devnull);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        throw Error("io_error", std::string("fork failed: ") + std::strerror(errno));
    }
    if (pid == 0) {
        ::setsid();  // 独立进程组：超时可以整组杀掉
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) ::_exit(126);
        ::dup2(log_fd, STDOUT_FILENO);
        ::dup2(log_fd, STDERR_FILENO);
        if (devnull >= 0) ::dup2(devnull, STDIN_FILENO);
        if (log_fd > 2) ::close(log_fd);
        if (devnull > 2) ::close(devnull);
        ::syscall(SYS_close_range, 3u, ~0u, 0u);  // 不带走调用方/守护进程的任何描述符
        const std::string ll = landlock_apply(landlock_rules);
        const char status = ll == "v1" ? '1' : '0';
        const ssize_t w = ::write(pipefd[1], &status, 1);
        (void)w;
        ::close(pipefd[1]);
        if (status != '1') ::_exit(125);  // 没有收容就不跑（宁可失败）
        ::execvpe(av[0], av.data(), envp.data());
        ::_exit(127);
    }
    ::close(log_fd);
    if (devnull >= 0) ::close(devnull);
    ::close(pipefd[1]);
    // 读收容状态（子进程在 exec 前就写好了；poll 防意外挂住）
    pollfd pfd{pipefd[0], POLLIN, 0};
    char status = '0';
    if (::poll(&pfd, 1, 30000) > 0 && (pfd.revents & POLLIN)) {
        const ssize_t n = ::read(pipefd[0], &status, 1);
        if (n != 1) status = '0';
    }
    ::close(pipefd[0]);
    out.landlock = status == '1' ? "v1" : "unavailable";

    const auto deadline =
        timeout_ms ? steady_clock::now() + milliseconds(timeout_ms) : steady_clock::time_point::max();
    for (;;) {
        int st = 0;
        const pid_t r = ::waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            out.exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1);
            return out;
        }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) throw Error("io_error", std::string("waitpid failed: ") + std::strerror(errno));
        if (steady_clock::now() > deadline) {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            int st2 = 0;
            ::waitpid(pid, &st2, 0);
            out.timed_out = true;
            out.exit_code = -1;
            return out;
        }
        ::usleep(50 * 1000);
    }
}

// ---------------------------------------------------------------------------
// Lua 沙箱
// ---------------------------------------------------------------------------
struct Budget {
    std::size_t used = 0, limit = 0;
    std::uint64_t ticks = 0;
    bool exhausted = false;
    steady_clock::time_point deadline{};
    // 自省位置（脚本线程写、HTTP 线程读；pos_mu 由 Sandbox 提供，与 Status 同一把锁）
    struct Position {
        std::uint64_t ticks = 0;
        int line = 0;
        std::string source;
        std::vector<std::string> frames;
    };
    Position* pos = nullptr;
    std::mutex* pos_mu = nullptr;
};

void* lua_alloc(void* ud, void* ptr, std::size_t old, std::size_t size) {
    auto* b = static_cast<Budget*>(ud);
    if (!ptr) old = 0;
    if (!size) {
        b->used -= old;
        std::free(ptr);
        return nullptr;
    }
    if (size > old && size - old > b->limit - std::min(b->used, b->limit)) return nullptr;
    auto* p = std::realloc(ptr, size);
    if (p) b->used = b->used - old + size;
    return p;
}

// alib6 panic 的消息是多行诊断块；用户看到的 error 字段压成一行，但保留 file:line。
std::string compact_error(const std::exception& e) {
    const std::string what = e.what();
    std::string msg, src;
    std::size_t pos = 0;
    bool panic_shape = false;
    while (pos < what.size()) {
        std::size_t eol = what.find('\n', pos);
        if (eol == std::string::npos) eol = what.size();
        const std::string_view line(what.data() + pos, eol - pos);
        if (line.starts_with("Message  : ")) {
            msg = line.substr(std::strlen("Message  : "));
            panic_shape = true;
        } else if (line.starts_with("Source   : ") && panic_shape) {
            src = line.substr(std::strlen("Source   : "));
        } else if (panic_shape) {
            break;  // Function/Stack 块：对用户没用
        }
        if (eol == what.size()) break;
        pos = eol + 1;
    }
    if (!panic_shape || msg.empty()) return what;  // 不是 panic：原样
    if (!src.empty()) {  // /abs/path/file.cpp:12:34 → file.cpp:12
        const auto colon = src.rfind(':');
        if (colon != std::string::npos && colon > 0) {
            const auto prev = src.rfind(':', colon - 1);
            if (prev != std::string::npos) {
                const auto slash = src.rfind('/', prev);
                const std::size_t from = slash == std::string::npos ? 0 : slash + 1;
                src = src.substr(from, colon - from);
            }
        }
        msg += " (" + src + ")";
    }
    return msg;
}

class Sandbox;

Budget* budget_of(lua_State* L) { return *static_cast<Budget**>(lua_getextraspace(L)); }

// 计数 hook：指令预算 + 墙钟 + 自省位置（行号每次、浅栈每 2048 tick）。
// 只碰 Lua C API 和预先给定的 Position/mutex，不调 Sandbox（避免未完成类型）。
// 实例写锁：同一实例的并发写操作（多个脚本同时装 mod）必须串行。
// 按实例根给锁，只在整个写调用期间持有（不在 exe 运行期间持有）。
std::mutex& instance_write_lock(const std::string& root) {
    static std::mutex mu;
    static std::map<std::string, std::unique_ptr<std::mutex>> locks;
    std::lock_guard lk(mu);
    auto& ptr = locks[root];
    if (!ptr) ptr = std::make_unique<std::mutex>();
    return *ptr;
}

void count_hook(lua_State* L, lua_Debug* ar) {
    auto* b = budget_of(L);
    if (b->exhausted || --b->ticks == 0) {
        b->exhausted = true;
        luaL_error(L, "instruction budget exhausted");
    }
    const auto now = steady_clock::now();
    if (now > b->deadline) luaL_error(L, "wall clock deadline exceeded");
    if (!b->pos || !b->pos_mu) return;
    if (++b->pos->ticks % 8 != 0) return;  // 8 tick = 8k 指令记一次行，够「卡住」的粒度
    int line = ar && ar->currentline > 0 ? ar->currentline : 0;
    if (!line) {
        lua_Debug d {};
        if (lua_getstack(L, 0, &d)) {
            lua_getinfo(L, "l", &d);
            line = d.currentline;
        }
    }
    const bool want_frames = b->pos->ticks % (8 * 256) == 0;
    std::vector<std::string> frames;
    std::string source;
    if (want_frames) {
        for (int level = 0; level < 16; ++level) {
            lua_Debug f {};
            if (!lua_getstack(L, level, &f)) break;
            lua_getinfo(L, "Slnt", &f);
            const std::string name = f.name ? f.name : (f.what && f.what[0] ? f.what : "?");
            frames.push_back(name + ":" + std::to_string(f.currentline));
            if (level == 0 && f.short_src[0]) source = f.short_src;
        }
    }
    std::lock_guard lk(*b->pos_mu);
    b->pos->line = line;
    if (!source.empty()) b->pos->source = source;
    if (want_frames) b->pos->frames = std::move(frames);
}

int literal_find(lua_State* L) {  // 与 rules 层一致：线性、可中断、禁 pattern
    std::size_t n = 0, pn = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    const char* p = luaL_checklstring(L, 2, &pn);
    if (!lua_toboolean(L, 4)) return luaL_error(L, "string.find requires plain=true");
    auto start = luaL_optinteger(L, 3, 1);
    if (start < 0) start = std::max<lua_Integer>(1, static_cast<lua_Integer>(n) + start + 1);
    if (pn > 4096 || n > 128 * 1024) return luaL_error(L, "literal search size limit exceeded");
    std::array<std::size_t, 4096> prefix{};
    for (std::size_t i = 1, j = 0; i < pn; ++i) {
        while (j && p[i] != p[j]) j = prefix[j - 1];
        if (p[i] == p[j]) ++j;
        prefix[i] = j;
    }
    const auto begin = static_cast<std::size_t>(std::max<lua_Integer>(1, start) - 1);
    auto pos = std::string_view::npos;
    if (!pn && begin <= n) pos = begin;
    for (std::size_t i = begin, j = 0; pn && i < n; ++i) {
        while (j && s[i] != p[j]) j = prefix[j - 1];
        if (s[i] == p[j]) ++j;
        if (j == pn) {
            pos = i + 1 - pn;
            break;
        }
    }
    if (pos == std::string_view::npos) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(pos + 1));
    lua_pushinteger(L, static_cast<lua_Integer>(pos + pn));
    return 2;
}

int bounded_table(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    if (lua_rawlen(L, 1) > 16384) return luaL_error(L, "table sequence limit exceeded");
    const int argc = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, argc, LUA_MULTRET);
    return lua_gettop(L);
}

class Sandbox {
  public:
    Sandbox(std::string script, std::string text, std::string instance_dir, Options opt, std::string ns,
            EventFn event)
        : script_(std::move(script)), text_(std::move(text)), instance_dir_(std::move(instance_dir)),
          opt_(std::move(opt)), event_(std::move(event)) {
        st_.ns = std::move(ns);
        st_.script = script_;
        st_.instance = instance_dir_;
        st_.http_url = opt_.http_url;
        st_.token = opt_.token;
        st_.started_ms = now_ms();
        lim_ = opt_.limits;
        lim_.memory_bytes = std::clamp<std::size_t>(lim_.memory_bytes, 1024 * 1024, 128 * 1024 * 1024);
        lim_.instructions = std::max<std::uint64_t>(lim_.instructions, 1'000'000ull);
    }

    ~Sandbox() {
        if (L_) lua_close(L_);
    }

    void prepare_root() {  // vroot 定位/创建（线程外做，错误直接抛给调用方）
        namespace fs = std::filesystem;
        if (opt_.dry_run) {
            const std::string stem = fs::path(script_).stem().string();
            root_ = (fs::temp_directory_path() / ("mol-script-" + stem + "-" + std::to_string(::getpid())))
                        .string();
        } else if (!opt_.root.empty()) {
            root_ = opt_.root;
        } else if (!instance_dir_.empty()) {
            root_ = (fs::path(instance_dir_) / "scripts" / (fs::path(script_).stem().string() + ".work"))
                        .string();
        } else {
            root_ = (fs::temp_directory_path() /
                     ("mol-script-" + fs::path(script_).stem().string() + "-" + std::to_string(::getpid())))
                        .string();
        }
        std::error_code ec;
        fs::create_directories(root_, ec);
        if (ec) throw Error("io_error", "cannot create the virtual root " + root_ + ": " + ec.message());
        root_ = fs::weakly_canonical(root_, ec).string();
        const int fd = ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) throw Error("io_error", "cannot open the virtual root: " + std::string(std::strerror(errno)));
        root_fd_ = fd;
        st_.root = root_;
    }

    RunResult execute() {
        RunResult res;
        try {
            if (!instance_dir_.empty()) inst_.emplace(load_instance(instance_dir_));
            run_lua();
            std::lock_guard lk(mu_);
            st_.run_state = "done";
            res.ok = true;
        } catch (const std::exception& e) {
            std::lock_guard lk(mu_);
            if (st_.run_state == "running") st_.run_state = "failed";
            res.ok = false;
            res.error = compact_error(e);
            st_.error = res.error;
        }
        st_.finished_ms = now_ms();
        {
            std::lock_guard lk(mu_);
            res.ns = st_.ns;
            res.root = st_.root;
            res.http_url = st_.http_url;
            res.token = st_.token;
            res.landlock = st_.landlock;
            res.log = st_.log;
            res.state = st_.state;
            res.fs_ops = fs_ops_;
            res.proc_runs = proc_runs_;
            res.net_requests = net_requests_;
            st_.fs_ops = fs_ops_;
            st_.proc_runs = proc_runs_;
            st_.net_requests = net_requests_;
            st_.bytes_written = bytes_written_;
        }
        if (event_) event_(event_done(res));
        return res;
    }

    Status snapshot() const {
        std::lock_guard lk(mu_);
        Status st = st_;
        if (budget_.pos) {
            st.instructions = budget_.pos->ticks * 8000;  // 每 tick 1000 条指令，每 8 tick 记一次
            st.lua_line = budget_.pos->line;
            if (!budget_.pos->source.empty()) st.lua_source = budget_.pos->source;
            if (!budget_.pos->frames.empty()) st.frames = budget_.pos->frames;
        }
        return st;
    }

  private:
    // ---- 运行期小工具 -------------------------------------------------------
    // 脚本侧校验失败：alib6 panicf（宏转发字面量，自动带 file:line + 栈；
    // ALIB6_FLAG_USE_EXCEPTIONS 开启时抛 std::runtime_error，sol2 的 trampoline 会接住转成 Lua 错误）。
    // 必须是宏：panicf 的 PanicFormat 是 consteval 的，函数参数没法转发字面量。
#define fail(...) ::alib6::panicf(__VA_ARGS__)

    void check_wall() {
        if (steady_clock::now() > budget_.deadline) fail("wall clock deadline exceeded");
    }

    void count_fs() {
        if (++fs_ops_ > lim_.fs_ops) fail("too many filesystem operations");
    }

  public:
    // HTTP 侧写入：键/值先过校验，失败返回 false（不抛）。
    bool state_set_checked(const std::string& key, const std::string& value) {
        try {
            if (!valid_state_key(key)) return false;
            state_put(key, value);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

  private:

    void log_line(std::string_view msg) {
        std::string line(msg);
        std::replace(line.begin(), line.end(), '\n', ' ');
        std::replace(line.begin(), line.end(), '\r', ' ');
        std::lock_guard lk(mu_);
        if (st_.log.size() >= lim_.log_lines) st_.log.erase(st_.log.begin());
        st_.log.push_back(line);
        if (event_) event_(event_log(line));
    }

    void op_begin(std::string name, std::string detail, std::int64_t timeout_ms = 0) {
        std::lock_guard lk(mu_);
        st_.op = OpInfo{std::move(name), std::move(detail), now_ms(), timeout_ms};
        if (event_) event_(event_op(st_));
    }

    void op_end() {
        std::lock_guard lk(mu_);
        st_.op = OpInfo{};
        if (event_) event_(event_op(st_));
    }

    std::string vroot_path(std::string_view vpath) const { return root_ + "/" + std::string(vpath); }

    void state_put(const std::string& key, const std::string& value) {
        if (value.size() > lim_.state_value_bytes)
            throw Error("limit_exceeded", "state value exceeds the per-value limit");
        std::lock_guard lk(mu_);
        if (!st_.state.count(key) && st_.state.size() >= lim_.state_entries)
            throw Error("limit_exceeded", "too many state keys");
        st_.state[key] = value;
    }

    // ---- Lua 侧 API ---------------------------------------------------------
    // sol2 把 std::vector 推成 userdata（Lua 侧 ipairs/table.concat 用不了），这里统一换成真表。
    sol::table to_lua_table(const std::vector<std::string>& items) {
        sol::state_view lua(L_);
        sol::table t = lua.create_table();
        for (std::size_t i = 0; i < items.size(); ++i) t.set(static_cast<int>(i + 1), items[i]);
        return t;
    }

    sol::optional<std::string> api_fs_read(const std::string& p) {
        check_wall();
        count_fs();
        const auto comps = checked_vpath(p);
        op_begin("fs.read", p);
        auto r = vfs_read(root_fd_, comps, lim_.file_bytes);
        op_end();
        return r ? sol::optional<std::string>(*r) : sol::nullopt;
    }

    void api_fs_write(const std::string& p, const std::string& data, bool append) {
        check_wall();
        count_fs();
        const auto comps = checked_vpath(p);
        op_begin(append ? "fs.append" : "fs.write", p);
        vfs_write(root_fd_, comps, data, append, lim_.file_bytes, bytes_written_, lim_.vroot_bytes);
        op_end();
    }

    bool api_fs_exists(const std::string& p) {
        check_wall();
        count_fs();
        const auto comps = checked_vpath(p);
        op_begin("fs.exists", p);
        const bool r = vfs_exists(root_fd_, comps);
        op_end();
        return r;
    }

    sol::optional<std::uint64_t> api_fs_size(const std::string& p) {
        check_wall();
        count_fs();
        const auto comps = checked_vpath(p);
        op_begin("fs.size", p);
        auto r = vfs_size(root_fd_, comps);
        op_end();
        return r ? sol::optional<std::uint64_t>(*r) : sol::nullopt;
    }

    sol::optional<sol::table> api_fs_list(const std::string& p) {
        check_wall();
        count_fs();
        const auto comps = checked_vpath(p);
        op_begin("fs.list", p);
        auto r = vfs_list(root_fd_, comps);
        op_end();
        return r ? sol::optional<sol::table>(to_lua_table(*r)) : sol::nullopt;
    }

    void api_fs_mkdir(const std::string& p) {
        check_wall();
        count_fs();
        const auto comps = checked_vpath(p);
        op_begin("fs.mkdir", p);
        vfs_mkdir(root_fd_, comps);
        op_end();
    }

    void api_fs_remove(const std::string& p) {
        check_wall();
        count_fs();
        const auto comps = checked_vpath(p);
        op_begin("fs.remove", p);
        vfs_remove(root_fd_, comps);
        op_end();
    }

    void api_fs_copy(const std::string& s, const std::string& d) {
        check_wall();
        count_fs();
        const auto sc = checked_vpath(s), dc = checked_vpath(d);
        op_begin("fs.copy", s + " -> " + d);
        vfs_copy(root_fd_, sc, dc, lim_.file_bytes, bytes_written_, lim_.vroot_bytes);
        op_end();
    }

    void api_fs_move(const std::string& s, const std::string& d) {
        check_wall();
        count_fs();
        const auto sc = checked_vpath(s), dc = checked_vpath(d);
        op_begin("fs.move", s + " -> " + d);
        vfs_move(root_fd_, sc, dc);
        op_end();
    }

    sol::table api_archive_list(const std::string& p) {
        check_wall();
        if (!inst_) fail("archive access needs an instance");
        const auto comps = checked_vpath(p);
        op_begin("archive.list", p);
        auto names = list_archive(vroot_path(p));
        op_end();
        if (names.size() > 20000) fail("archive lists too many entries");
        for (auto& n : names) {
            std::replace(n.begin(), n.end(), '\\', '/');
            if (!valid_vpath(n)) n = "(unsafe entry) " + n;  // 不让归档里的怪路径回到 Lua
        }
        return to_lua_table(names);
    }

    std::size_t api_archive_extract(const std::string& p, const std::string& dest) {
        check_wall();
        if (!inst_) fail("archive access needs an instance");
        checked_vpath(p);
        checked_vpath(dest);
        op_begin("archive.extract", p + " -> " + dest);
        const std::uint64_t before = du_bytes(vroot_path(dest));
        extract_archive(vroot_path(p), vroot_path(dest));
        const std::size_t files = validate_tree(vroot_path(dest));
        const std::uint64_t after = du_bytes(vroot_path(dest));
        if (after > before) {
            if (after - before > lim_.vroot_bytes - std::min(bytes_written_, lim_.vroot_bytes))
                fail("the virtual root's total write budget is exhausted");
            bytes_written_ += after - before;
        }
        // 解包落盘用的是宿主路径，事后复查仍在 vroot 内（防解包过程中被换链接）
        std::error_code ec;
        const auto canon = std::filesystem::weakly_canonical(vroot_path(dest), ec);
        if (ec || canon.string().rfind(root_ + "/", 0) != 0) fail("extracted tree left the virtual root");
        op_end();
        return files;
    }

    // opts 形参统一处理：nil / 缺省 / 表
    static sol::table as_options(sol::object o) {
        if (o.get_type() == sol::type::nil || o.get_type() == sol::type::none) return {};
        if (o.get_type() == sol::type::table) return o.as<sol::table>();
        fail("options must be a table");
    }
    static std::uint64_t opt_u64(const sol::table& t, const char* key, std::uint64_t def, std::uint64_t lo,
                                 std::uint64_t hi) {
        if (!t.valid()) return def;
        const sol::object o = t.raw_get<sol::object>(key);
        if (o.get_type() != sol::type::number) return def;
        const double v = o.as<double>();
        if (!(v >= 0)) return def;
        return static_cast<std::uint64_t>(std::clamp(v, static_cast<double>(lo), static_cast<double>(hi)));
    }

    void net_checks(const std::string& url) {
        if (opt_.dry_run) fail("net access is disabled in --dry-run");
        if (!opt_.net) fail("net access is disabled for this run");
        if (!http_url_ok(url)) fail("only http/https URLs are allowed");
        if (opt_.deny_private && host_is_private(url_host(url)))
            fail("the URL points at a loopback/private address and --deny-private is set");
    }

    sol::table api_net_get(const std::string& url, sol::object opts_obj) {
        check_wall();
        net_checks(url);
        const sol::table opts = as_options(std::move(opts_obj));
        const auto timeout = opt_u64(opts, "timeout_ms", 30000, 100, 300000);
        std::vector<std::pair<std::string, std::string>> headers;
        if (opts.valid()) {
            const sol::object h = opts.raw_get<sol::object>("headers");
            if (h.get_type() == sol::type::table) {
                const sol::table ht = h.as<sol::table>();
                for (auto&& kv : ht) {
                    if (kv.second.get_type() != sol::type::string) fail("headers must be strings");
                    std::string k = kv.first.as<std::string>(), v = kv.second.as<std::string>();
                    if (k.empty() || k.size() > 128 || v.size() > 4096 ||
                        k.find_first_of(":\r\n") != std::string::npos)
                        fail("invalid header");
                    headers.emplace_back(std::move(k), std::move(v));
                    if (headers.size() > 32) fail("too many headers");
                }
            }
        }
        op_begin("net.get", url);
        const auto r = http_get(url, headers, static_cast<long>(timeout / 1000));
        ++net_requests_;
        op_end();
        if (static_cast<std::uint64_t>(r.body.size()) > lim_.net_body_bytes)
            fail("response body exceeds the limit");
        sol::state_view lua(L_);
        return lua.create_table_with("status", static_cast<std::int64_t>(r.status), "body", std::string(r.body));
    }

    std::uint64_t api_net_download(const std::string& url, const std::string& vpath) {
        check_wall();
        net_checks(url);
        const auto comps = checked_vpath(vpath);
        std::vector<std::string> parents(comps.begin(), comps.end() - 1);
        open_below(root_fd_, parents, false);  // 父目录必须已在 vroot 里建好
        if (vfs_exists(root_fd_, comps) && !vfs_size(root_fd_, comps).has_value())
            fail("destination exists and is not a regular file");
        const std::string dest = vroot_path(vpath);
        op_begin("net.download", url + " -> " + vpath);
        const auto budget = lim_.vroot_bytes - std::min(bytes_written_, lim_.vroot_bytes);
        const auto dl = http_download(url, dest, {}, [&](std::uint64_t done, std::uint64_t) {
            return done <= budget && steady_clock::now() <= budget_.deadline;
        });
        ++net_requests_;
        op_end();
        std::error_code ec;
        const auto canon = std::filesystem::weakly_canonical(dest, ec);
        if (ec || canon.string().rfind(root_ + "/", 0) != 0) fail("download left the virtual root");
        bytes_written_ += dl;
        return dl;
    }

    sol::table api_proc_run(const std::string& exe, sol::object opts_obj) {
        check_wall();
        const sol::table opts = as_options(std::move(opts_obj));
        // 先校验「这是个能跑的 Windows exe」再要实例：明显的用法错误先报，且不需要实例也能验
        const auto comps = checked_vpath(exe);
        const std::string leaf = comps.back();
        std::string lower = leaf;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower.size() < 4 || lower.substr(lower.size() - 4) != ".exe")
            fail("only Windows .exe files can be run");
        if (!vfs_exists(root_fd_, comps)) fail("executable not found in the virtual root");
        if (!vfs_size(root_fd_, comps).has_value()) fail("not a regular file");
        const std::string host = vroot_path(exe);
        const std::string arch = pe_machine(host);
        if (arch == "unknown") fail("not a Windows executable (no PE header)");
        if (arch == "arm64") fail("this host can only run 32/64-bit x86 executables");
        if (arch == "i386") log_line("note: 32-bit executable; containment relies on Landlock, not LD_PRELOAD hooks");
        if (opt_.dry_run) fail("running executables is disabled in --dry-run");
        if (!inst_) fail("running executables needs an instance (script run without one has no runner)");

        std::vector<std::string> args;
        if (opts.valid()) {
            const sol::object a = opts.raw_get<sol::object>("args");
            if (a.get_type() == sol::type::table) {
                const sol::table at = a.as<sol::table>();
                for (std::size_t i = 1; i <= at.size(); ++i) {
                    const sol::object v = at.raw_get<sol::object>(i);
                    if (v.get_type() != sol::type::string) fail("args must be strings");
                    std::string s = v.as<std::string>();
                    if (s.size() > 4096 || s.find('\0') != std::string::npos) fail("argument too long");
                    args.push_back(std::move(s));
                    if (args.size() > 64) fail("too many arguments");
                }
            }
        }
        std::vector<std::pair<std::string, std::string>> env;
        if (opts.valid()) {
            const sol::object e = opts.raw_get<sol::object>("env");
            if (e.get_type() == sol::type::table) {
                const sol::table et = e.as<sol::table>();
                for (auto&& kv : et) {
                    if (kv.second.get_type() != sol::type::string) fail("env values must be strings");
                    std::string k = kv.first.as<std::string>(), v = kv.second.as<std::string>();
                    if (!env_key_ok(k)) fail("environment variable name is not allowed: {}", k);
                    if (v.size() > 8192) fail("environment value too long");
                    env.emplace_back(std::move(k), std::move(v));
                    if (env.size() > 64) fail("too many environment variables");
                }
            }
        }
        const auto timeout = opt_u64(opts, "timeout_ms", lim_.proc_timeout_ms, 1000, 6ull * 3600 * 1000);
        std::string cwd_v = ".";
        if (opts.valid()) {
            const sol::object c = opts.raw_get<sol::object>("cwd");
            if (c.get_type() == sol::type::string) cwd_v = c.as<std::string>();
        }
        const auto cwd_comps = checked_vpath(cwd_v);
        if (!vfs_exists(root_fd_, cwd_comps)) fail("working directory does not exist");
        op_begin("proc.run", leaf + " " + join_args(args), static_cast<std::int64_t>(timeout));

        // 组装启动命令（与 build_launch 同语义：proton run / wine）
        std::vector<std::string> argv;
        std::vector<std::pair<std::string, std::string>> menv;
        const auto& cfg = inst_->cfg;
        const std::string exe_host = host;
        if (cfg.runner_kind == "proton") {
            const std::string proton_dir(cfg.proton_path);
            if (proton_dir.empty()) fail("runner 'proton' requires proton_path in mo-linux.json");
            argv.push_back(proton_dir + (proton_dir.back() == '/' ? "" : "/") + "proton");
            argv.emplace_back("run");
            argv.push_back(exe_host);
            std::string_view compat(cfg.prefix.data(), cfg.prefix.size());
            if (compat.size() >= 4 && compat.substr(compat.size() - 4) == "/pfx") compat.remove_suffix(4);
            if (!compat.empty()) menv.emplace_back("STEAM_COMPAT_DATA_PATH", std::string(compat));
            const std::string steam_root(cfg.steam_root);
            if (!steam_root.empty()) menv.emplace_back("STEAM_COMPAT_CLIENT_INSTALL_PATH", steam_root);
            menv.emplace_back("STEAM_COMPAT_APP_ID", "489830");
            menv.emplace_back("SteamAppId", "489830");
            menv.emplace_back("SteamGameId", "489830");
            std::string mounts;
            for (std::string_view d : {std::string_view(inst_->farm_path.data(), inst_->farm_path.size()),
                                       std::string_view(inst_->root.data(), inst_->root.size()),
                                       std::string_view(inst_->mods_dir.data(), inst_->mods_dir.size()),
                                       std::string_view(inst_->overwrite_dir.data(), inst_->overwrite_dir.size()),
                                       std::string_view(cfg.game_dir.data(), cfg.game_dir.size())}) {
                if (d.empty()) continue;
                if (!mounts.empty()) mounts.push_back(':');
                mounts.append(d);
            }
            mounts.push_back(':');
            mounts.append(root_);
            menv.emplace_back("STEAM_COMPAT_MOUNTS", mounts);
            menv.emplace_back("PRESSURE_VESSEL_FILESYSTEMS_RW", mounts);
        } else if (cfg.runner_kind == "wine") {
            argv.emplace_back("wine");
            argv.push_back(exe_host);
            const std::string prefix(cfg.prefix);
            if (!prefix.empty()) menv.emplace_back("WINEPREFIX", prefix);
            if (!std::getenv("WINEDEBUG")) menv.emplace_back("WINEDEBUG", "-all");
        } else {
            fail("unknown runner_kind: {}", std::string(cfg.runner_kind));
        }
        for (auto& a : args) argv.push_back(a);
        for (auto& [k, v] : env) menv.emplace_back(k, v);  // 脚本 env（已过滤注入变量）

        // Landlock：读全盘、只写 vroot + prefix + 临时目录
        std::vector<Rule> rules;
        rules.emplace_back("/", kReadOnly);
        rules.emplace_back(root_, kAbi1All);
        const char* tmpdir = std::getenv("TMPDIR");
        const std::string tmp = tmpdir && *tmpdir ? std::string(tmpdir) : "/tmp";
        rules.emplace_back(tmp, kAbi1All);
        if (!cfg.prefix.empty()) {
            const std::string prefix(cfg.prefix);
            std::error_code ec2;
            if (std::filesystem::is_directory(prefix, ec2)) {
                const auto canon = std::filesystem::weakly_canonical(prefix, ec2);
                rules.emplace_back(ec2 ? prefix : canon.string(), kAbi1All);
            } else {
                fail("the Wine prefix does not exist ({}); run `mo-linux instance init` first", prefix);
            }
        }
        std::error_code ec;
        const std::string cwd = std::filesystem::weakly_canonical(vroot_path(cwd_v), ec).string();
        if (++proc_runs_ > lim_.proc_runs) fail("too many executable runs");
        const std::string log_rel = ".mol-logs/proc-" + std::to_string(proc_runs_) + ".log";
        vfs_mkdir(root_fd_, split_vpath(".mol-logs"));
        const ProcOutcome out = spawn_exe(argv, menv, cwd, vroot_path(log_rel), timeout, rules);
        {
            std::lock_guard lk(mu_);
            st_.landlock = out.landlock;
        }
        op_end();
        sol::state_view lua(L_);
        return lua.create_table_with("exit", out.exit_code, "log", log_rel, "arch", out.arch,
                                     "landlock", out.landlock, "timed_out", out.timed_out);
    }

    sol::optional<std::string> api_state_get(const std::string& key, sol::object def) {
        check_wall();
        if (!valid_state_key(key)) fail("invalid state key");
        std::lock_guard lk(mu_);
        const auto it = st_.state.find(key);
        if (it != st_.state.end()) return it->second;
        if (def.is<std::string>()) return def.as<std::string>();
        return sol::nullopt;
    }

    void api_state_set(const std::string& key, sol::object value) {
        check_wall();
        if (!valid_state_key(key)) fail("invalid state key");
        std::string v;
        if (value.is<std::string>()) {
            v = value.as<std::string>();
        } else if (value.is<bool>()) {
            v = value.as<bool>() ? "true" : "false";
        } else if (value.is<long long>() || value.is<double>()) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.14g", value.is<long long>() ? static_cast<double>(value.as<long long>())
                                                                          : value.as<double>());
            v = buf;
        } else {
            fail("state values must be string/number/boolean");
        }
        state_put(key, v);
    }

    void api_state_delete(const std::string& key) {
        check_wall();
        if (!valid_state_key(key)) fail("invalid state key");
        std::lock_guard lk(mu_);
        st_.state.erase(key);
    }

    sol::table api_state_keys() {
        check_wall();
        std::lock_guard lk(mu_);
        std::vector<std::string> keys;
        for (const auto& [k, v] : st_.state) keys.push_back(k);
        return to_lua_table(keys);
    }

    // ---- WP2：实例写 API（创建实例 / 装 mod / 部署） --------------------------
    // 所有写操作：dry-run 一律拒绝；按实例根串行；op 自省照旧。
    void wp2_checks() {
        check_wall();
        if (opt_.dry_run) fail("instance writes are disabled in --dry-run");
        if (!inst_) fail("this run has no instance (script run needs one for instance writes)");
    }

    static std::string opt_string(const sol::table& t, const char* key) {
        if (!t.valid()) return {};
        const sol::object o = t.raw_get<sol::object>(key);
        if (o.get_type() != sol::type::string) return {};
        const std::string s = o.as<std::string>();
        if (s.size() > 4096) fail("option value too long");
        return s;
    }
    static bool opt_bool(const sol::table& t, const char* key, bool def) {
        if (!t.valid()) return def;
        const sol::object o = t.raw_get<sol::object>(key);
        return o.is<bool>() ? o.as<bool>() : def;
    }

    sol::table api_instance_create(sol::object opts_obj) {
        check_wall();
        if (opt_.dry_run) fail("creating an instance is disabled in --dry-run");
        const sol::table o = as_options(std::move(opts_obj));
        // InitOptions 持 string_view：先用局部 string 承接，保证视图有效
        const std::string root = opt_string(o, "root");
        const std::string game_dir = opt_string(o, "game_dir");
        const std::string prefix = opt_string(o, "prefix");
        const std::string prefix_user = opt_string(o, "prefix_user");
        const std::string profile = opt_string(o, "profile");
        const std::string runner = opt_string(o, "runner");
        const std::string proton_path = opt_string(o, "proton_path");
        const std::string steam_root = opt_string(o, "steam_root");
        const std::string game = opt_string(o, "game");
        if (root.empty()) fail("instance.create needs a root");
        if (game_dir.empty()) fail("instance.create needs game_dir (an existing game directory)");
        if (root.size() > 4096 || game_dir.size() > 4096) fail("path too long");
        for (const std::string* p : {&root, &game_dir, &prefix, &proton_path, &steam_root})
            if (!p->empty() && p->front() != '/')
                fail("instance paths must be absolute (this API writes outside the virtual root)");
        if (!runner.empty() && runner != "proton" && runner != "wine")
            fail("runner must be proton or wine");
        if (runner == "proton" && proton_path.empty()) fail("runner 'proton' needs proton_path");
        std::error_code ec;
        if (!std::filesystem::is_directory(game_dir, ec))
            fail("game_dir does not exist: {}", game_dir);
        InitOptions io;
        io.root = root;
        io.game_dir = game_dir;
        io.prefix = prefix;
        io.prefix_user = prefix_user;
        io.profile = profile;
        io.runner_kind = runner;
        io.proton_path = proton_path;
        io.steam_root = steam_root;
        io.game = game;
        op_begin("instance.create", root);
        std::lock_guard lk(instance_write_lock(root));
        const bool changed = init_instance(io);
        inst_.emplace(load_instance(root));  // 之后 mods/farm API 都指向新实例
        instance_dir_ = std::string(inst_->root);
        st_.instance = instance_dir_;
        op_end();
        sol::state_view lua(L_);
        return lua.create_table_with("root", std::string(inst_->root), "changed", changed,
                                     "game", std::string(inst_->cfg.game),
                                     "profile", std::string(inst_->cfg.profile));
    }

    sol::table api_instance_info() {
        check_wall();
        if (!inst_) fail("this run has no instance");
        sol::state_view lua(L_);
        return lua.create_table_with(
            "root", std::string(inst_->root), "game", std::string(inst_->cfg.game),
            "game_dir", std::string(inst_->cfg.game_dir), "prefix", std::string(inst_->cfg.prefix),
            "profile", std::string(inst_->cfg.profile), "farm", std::string(inst_->farm_path),
            "mods", std::string(inst_->mods_dir), "downloads", std::string(inst_->downloads_dir),
            "overwrite", std::string(inst_->overwrite_dir), "runner", std::string(inst_->cfg.runner_kind));
    }

    fomod::Choices parse_choices(const sol::object& o) {
        fomod::Choices out;
        if (o.get_type() != sol::type::table) return out;
        const sol::table steps = o.as<sol::table>();
        for (auto&& s : steps) {
            if (s.second.get_type() != sol::type::table) fail("choices must be step -> group -> {{plugins}}");
            const std::string step = s.first.as<std::string>();
            const sol::table groups = s.second.as<sol::table>();
            for (auto&& g : groups) {
                if (g.second.get_type() != sol::type::table)
                    fail("choices must be step -> group -> {{plugins}}");
                const std::string group = g.first.as<std::string>();
                const sol::table plugins = g.second.as<sol::table>();
                auto& slot = out[step][group];
                for (auto&& p : plugins) {
                    if (p.second.get_type() != sol::type::string) fail("plugin names must be strings");
                    slot.insert(p.second.as<std::string>());
                }
            }
        }
        return out;
    }

    sol::table api_mods_install_archive(const std::string& vpath, const std::string& name, sol::object opts_obj) {
        wp2_checks();
        checked_vpath(vpath);
        const sol::table o = as_options(std::move(opts_obj));
        InstallOptions io;
        io.name = name;
        io.profile = inst_->cfg.profile;
        io.force_root = opt_bool(o, "root", false);
        io.replace_existing = opt_bool(o, "replace", false);
        const std::string fomod = opt_string(o, "fomod");
        if (fomod == "raw") io.fomod = FomodMode::Raw;
        else if (fomod == "defaults") io.fomod = FomodMode::Defaults;
        else if (fomod == "choices") {
            io.fomod = FomodMode::Choices;
            io.choices = parse_choices(o.valid() ? o.raw_get<sol::object>("choices") : sol::object());
        } else if (!fomod.empty()) {
            fail("fomod must be 'defaults', 'choices' or 'raw'");
        }
        op_begin("mods.install_archive", vpath + (name.empty() ? "" : " -> " + name));
        std::lock_guard lk(instance_write_lock(std::string(inst_->root)));
        const InstallResult r = install_archive(*inst_, vroot_path(vpath), io);
        op_end();
        sol::state_view lua(L_);
        return lua.create_table_with("name", std::string(r.name), "path", std::string(r.path), "root", r.root,
                                     "files", static_cast<std::int64_t>(r.files), "fomod", r.fomod);
    }

    sol::table api_mods_install_staged(const std::string& name, const std::string& vpath, sol::object opts_obj) {
        wp2_checks();
        checked_vpath(vpath);
        if (name.empty()) fail("install_staged needs a mod name");
        const sol::table o = as_options(std::move(opts_obj));
        InstallOptions io;
        io.name = name;
        io.profile = inst_->cfg.profile;
        io.force_root = opt_bool(o, "root", false);
        io.replace_existing = opt_bool(o, "replace", false);
        op_begin("mods.install_staged", vpath + " -> " + name);
        std::lock_guard lk(instance_write_lock(std::string(inst_->root)));
        const InstallResult r = install_directory(*inst_, vroot_path(vpath), io);
        op_end();
        sol::state_view lua(L_);
        return lua.create_table_with("name", std::string(r.name), "path", std::string(r.path), "root", r.root,
                                     "files", static_cast<std::int64_t>(r.files));
    }

    sol::table api_mods_set_enabled(const std::string& name, bool enabled) {
        wp2_checks();
        if (name.empty() || name.find('\n') != std::string::npos) fail("invalid mod name");
        op_begin(enabled ? "mods.enable" : "mods.disable", name);
        std::lock_guard lk(instance_write_lock(std::string(inst_->root)));
        const bool changed = set_mod_enabled(*inst_, name, enabled, inst_->cfg.profile);
        op_end();
        sol::state_view lua(L_);
        return lua.create_table_with("name", name, "enabled", enabled, "changed", changed);
    }

    sol::table api_mods_list() {
        check_wall();
        if (!inst_) fail("this run has no instance");
        const auto mods = list_mods(*inst_, inst_->cfg.profile);
        sol::state_view lua(L_);
        sol::table out = lua.create_table();
        int n = 0;
        for (const auto& m : mods) {
            if (m.separator) continue;
            sol::table row = lua.create_table_with(
                "name", std::string(m.name), "enabled", m.enabled, "exists", m.exists, "root", m.root,
                "version", std::string(m.version), "nexus_id", static_cast<std::int64_t>(m.nexus_id),
                "priority", static_cast<std::int64_t>(m.priority));
            out.set(++n, row);
        }
        return out;
    }

    sol::table api_farm_apply() {
        wp2_checks();
        op_begin("farm.apply", std::string(inst_->farm_path));
        std::lock_guard lk(instance_write_lock(std::string(inst_->root)));
        mol::require_farm_idle(*inst_);
        const FarmModel model = build_farm_model(*inst_, inst_->cfg.profile);
        const Plan plan = plan_instance(*inst_, model);
        apply_instance(*inst_, plan);
        op_end();
        sol::state_view lua(L_);
        return lua.create_table_with("applied", static_cast<std::int64_t>(plan.ops.size()),
                                     "changed", !plan.ops.empty(), "farm", std::string(inst_->farm_path));
    }

    // WP-C：安装后自检——脚本刚装完的 mod 到底注入了什么、能碰什么
    sol::table api_impact_of(const std::string& name) {
        check_wall();
        if (!inst_) fail("impact analysis needs an instance");
        if (name.empty() || name.size() > 4096) fail("invalid mod name");
        const auto m = impact::analyze_mod(*inst_, name);
        sol::state_view lua(L_);
        sol::table inj = lua.create_table();
        int n = 0;
        for (const auto& i : m.injections)
            inj.set(++n, lua.create_table_with("kind", std::string(i.kind), "path", std::string(i.path),
                                               "loaded_by", std::string(i.loaded_by),
                                               "reach", std::string(i.reach)));
        sol::table caps = lua.create_table_with(
            "writes_files", m.caps.writes_files, "spawns_processes", m.caps.spawns_processes,
            "network", m.caps.network, "registry", m.caps.registry, "memory_patch", m.caps.memory_patch,
            "chain_loads", m.caps.chain_loads, "unknown", m.caps.unknown);
        return lua.create_table_with("mod", std::string(m.mod), "summary", std::string(m.summary),
                                     "packed_suspect", m.packed_suspect, "injections", inj, "caps", caps);
    }

    void api_log(const std::string& msg) {
        if (msg.size() > 4096) fail("log line too long");
        log_line(msg);
    }

    void run_lua() {
        budget_.limit = lim_.memory_bytes;
        budget_.ticks = std::max<std::uint64_t>(1, lim_.instructions / 1000);
        budget_.deadline = steady_clock::now() + milliseconds(lim_.wall_ms);
        budget_.pos = &pos_storage_;
        budget_.pos_mu = &mu_;
        L_ = lua_newstate(lua_alloc, &budget_);
        if (!L_) throw Error("io_error", "cannot allocate the Lua state");
        *static_cast<Budget**>(lua_getextraspace(L_)) = &budget_;
        luaL_requiref(L_, "_G", luaopen_base, 1);
        lua_pop(L_, 1);
        for (const char* n : {"dofile", "loadfile", "load", "collectgarbage", "pcall", "xpcall", "print",
                              "warn", "setmetatable", "getmetatable", "require", "module", "rawlen",
                              "rawequal"}) {
            lua_pushnil(L_);
            lua_setglobal(L_, n);
        }
        luaL_requiref(L_, "string", luaopen_string, 1);
        for (const char* n : {"match", "gmatch", "gsub", "dump", "format", "pack", "unpack", "packsize"}) {
            lua_pushnil(L_);
            lua_setfield(L_, -2, n);
        }
        lua_pushcfunction(L_, literal_find);
        lua_setfield(L_, -2, "find");
        lua_pop(L_, 1);
        luaL_requiref(L_, "table", luaopen_table, 1);
        lua_pushnil(L_);
        lua_setfield(L_, -2, "move");
        for (const char* n : {"sort", "insert", "remove", "concat", "unpack"}) {
            lua_getfield(L_, -1, n);
            lua_pushcclosure(L_, bounded_table, 1);
            lua_setfield(L_, -2, n);
        }
        lua_pop(L_, 1);
        luaL_requiref(L_, "math", luaopen_math, 1);
        lua_pop(L_, 1);
        // os 只留 clock/time/date（纯函数）；io/debug/package 一律不给
        luaL_requiref(L_, "os", luaopen_os, 1);
        for (const char* n : {"execute", "exit", "remove", "rename", "tmpname", "getenv", "setlocale"}) {
            lua_pushnil(L_);
            lua_setfield(L_, -2, n);
        }
        lua_pop(L_, 1);

        // 注册宿主表。注意：函数必须用 set_function——本 sol2 构建下 table::set / create_table_with
        // 传 lambda 会把闭包绑到错误的函数上（实测调用方拿到的是别表的函数），只有
        // table_proxy::operator=（内部走 set_function）这条路径正确。
        sol::state_view lua(L_);
        auto table_for = [&lua](const char* name) {
            sol::table t = lua.create_table();
            lua[name] = t;
            return t;
        };
        if (sol::table t = table_for("host")) t.set("name", std::string("mo-linux"));
        if (sol::table t = table_for("log")) {
            t.set_function("info", [this](const std::string& m) { api_log(m); });
            t.set_function("warn", [this](const std::string& m) { api_log(m); });
            t.set_function("error", [this](const std::string& m) { api_log(m); });
        }
        if (sol::table t = table_for("fs")) {
            t.set_function("read", [this](sol::this_state, const std::string& p) { return api_fs_read(p); });
            t.set_function("write", [this](sol::this_state, const std::string& p, const std::string& d) { api_fs_write(p, d, false); return true; });
            t.set_function("append", [this](sol::this_state, const std::string& p, const std::string& d) { api_fs_write(p, d, true); return true; });
            t.set_function("exists", [this](sol::this_state, const std::string& p) { return api_fs_exists(p); });
            t.set_function("size", [this](sol::this_state, const std::string& p) { return api_fs_size(p); });
            t.set_function("list", [this](sol::this_state, const std::string& p) { return api_fs_list(p); });
            t.set_function("mkdir", [this](sol::this_state, const std::string& p) { api_fs_mkdir(p); return true; });
            t.set_function("remove", [this](sol::this_state, const std::string& p) { api_fs_remove(p); return true; });
            t.set_function("copy", [this](sol::this_state, const std::string& s, const std::string& d) { api_fs_copy(s, d); return true; });
            t.set_function("move", [this](sol::this_state, const std::string& s, const std::string& d) { api_fs_move(s, d); return true; });
        }
        if (sol::table t = table_for("archive")) {
            t.set_function("list", [this](sol::this_state, const std::string& p) { return api_archive_list(p); });
            t.set_function("extract", [this](sol::this_state, const std::string& p, const std::string& d) {
                return static_cast<std::int64_t>(api_archive_extract(p, d));
            });
        }
        if (sol::table t = table_for("net")) {
            t.set_function("get", [this](sol::this_state, const std::string& u, sol::object o) { return api_net_get(u, std::move(o)); });
            t.set_function("download", [this](sol::this_state, const std::string& u, const std::string& p) {
                return static_cast<std::uint64_t>(api_net_download(u, p));
            });
        }
        if (sol::table t = table_for("proc")) {
            t.set_function("run", [this](sol::this_state, const std::string& e, sol::object o) { return api_proc_run(e, std::move(o)); });
        }
        if (sol::table t = table_for("state")) {
            t.set_function("get", [this](sol::this_state, const std::string& k, sol::object d) { return api_state_get(k, std::move(d)); });
            t.set_function("set", [this](sol::this_state, const std::string& k, sol::object v) { api_state_set(k, std::move(v)); return true; });
            t.set_function("delete", [this](sol::this_state, const std::string& k) { api_state_delete(k); return true; });
            t.set_function("keys", [this](sol::this_state) { return api_state_keys(); });
        }
        if (sol::table t = table_for("instance")) {
            t.set_function("create", [this](sol::this_state, sol::object o) { return api_instance_create(std::move(o)); });
            t.set_function("info", [this](sol::this_state) { return api_instance_info(); });
        }
        if (sol::table t = table_for("mods")) {
            t.set_function("install_archive", [this](sol::this_state, const std::string& p, const std::string& n, sol::object o) { return api_mods_install_archive(p, n, std::move(o)); });
            t.set_function("install_staged", [this](sol::this_state, const std::string& n, const std::string& p, sol::object o) { return api_mods_install_staged(n, p, std::move(o)); });
            t.set_function("enable", [this](sol::this_state, const std::string& n) { return api_mods_set_enabled(n, true); });
            t.set_function("disable", [this](sol::this_state, const std::string& n) { return api_mods_set_enabled(n, false); });
            t.set_function("list", [this](sol::this_state) { return api_mods_list(); });
        }
        if (sol::table t = table_for("farm")) {
            t.set_function("apply", [this](sol::this_state) { return api_farm_apply(); });
        }
        if (sol::table t = table_for("impact")) {
            t.set_function("of", [this](sol::this_state, const std::string& name) { return api_impact_of(name); });
        }

        auto loaded = lua.load(text_, script_, sol::load_mode::text);
        if (!loaded.valid()) throw Error("script_error", "cannot load the Lua source");
        sol::protected_function fn = loaded;
        // 计数 hook：指令预算 + 墙钟 + 自省位置。装在整个执行期。
        lua_sethook(L_, count_hook, LUA_MASKCOUNT, 1000);
        const auto r = fn();
        if (!r.valid()) {
            const sol::error err = r;
            throw Error("script_error", err.what());
        }
        lua_sethook(L_, nullptr, 0, 0);
    }

    // ---- 成员 ---------------------------------------------------------------
    std::string script_, text_, instance_dir_;
    Options opt_;
    Limits lim_;
    EventFn event_;
    std::string root_;
    int root_fd_ = -1;
    std::optional<Instance> inst_;
    Budget budget_{};
    Budget::Position pos_storage_{};
    lua_State* L_ = nullptr;
    std::uint64_t fs_ops_ = 0, proc_runs_ = 0, net_requests_ = 0, bytes_written_ = 0;
    mutable std::mutex mu_;
    Status st_;
};

// ---------------------------------------------------------------------------
// Run / Registry
// ---------------------------------------------------------------------------
class LocalRun : public Run {
  public:
    LocalRun(std::string script, std::string text, std::string instance_dir, Options opt, std::string ns,
             EventFn event) {
        sb_ = std::make_unique<Sandbox>(std::move(script), std::move(text), std::move(instance_dir),
                                        std::move(opt), std::move(ns), std::move(event));
    }
    ~LocalRun() override {
        if (th_.joinable()) th_.join();
    }
    void start() {
        sb_->prepare_root();  // vroot 定位/创建在调用方线程：错误直接抛给调用方
        th_ = std::thread([this] {
            res_ = sb_->execute();
            done_ = true;
        });
    }
    void join() override {
        if (th_.joinable()) th_.join();
    }
    [[nodiscard]] bool done() const override { return done_.load(); }
    [[nodiscard]] std::string ns() const override { return sb_->snapshot().ns; }
    [[nodiscard]] Status status() const override { return sb_->snapshot(); }
    [[nodiscard]] RunResult result() const override {
        std::lock_guard lk(m_);
        return res_;
    }
    bool set_state(const std::string& key, const std::string& value) override {
        return sb_ && sb_->state_set_checked(key, value);
    }

  private:
    std::unique_ptr<Sandbox> sb_;
    std::thread th_;
    mutable std::mutex m_;
    RunResult res_;
    std::atomic<bool> done_{false};
};

class RegistryImpl : public Registry {
  public:
    void add(const RunPtr& run) override {
        std::lock_guard lk(mu_);
        if (!valid_ns(run->ns())) throw Error("invalid_argument", "invalid namespace: " + run->ns());
        if (runs_.count(run->ns())) throw Error("invalid_argument", "namespace already in use: " + run->ns());
        runs_.emplace(run->ns(), run);
    }
    void remove(std::string_view ns) override {
        std::lock_guard lk(mu_);
        runs_.erase(std::string(ns));
    }
    [[nodiscard]] std::vector<RunPtr> runs() const override {
        std::lock_guard lk(mu_);
        std::vector<RunPtr> out;
        for (const auto& [k, v] : runs_) out.push_back(v);
        std::sort(out.begin(), out.end(), [](const RunPtr& a, const RunPtr& b) { return a->ns() < b->ns(); });
        return out;
    }
    [[nodiscard]] RunPtr find(std::string_view ns) const override {
        std::lock_guard lk(mu_);
        const auto it = runs_.find(std::string(ns));
        return it == runs_.end() ? nullptr : it->second;
    }
    [[nodiscard]] std::size_t size() const override {
        std::lock_guard lk(mu_);
        return runs_.size();
    }

  private:
    mutable std::mutex mu_;
    std::map<std::string, RunPtr> runs_;
};

}  // namespace

std::string unique_ns(const std::vector<std::string>& taken, std::string_view want) {
    std::set<std::string> used(taken.begin(), taken.end());
    if (!want.empty()) {
        const std::string base(want);
        if (!valid_ns(base)) throw Error("invalid_argument", "invalid namespace '" + base + "'");
        if (!used.count(base)) return base;
        for (int i = 2; i < 1000; ++i) {
            const std::string cand = base + "-" + std::to_string(i);
            if (cand.size() > 32) break;
            if (!used.count(cand)) return cand;
        }
        throw Error("invalid_argument", "namespace '" + base + "' is taken and no suffix is free");
    }
    for (int i = 0; i < 1000; ++i) {
        const std::string cand = "script_" + std::to_string(i);
        if (!used.count(cand)) return cand;
    }
    throw Error("invalid_argument", "no free script namespace");
}

RunPtr local_run(std::string script, std::string text, std::string instance_dir, Options opt, std::string ns,
                 EventFn event) {
    if (!valid_ns(ns)) throw Error("invalid_argument", "invalid namespace: " + ns);
    return std::make_shared<LocalRun>(std::move(script), std::move(text), std::move(instance_dir),
                                      std::move(opt), std::move(ns), std::move(event));
}

RegistryPtr make_registry() { return std::make_shared<RegistryImpl>(); }

}  // namespace mol::script
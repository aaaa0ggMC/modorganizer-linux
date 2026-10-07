#include "mol/reflink.hpp"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "mol/casefold.hpp"

namespace mol {
namespace fs = std::filesystem;
namespace {

struct Fd {
    int fd = -1;
    explicit Fd(int f) : fd(f) {}
    ~Fd() { if (fd >= 0) ::close(fd); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};

bool clone_file(const fs::path& src, const fs::path& dst) {
    Fd in(::open(src.c_str(), O_RDONLY | O_CLOEXEC));
    if (in.fd < 0) return false;
    struct stat st{};
    if (::fstat(in.fd, &st) != 0) return false;
    Fd out(::open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 07777));
    if (out.fd < 0) return false;
    return ::ioctl(out.fd, FICLONE, in.fd) == 0;
}

}  // namespace

bool reflink_tree(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    if (fs::exists(fs::symlink_status(dst, ec))) return false;
    fs::create_directories(dst, ec);
    if (ec) return false;
    bool ok = true;
    for (fs::recursive_directory_iterator it(src, ec), end; ok && !ec && it != end; it.increment(ec)) {
        const fs::path to = dst / it->path().lexically_relative(src);
        const auto st = it->symlink_status(ec);
        if (fs::is_directory(st)) fs::create_directories(to, ec);
        else if (fs::is_regular_file(st)) ok = clone_file(it->path(), to);
        else ok = false;  // mod 里不该有符号链接/设备文件
        if (ec) ok = false;
    }
    if (ec) ok = false;
    if (!ok) fs::remove_all(dst, ec);
    return ok;
}

DedupeStats dedupe_tree(const fs::path& dir, const fs::path& ref) {
    DedupeStats out;
    std::error_code ec;
    std::map<std::string, fs::path> refs;  // casefold(相对路径) → 文件
    for (fs::recursive_directory_iterator it(ref, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) refs.emplace(std::string(casefold(it->path().lexically_relative(ref).generic_string())), it->path());
    ec.clear();
    // file_dedupe_range 后面跟一个 file_dedupe_range_info
    std::vector<unsigned char> buf(sizeof(file_dedupe_range) + sizeof(file_dedupe_range_info));
    auto* req = reinterpret_cast<file_dedupe_range*>(buf.data());
    constexpr std::uint64_t kChunk = 16ull << 20;  // btrfs 单次上限 16 MiB
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto rit = refs.find(std::string(casefold(it->path().lexically_relative(dir).generic_string())));
        if (rit == refs.end()) continue;
        const auto size = it->file_size(ec);
        if (ec || size == 0 || fs::file_size(rit->second, ec) != size) { ec.clear(); continue; }
        Fd src(::open(rit->second.c_str(), O_RDONLY | O_CLOEXEC));
        Fd dst(::open(it->path().c_str(), O_RDWR | O_CLOEXEC));  // 目标要可写
        if (src.fd < 0 || dst.fd < 0) continue;
        std::uint64_t done = 0;
        bool same = true;
        while (same && done < size) {
            const std::uint64_t len = std::min<std::uint64_t>(kChunk, size - done);
            std::fill(buf.begin(), buf.end(), 0);
            req->src_offset = done;
            req->src_length = len;
            req->dest_count = 1;
            req->info[0].dest_fd = dst.fd;
            req->info[0].dest_offset = done;
            if (::ioctl(src.fd, FIDEDUPERANGE, req) != 0 || req->info[0].status != FILE_DEDUPE_RANGE_SAME) { same = false; break; }
            done += req->info[0].bytes_deduped ? req->info[0].bytes_deduped : len;
        }
        if (same && done >= size) {
            ++out.files;
            out.bytes += size;
        }
    }
    return out;
}

}  // namespace mol

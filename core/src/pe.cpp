// PE 读取器实现：手写结构体解析，全部边界检查。有界：文件 ≤ 512 MiB、
// 导入 ≤ 8192、导出 ≤ 8192、节 ≤ 32；任何越界/畸形 → nullopt（调用方降级该文件）。
#include "mol/pe.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

namespace mol::pe {
namespace {

constexpr std::size_t kMaxFile = 512ull * 1024 * 1024;
constexpr std::size_t kMaxImports = 8192;
constexpr std::size_t kMaxExports = 8192;
constexpr std::size_t kMaxSections = 32;
constexpr std::uint32_t kDirExport = 0, kDirImport = 1;

std::uint16_t rd16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t rd32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t rd64(const unsigned char* p) {
    return static_cast<std::uint64_t>(rd32(p)) | (static_cast<std::uint64_t>(rd32(p + 4)) << 32);
}

// 读整个文件（只读我们需要的字节；PE 头+目录都在文件里）
bool read_file(const std::string& path, std::vector<unsigned char>& out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0 || static_cast<std::uint64_t>(st.st_size) > kMaxFile ||
        !S_ISREG(st.st_mode)) {
        ::close(fd);
        return false;
    }
    out.resize(static_cast<std::size_t>(st.st_size));
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t n = ::read(fd, out.data() + done, out.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += static_cast<std::size_t>(n);
    }
    ::close(fd);
    out.resize(done);
    return done == static_cast<std::size_t>(st.st_size);
}

// 节内 RVA → 文件偏移；不在任何节里返回 nullopt（头部 RVA 由调用方单独处理）
std::optional<std::size_t> rva_to_off(const std::vector<unsigned char>& b, std::uint32_t rva,
                                      const std::vector<Section>& secs, std::size_t sections_off,
                                      std::size_t headers_size) {
    if (rva < headers_size) return static_cast<std::size_t>(rva);  // 头部的目录/字符串
    for (std::size_t i = 0; i < secs.size(); ++i) {
        const std::size_t off = sections_off + i * 40;
        if (off + 40 > b.size()) break;
        const std::uint32_t va = rd32(&b[off + 12]);
        const std::uint32_t raw_size = rd32(&b[off + 16]);
        const std::uint32_t raw_ptr = rd32(&b[off + 20]);
        if (raw_size == 0) continue;
        if (rva >= va && rva < va + raw_size) {
            const std::uint64_t o = static_cast<std::uint64_t>(raw_ptr) + (rva - va);
            if (o >= b.size()) return std::nullopt;
            return static_cast<std::size_t>(o);
        }
    }
    return std::nullopt;
}

std::string cstr_at(const std::vector<unsigned char>& b, std::size_t off, std::size_t max_len = 512) {
    if (off >= b.size()) return {};
    std::size_t end = off;
    while (end < b.size() && end - off < max_len && b[end] != 0) ++end;
    return std::string(reinterpret_cast<const char*>(b.data()) + off, end - off);
}

}  // namespace

std::optional<Info> parse(std::string_view path) {
    std::vector<unsigned char> b;
    if (!read_file(std::string(path), b)) return std::nullopt;
    if (b.size() < 64 || b[0] != 'M' || b[1] != 'Z') return std::nullopt;
    const std::uint32_t pe = rd32(&b[0x3c]);
    if (pe < 64 || pe + 24 > b.size()) return std::nullopt;
    if (b[pe] != 'P' || b[pe + 1] != 'E' || b[pe + 2] != 0 || b[pe + 3] != 0) return std::nullopt;

    Info info;
    const std::uint16_t machine = rd16(&b[pe + 4]);
    info.machine = machine == 0x14c ? "i386" : machine == 0x8664 ? "amd64" : machine == 0xaa64 ? "arm64" : "unknown";
    const std::uint16_t nsections = rd16(&b[pe + 6]);
    const std::uint16_t opt_size = rd16(&b[pe + 20]);
    if (nsections > kMaxSections || opt_size < 112) return std::nullopt;
    const std::size_t opt = pe + 24;
    if (opt + opt_size > b.size()) return std::nullopt;
    const std::uint16_t magic = rd16(&b[opt]);
    const bool plus = magic == 0x20b;
    if (!plus && magic != 0x10b) return std::nullopt;
    // 数据目录：PE32+ 在可选头 +112，PE32 在 +96
    const std::size_t dirs = opt + (plus ? 112 : 96);
    if (dirs + 16 > b.size()) return std::nullopt;  // 至少要有前两个目录项
    // SizeOfHeaders：PE32+ 在 +60，PE32 在 +56
    const std::uint32_t headers_size = rd32(&b[opt + (plus ? 60 : 56)]);

    // 节表
    const std::size_t sections_off = opt + opt_size;
    for (std::uint16_t i = 0; i < nsections; ++i) {
        const std::size_t off = sections_off + static_cast<std::size_t>(i) * 40;
        if (off + 40 > b.size()) return std::nullopt;
        Section s;
        s.name = cstr_at(b, off, 8);
        s.virtual_size = rd32(&b[off + 8]);
        s.raw_size = rd32(&b[off + 16]);
        const std::uint32_t ch = rd32(&b[off + 36]);
        s.writable = (ch & 0x80000000u) != 0;     // IMAGE_SCN_MEM_WRITE
        s.executable = (ch & 0x20000000u) != 0;   // IMAGE_SCN_MEM_EXECUTE
        info.sections.push_back(std::move(s));
    }

    auto to_off = [&](std::uint32_t rva) { return rva_to_off(b, rva, info.sections, sections_off, headers_size); };

    // 导出表
    const std::uint32_t exp_rva = rd32(&b[dirs + kDirExport * 8]);
    const std::uint32_t exp_size = rd32(&b[dirs + kDirExport * 8 + 4]);
    if (exp_rva && exp_size) {
        const auto eo = to_off(exp_rva);
        if (!eo || *eo + 40 > b.size()) return std::nullopt;
        const std::size_t e = *eo;
        const std::uint32_t ordinal_base = rd32(&b[e + 16]);
        const std::uint32_t n_names = rd32(&b[e + 24]);
        const std::uint32_t eat_rva = rd32(&b[e + 28]);  // address of functions
        const std::uint32_t ent_rva = rd32(&b[e + 32]);  // address of names
        const std::uint32_t eot_rva = rd32(&b[e + 36]);  // address of name ordinals
        if (n_names > kMaxExports) return std::nullopt;
        const auto eat = to_off(eat_rva), ent = to_off(ent_rva), eot = to_off(eot_rva);
        if (!eat || !ent || !eot) return std::nullopt;
        for (std::uint32_t i = 0; i < n_names; ++i) {
            if (*eat + i * 4 + 4 > b.size() || *ent + i * 4 + 4 > b.size() || *eot + i * 2 + 2 > b.size())
                return std::nullopt;
            const std::uint32_t name_rva = rd32(&b[*ent + i * 4]);
            const auto name_off = to_off(name_rva);
            if (!name_off) continue;
            Export x;
            x.name = cstr_at(b, *name_off);
            const std::uint16_t idx = rd16(&b[*eot + i * 2]);
            x.rva = rd32(&b[*eat + idx * 4]);
            x.ordinal = ordinal_base + idx;
            if (!x.name.empty()) info.exports.push_back(std::move(x));
        }
    }

    // 导入表：IMAGE_IMPORT_DESCRIPTOR 数组，全零项结束
    const std::uint32_t imp_rva = rd32(&b[dirs + kDirImport * 8]);
    const std::uint32_t imp_size = rd32(&b[dirs + kDirImport * 8 + 4]);
    if (imp_rva && imp_size) {
        const auto io = to_off(imp_rva);
        if (!io) return std::nullopt;
        const std::size_t thunk_size = plus ? 8 : 4;
        const std::uint64_t ord_flag = plus ? 0x8000000000000000ull : 0x80000000ull;
        for (std::size_t d = 0;; ++d) {
            const std::size_t desc = *io + d * 20;
            if (desc + 20 > b.size()) return std::nullopt;
            const std::uint32_t oft = rd32(&b[desc + 0]);      // original first thunk
            const std::uint32_t name_rva = rd32(&b[desc + 12]);
            const std::uint32_t ft = rd32(&b[desc + 16]);      // first thunk
            if (oft == 0 && name_rva == 0 && ft == 0) break;   // 结束项
            if (info.imports.size() > kMaxImports) return std::nullopt;
            const auto name_off = to_off(name_rva);
            if (!name_off) return std::nullopt;
            const std::string dll = cstr_at(b, *name_off);
            if (dll.empty()) return std::nullopt;
            const std::uint32_t thunk_rva = oft ? oft : ft;
            const auto th = to_off(thunk_rva);
            if (!th) return std::nullopt;
            for (std::size_t i = 0;; ++i) {
                if (*th + i * thunk_size + thunk_size > b.size()) return std::nullopt;
                const std::uint64_t v = plus ? rd64(&b[*th + i * thunk_size]) : rd32(&b[*th + i * thunk_size]);
                if (v == 0) break;  // thunk 数组结束
                if (info.imports.size() >= kMaxImports) return std::nullopt;
                Import im;
                im.dll = dll;
                if (v & ord_flag) {
                    im.by_ordinal = true;
                    im.ordinal = static_cast<std::uint16_t>(v & 0xffff);
                } else {
                    const auto hn = to_off(static_cast<std::uint32_t>(v));
                    if (!hn) return std::nullopt;
                    if (*hn + 2 > b.size()) return std::nullopt;
                    im.name = cstr_at(b, *hn + 2);  // 前两个字节是 hint
                    if (im.name.empty()) return std::nullopt;
                }
                info.imports.push_back(std::move(im));
            }
        }
    }
    return info;
}

}  // namespace mol::pe

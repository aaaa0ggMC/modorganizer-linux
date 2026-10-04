#include "mol/bsa.hpp"

#include <lz4frame.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

namespace mol::bsa {
namespace fs = std::filesystem;
namespace {

[[noreturn]] void bad(const std::string& m) { throw Error("invalid_argument", "bsa: " + m); }

std::string norm_path(std::string p) {
    for (char& c : p) {
        if (c == '/') c = '\\';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (!p.empty() && p.front() == '\\') p.erase(p.begin());
    return p;
}

void put32(std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }
void put64(std::string& s, std::uint64_t v) { for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }
void set32(std::string& s, std::size_t at, std::uint32_t v) { for (int i = 0; i < 4; ++i) s[at + static_cast<std::size_t>(i)] = static_cast<char>((v >> (8 * i)) & 0xFF); }
std::uint32_t get32(const std::string& s, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + static_cast<std::size_t>(i)])) << (8 * i);
    return v;
}
std::uint64_t get64(const std::string& s, std::size_t at) { return get32(s, at) | (static_cast<std::uint64_t>(get32(s, at + 4)) << 32); }

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw Error("io_error", "cannot read file", p.string());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string lz4_compress(const std::string& in) {
    const std::size_t bound = LZ4F_compressFrameBound(in.size(), nullptr);
    std::string out(bound, '\0');
    LZ4F_preferences_t prefs{};
    prefs.frameInfo.contentSize = in.size();
    prefs.compressionLevel = 0;
    const std::size_t n = LZ4F_compressFrame(out.data(), out.size(), in.data(), in.size(), &prefs);
    if (LZ4F_isError(n)) bad(std::string("LZ4 compression failed: ") + LZ4F_getErrorName(n));
    out.resize(n);
    return out;
}

std::string lz4_decompress(const char* src, std::size_t n, std::size_t expect) {
    LZ4F_dctx* ctx = nullptr;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION))) bad("LZ4 init failed");
    std::string out(expect, '\0');
    std::size_t in_pos = 0, out_pos = 0;
    while (in_pos < n && out_pos < expect) {
        std::size_t in_n = n - in_pos, out_n = expect - out_pos;
        const std::size_t r = LZ4F_decompress(ctx, out.data() + out_pos, &out_n, src + in_pos, &in_n, nullptr);
        if (LZ4F_isError(r)) { LZ4F_freeDecompressionContext(ctx); bad(std::string("LZ4 decompression failed: ") + LZ4F_getErrorName(r)); }
        in_pos += in_n;
        out_pos += out_n;
        if (r == 0) break;
    }
    LZ4F_freeDecompressionContext(ctx);
    if (out_pos != expect) bad("decompressed size mismatch");
    return out;
}

}  // namespace

std::uint64_t hash_name(std::string_view name, bool is_folder) {
    std::string root(name), ext;
    if (!is_folder) {
        const auto dot = root.rfind('.');
        if (dot != std::string::npos) { ext = root.substr(dot); root.resize(dot); }
    }
    std::uint32_t lo = 0;
    if (!root.empty()) {
        lo = static_cast<unsigned char>(root.back());
        if (root.size() >= 3) lo |= static_cast<std::uint32_t>(static_cast<unsigned char>(root[root.size() - 2])) << 8;
        lo |= static_cast<std::uint32_t>(root.size()) << 16;
        lo |= static_cast<std::uint32_t>(static_cast<unsigned char>(root.front())) << 24;
    }
    if (ext == ".kf") lo |= 0x80;
    else if (ext == ".nif") lo |= 0x8000;
    else if (ext == ".dds") lo |= 0x8080;
    else if (ext == ".wav") lo |= 0x80000000u;
    std::uint32_t h2 = 0, h3 = 0;
    if (root.size() > 3)
        for (std::size_t i = 1; i + 2 < root.size(); ++i) h2 = h2 * 0x1003f + static_cast<unsigned char>(root[i]);
    for (char c : ext) h3 = h3 * 0x1003f + static_cast<unsigned char>(c);
    return (static_cast<std::uint64_t>(h2 + h3) << 32) | lo;
}

std::uint64_t write(std::string_view out_path, const Header& h, const std::vector<InputFile>& in_files) {
    if (h.version != 104 && h.version != 105) bad("only versions 104 and 105 are supported");
    if (!(h.archive_flags & 0x1) || !(h.archive_flags & 0x2)) bad("archives without directory/file names are not supported");
    struct F { std::string name, folder, source; std::uint64_t hash; bool flip; };
    struct D { std::string name; std::uint64_t hash; std::vector<F> files; };
    std::map<std::string, D> folders;
    for (const auto& f : in_files) {
        const std::string p = norm_path(f.path);
        if (p.empty()) bad("empty path");
        const auto slash = p.rfind('\\');
        const std::string folder = slash == std::string::npos ? "" : p.substr(0, slash);
        const std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
        auto& d = folders[folder];
        d.name = folder;
        d.hash = hash_name(folder, true);
        d.files.push_back({name, folder, f.source, hash_name(name, false), f.flip_compression});
    }
    std::vector<D*> order;
    for (auto& [k, d] : folders) order.push_back(&d);
    std::stable_sort(order.begin(), order.end(), [](const D* a, const D* b) { return a->hash < b->hash; });
    for (D* d : order) std::stable_sort(d->files.begin(), d->files.end(), [](const F& a, const F& b) { return a.hash < b.hash; });

    std::uint32_t file_count = 0, total_folder_names = 0, total_file_names = 0;
    for (const D* d : order) {
        total_folder_names += static_cast<std::uint32_t>(d->name.size() + 1);
        for (const F& f : d->files) { ++file_count; total_file_names += static_cast<std::uint32_t>(f.name.size() + 1); }
    }
    const std::size_t folder_rec = h.version == 105 ? 24 : 16;
    std::string out;
    out.append("BSA\0", 4);
    put32(out, h.version);
    put32(out, 36);
    put32(out, h.archive_flags);
    put32(out, static_cast<std::uint32_t>(order.size()));
    put32(out, file_count);
    put32(out, total_folder_names);
    put32(out, total_file_names);
    out.push_back(static_cast<char>(h.file_flags & 0xFF));
    out.push_back(static_cast<char>(h.file_flags >> 8));
    out.push_back(0);
    out.push_back(0);

    // 文件夹记录先占位（偏移稍后回填）
    const std::size_t folder_table = out.size();
    out.append(folder_rec * order.size(), '\0');
    // 文件记录块
    std::vector<std::size_t> file_rec_pos;  // 每个文件的 16 字节记录位置，顺序同 order×files
    std::vector<std::size_t> folder_block_pos;
    for (const D* d : order) {
        folder_block_pos.push_back(out.size());
        out.push_back(static_cast<char>(d->name.size() + 1));
        out.append(d->name);
        out.push_back('\0');
        for (std::size_t i = 0; i < d->files.size(); ++i) {
            file_rec_pos.push_back(out.size());
            out.append(16, '\0');
        }
    }
    // 文件名块
    for (const D* d : order)
        for (const F& f : d->files) { out.append(f.name); out.push_back('\0'); }

    // 回填文件夹记录
    for (std::size_t i = 0; i < order.size(); ++i) {
        std::string rec;
        put64(rec, order[i]->hash);
        put32(rec, static_cast<std::uint32_t>(order[i]->files.size()));
        if (h.version == 105) {
            put32(rec, 0);
            put32(rec, static_cast<std::uint32_t>(folder_block_pos[i] + total_file_names));
            put32(rec, 0);
        } else {
            put32(rec, static_cast<std::uint32_t>(folder_block_pos[i] + total_file_names));
        }
        std::memcpy(out.data() + folder_table + i * folder_rec, rec.data(), rec.size());
    }

    // 数据
    const fs::path outp{std::string(out_path)};
    std::error_code ec;
    fs::create_directories(outp.parent_path(), ec);
    const fs::path tmp = outp.string() + ".mol-tmp";
    std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
    if (!os) throw Error("io_error", "cannot write", tmp.string());
    // 头部（含记录与名字块）要等到偏移全部确定后才能最终写出：先在内存里算出所有数据块的偏移
    std::uint64_t data_pos = out.size();
    std::size_t rec_i = 0;
    std::vector<std::string> blocks;  // 每个文件的数据块（内存里保存会很大；这里逐个生成、先算大小、写文件时再生成一次）
    struct Plan { std::string source; bool compress; std::string embed; std::uint32_t size; std::uint64_t offset; bool flip; };
    std::vector<Plan> plan;
    for (const D* d : order)
        for (const F& f : d->files) {
            const bool compress = ((h.archive_flags & 0x4) != 0) != f.flip;
            std::string embed;
            if (h.archive_flags & 0x100) {
                const std::string full = f.folder.empty() ? f.name : f.folder + "\\" + f.name;
                embed.push_back(static_cast<char>(full.size()));
                embed += full;
            }
            const std::string data = read_all(f.source);
            std::uint64_t block;
            if (compress) block = embed.size() + 4 + lz4_compress(data).size();
            else block = embed.size() + data.size();
            if (block > 0x3FFFFFFFULL) bad("file too large for a BSA: " + f.source);
            plan.push_back({f.source, compress, embed, static_cast<std::uint32_t>(block), data_pos, f.flip});
            data_pos += block;
            if (data_pos > 0xFFFFFFFFULL) bad("archive larger than 4 GiB is not supported");
        }
    for (const Plan& p : plan) {
        const std::size_t at = file_rec_pos[rec_i];
        (void)at;
        ++rec_i;
    }
    // 回填文件记录：哈希 + 大小(含 flip 位) + 偏移
    {
        std::size_t i = 0;
        for (const D* d : order)
            for (const F& f : d->files) {
                const Plan& p = plan[i];
                std::string rec;
                put64(rec, f.hash);
                put32(rec, p.size | (p.flip ? 0x40000000u : 0u));
                put32(rec, static_cast<std::uint32_t>(p.offset));
                std::memcpy(out.data() + file_rec_pos[i], rec.data(), 16);
                ++i;
            }
    }
    os.write(out.data(), static_cast<std::streamsize>(out.size()));
    for (const Plan& p : plan) {
        const std::string data = read_all(p.source);
        os.write(p.embed.data(), static_cast<std::streamsize>(p.embed.size()));
        if (p.compress) {
            std::string pre;
            put32(pre, static_cast<std::uint32_t>(data.size()));
            os.write(pre.data(), 4);
            const std::string z = lz4_compress(data);
            os.write(z.data(), static_cast<std::streamsize>(z.size()));
        } else {
            os.write(data.data(), static_cast<std::streamsize>(data.size()));
        }
    }
    os.flush();
    if (!os) throw Error("io_error", "write failed", tmp.string());
    os.close();
    fs::rename(tmp, outp, ec);
    if (ec) throw Error("io_error", "rename failed: " + ec.message(), outp.string());
    return data_pos;
}

Index read_index(std::string_view path) {
    std::ifstream in{std::string(path), std::ios::binary};
    if (!in) throw Error("io_error", "cannot open BSA", std::string(path));
    std::string head(36, '\0');
    in.read(head.data(), 36);
    if (in.gcount() != 36 || std::memcmp(head.data(), "BSA\0", 4) != 0) bad("not a BSA file");
    Index idx;
    idx.header.version = get32(head, 4);
    if (idx.header.version != 104 && idx.header.version != 105) bad("unsupported BSA version " + std::to_string(idx.header.version));
    idx.header.archive_flags = get32(head, 12);
    const std::uint32_t folder_count = get32(head, 16), file_count = get32(head, 20), tot_fn = get32(head, 28);
    idx.header.file_flags = static_cast<std::uint16_t>(static_cast<unsigned char>(head[32]) | (static_cast<unsigned char>(head[33]) << 8));
    if (!(idx.header.archive_flags & 0x1) || !(idx.header.archive_flags & 0x2)) bad("the BSA has no directory/file names");
    const std::size_t rec = idx.header.version == 105 ? 24 : 16;
    std::string table(rec * folder_count, '\0');
    in.read(table.data(), static_cast<std::streamsize>(table.size()));
    // 文件记录块紧随其后；一次读完到文件名块结束
    std::string rest;
    {
        const auto here = in.tellg();
        in.seekg(0, std::ios::end);
        const auto end = in.tellg();
        in.seekg(here);
        rest.resize(static_cast<std::size_t>(end - here));
        in.read(rest.data(), static_cast<std::streamsize>(rest.size()));
    }
    const std::size_t base = 36 + table.size();  // rest[0] 的文件偏移
    // 先解析文件夹块，得到每个文件的 (folder, hash, size, offset)
    struct Raw { std::string folder; std::uint64_t hash; std::uint32_t size, offset; };
    std::vector<Raw> raws;
    for (std::uint32_t i = 0; i < folder_count; ++i) {
        const std::uint32_t cnt = get32(table, i * rec + 8);
        const std::uint32_t off = idx.header.version == 105 ? get32(table, i * rec + 16) : get32(table, i * rec + 12);
        if (off < tot_fn || off - tot_fn < base) bad("bad folder offset");
        std::size_t p = off - tot_fn - base;
        if (p >= rest.size()) bad("folder block outside the file");
        const std::size_t nlen = static_cast<unsigned char>(rest[p]);
        std::string fname = rest.substr(p + 1, nlen > 0 ? nlen - 1 : 0);
        p += 1 + nlen;
        for (std::uint32_t k = 0; k < cnt; ++k) {
            if (p + 16 > rest.size()) bad("file record outside the file");
            raws.push_back({fname, get64(rest, p), get32(rest, p + 8), get32(rest, p + 12)});
            p += 16;
        }
    }
    if (raws.size() != file_count) bad("file count mismatch");
    // 文件名块位于所有文件夹块之后：位置 = 最后一个文件夹块末尾（即最后一条文件记录之后）
    std::size_t names_at = 0;
    {
        std::size_t max_end = 0;
        for (std::uint32_t i = 0; i < folder_count; ++i) {
            const std::uint32_t cnt = get32(table, i * rec + 8);
            const std::uint32_t off = idx.header.version == 105 ? get32(table, i * rec + 16) : get32(table, i * rec + 12);
            const std::size_t p = off - tot_fn - base;
            const std::size_t nlen = static_cast<unsigned char>(rest[p]);
            max_end = std::max(max_end, p + 1 + nlen + static_cast<std::size_t>(cnt) * 16);
        }
        names_at = max_end;
    }
    std::size_t np = names_at;
    for (const Raw& r : raws) {
        const auto z = rest.find('\0', np);
        if (z == std::string::npos) bad("truncated file name block");
        Entry e;
        const std::string name = rest.substr(np, z - np);
        e.path = r.folder.empty() ? name : r.folder + "\\" + name;
        e.hash = r.hash;
        e.flip = (r.size & 0x40000000u) != 0;
        e.size = r.size & ~0xC0000000u;
        e.offset = r.offset;
        idx.files.push_back(std::move(e));
        np = z + 1;
    }
    return idx;
}

std::string read_file(std::string_view path, const Index& idx, const Entry& e) {
    std::ifstream in{std::string(path), std::ios::binary};
    if (!in) throw Error("io_error", "cannot open BSA", std::string(path));
    in.seekg(e.offset);
    std::string block(e.size, '\0');
    in.read(block.data(), static_cast<std::streamsize>(block.size()));
    if (static_cast<std::uint32_t>(in.gcount()) != e.size) bad("file data outside the archive");
    std::size_t skip = 0;
    if (idx.header.archive_flags & 0x100) skip = 1 + static_cast<unsigned char>(block[0]);
    const bool compressed = ((idx.header.archive_flags & 0x4) != 0) != e.flip;
    if (!compressed) return block.substr(skip);
    if (block.size() < skip + 4) bad("truncated compressed file");
    const std::uint32_t orig = get32(block, skip);
    return lz4_decompress(block.data() + skip + 4, block.size() - skip - 4, orig);
}

}  // namespace mol::bsa

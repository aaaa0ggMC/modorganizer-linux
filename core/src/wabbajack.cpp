#include "mol/wabbajack.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "mol/casefold.hpp"
#include "mol/http.hpp"
#include "mol/xxh64.hpp"

import alib6;

extern char** environ;

namespace mol::wabbajack {
namespace fs = std::filesystem;
namespace {

[[noreturn]] void bad(const std::string& m) { throw Error("invalid_argument", "wabbajack: " + m); }

std::string S(const alib6::AData& o, const char* k) {
    if (!o.is_object()) return {};
    auto it = o.object().find(k);
    if (it == o.object().end()) return {};
    auto v = it.second().try_to<std::string_view>();
    return v ? std::string(*v) : std::string();
}
std::int64_t I(const alib6::AData& o, const char* k) {
    if (!o.is_object()) return 0;
    auto it = o.object().find(k);
    if (it == o.object().end()) return 0;
    if (auto v = it.second().try_to<long long>()) return *v;
    if (auto s = it.second().try_to<std::string_view>()) {
        std::int64_t r = 0;
        for (char c : *s) { if (c < '0' || c > '9') return 0; r = r * 10 + (c - '0'); }
        return r;
    }
    return 0;
}
bool B(const alib6::AData& o, const char* k) {
    if (!o.is_object()) return false;
    auto it = o.object().find(k);
    if (it == o.object().end()) return false;
    auto v = it.second().try_to<bool>();
    return v && *v;
}
const alib6::AData* sub(const alib6::AData& o, const char* k) {
    if (!o.is_object()) return nullptr;
    auto it = o.object().find(k);
    return it == o.object().end() ? nullptr : &it.second();
}

std::string lower(std::string_view s) { return std::string(casefold(s)); }
bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

Source parse_source(const alib6::AData& st) {
    Source s;
    s.type_name = S(st, "$type");
    const std::string t = s.type_name;
    s.url = S(st, "Url");
    s.prompt = S(st, "Prompt");
    if (has(t, "Nexus")) {
        s.kind = "nexus";
        s.mod_id = I(st, "ModID");
        s.file_id = I(st, "FileID");
        s.game_domain = lower(S(st, "GameName"));
    } else if (has(t, "WabbajackCDN")) {
        s.kind = "cdn";
    } else if (has(t, "GameFileSource")) {
        s.kind = "gamefile";
        s.game = S(st, "Game");
        s.game_file = S(st, "GameFile");
        s.game_version = S(st, "GameVersion");
    } else if (has(t, "Manual")) {
        s.kind = "manual";
    } else if (has(t, "Http")) {
        s.kind = "http";
    } else if (has(t, "Mega")) {
        s.kind = "mega";
    } else if (has(t, "GoogleDrive")) {
        s.kind = "gdrive";
    } else if (has(t, "MediaFire")) {
        s.kind = "mediafire";
    } else if (has(t, "ModDB")) {
        s.kind = "moddb";
    } else if (has(t, "LoversLab")) {
        s.kind = "loverslab";
    } else if (has(t, "VectorPlexus")) {
        s.kind = "vectorplexus";
    } else {
        s.kind = "unknown";
    }
    if (const auto* h = sub(st, "Headers"); h && h->is_array())
        for (const auto& v : h->array()) if (auto x = v.try_to<std::string_view>()) s.headers.emplace_back(*x);
    return s;
}

Kind kind_of_type(const std::string& t) {
    if (t == "FromArchive") return Kind::FromArchive;
    if (t == "PatchedFromArchive") return Kind::PatchedFromArchive;
    if (t == "InlineFile") return Kind::InlineFile;
    if (t == "RemappedInlineFile") return Kind::RemappedInlineFile;
    if (t == "CreateBSA") return Kind::CreateBSA;
    if (t == "TransformedTexture") return Kind::TransformedTexture;
    if (t == "MergedPatch") return Kind::MergedPatch;
    if (t == "IgnoredDirectly" || t == "NoMatch" || t == "IncludedFile") return Kind::Ignored;
    return Kind::Other;
}

// 运行外部程序，捕获 stdout（stderr 丢弃）。失败返回 false。
bool run_capture(const std::vector<std::string>& argv, std::string& out) {
    int pipefd[2];
    if (::pipe(pipefd) != 0) return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    std::vector<char*> av;
    for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, av[0], &fa, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(pipefd[1]);
    if (rc != 0) { ::close(pipefd[0]); return false; }
    char buf[1 << 16];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof buf)) > 0) out.append(buf, static_cast<std::size_t>(n));
    ::close(pipefd[0]);
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0) { if (errno != EINTR) return false; }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

std::string safe_file(const std::string& s) {
    std::string o;
    for (char c : s) o.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    return o;
}

std::string gunzip(std::string_view in) {
    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) throw Error("io_error", "zlib init failed");
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = static_cast<uInt>(in.size());
    std::string out;
    char buf[1 << 15];
    int rc;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof buf;
        rc = inflate(&zs, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&zs); throw Error("invalid_argument", "wabbajack: corrupt gzip data"); }
        out.append(buf, sizeof buf - zs.avail_out);
    } while (rc != Z_STREAM_END);
    inflateEnd(&zs);
    return out;
}

}  // namespace

bool supported(Kind k) { return k == Kind::FromArchive || k == Kind::PatchedFromArchive || k == Kind::InlineFile || k == Kind::RemappedInlineFile || k == Kind::CreateBSA || k == Kind::MergedPatch || k == Kind::TransformedTexture || k == Kind::Ignored; }

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::FromArchive: return "FromArchive";
        case Kind::PatchedFromArchive: return "PatchedFromArchive";
        case Kind::InlineFile: return "InlineFile";
        case Kind::RemappedInlineFile: return "RemappedInlineFile";
        case Kind::CreateBSA: return "CreateBSA";
        case Kind::TransformedTexture: return "TransformedTexture";
        case Kind::MergedPatch: return "MergedPatch";
        case Kind::Ignored: return "Ignored";
        case Kind::Other: return "Other";
    }
    return "Other";
}

std::string game_id_of(std::string_view g) {
    const auto l = lower(g);
    if (l == "skyrimspecialedition") return "skyrimse";
    return l;
}

Modlist parse_modlist(std::string_view json) {
    alib6::AData doc(default_mr());
    if (!doc.load_from_memory(json) || !doc.is_object()) bad("modlist is not a JSON object");
    Modlist m;
    m.name = S(doc, "Name");
    m.author = S(doc, "Author");
    m.description = S(doc, "Description");
    m.version = S(doc, "Version");
    m.game_type = S(doc, "GameType");
    m.nsfw = B(doc, "IsNSFW");
    const auto* ar = sub(doc, "Archives");
    const auto* dr = sub(doc, "Directives");
    if (!ar || !ar->is_array() || !dr || !dr->is_array()) bad("modlist has no Archives/Directives");
    m.archives.reserve(ar->array().size());
    for (const auto& a : ar->array()) {
        Archive x;
        x.hash = S(a, "Hash");
        x.name = S(a, "Name");
        x.meta = S(a, "Meta");
        x.size = I(a, "Size");
        if (const auto* st = sub(a, "State")) x.src = parse_source(*st);
        m.archives.push_back(std::move(x));
    }
    m.directives.reserve(dr->array().size());
    for (const auto& d : dr->array()) {
        Directive x;
        x.type = S(d, "$type");
        x.kind = kind_of_type(x.type);
        if (const auto* p = sub(d, "ArchiveHashPath"); p && p->is_array())
            for (const auto& e : p->array()) if (auto s = e.try_to<std::string_view>()) x.archive_path.emplace_back(*s);
        x.to = S(d, "To");
        x.hash = S(d, "Hash");
        x.from_hash = S(d, "FromHash");
        x.source_data_id = S(d, "SourceDataID");
        x.patch_id = S(d, "PatchID");
        x.size = I(d, "Size");
        if (x.kind == Kind::CreateBSA) {
            x.temp_id = S(d, "TempID");
            if (const auto* st = sub(d, "State")) {
                x.bsa_version = static_cast<std::uint32_t>(I(*st, "Version"));
                x.bsa_flags = static_cast<std::uint32_t>(I(*st, "ArchiveFlags"));
                x.bsa_file_flags = static_cast<std::uint32_t>(I(*st, "FileFlags"));
            }
            if (const auto* fs_ = sub(d, "FileStates"); fs_ && fs_->is_array())
                for (const auto& f : fs_->array()) x.bsa_files.push_back({S(f, "Path"), B(f, "FlipCompression")});
        }
        if (x.kind == Kind::MergedPatch) {
            x.patch_id = S(d, "PatchID");
            if (const auto* src = sub(d, "Sources"); src && src->is_array())
                for (const auto& e : src->array()) x.merge_sources.push_back({S(e, "RelativePath"), S(e, "Hash")});
        }
        if (x.kind == Kind::TransformedTexture) {
            if (const auto* im = sub(d, "ImageState")) {
                x.image.width = I(*im, "Width");
                x.image.height = I(*im, "Height");
                x.image.mips = I(*im, "MipLevels");
                x.image.format = S(*im, "Format");
            }
        }
        m.directives.push_back(std::move(x));
    }
    return m;
}

std::string read_modlist_json(std::string_view file) {
    std::error_code ec;
    if (!fs::is_regular_file(std::string(file), ec)) throw Error("invalid_argument", "wabbajack file not found", std::string(file));
    std::string out;
    if (!run_capture({"7z", "e", "-so", "-y", "-bd", "-bso0", "-bsp0", std::string(file), "modlist"}, out) || out.empty()) {
        out.clear();
        if (!run_capture({"7zz", "e", "-so", "-y", "-bd", "-bso0", "-bsp0", std::string(file), "modlist"}, out) || out.empty())
            throw Error("invalid_argument", "cannot read the 'modlist' entry (not a .wabbajack file, or 7z is missing)", std::string(file));
    }
    return out;
}

namespace {
std::string cache_root(std::string_view given) {
    if (!given.empty()) return std::string(given);
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::string(x) + "/mo-linux/gallery";
    const char* h = std::getenv("HOME");
    return std::string(h ? h : "/tmp") + "/.cache/mo-linux/gallery";
}

// 取一个 URL 的文本，带缓存：新鲜缓存直接用；联网失败退回旧缓存。
std::string cached_get(const std::string& url, const fs::path& file) {
    std::error_code ec;
    constexpr auto ttl = std::chrono::hours(6);
    if (fs::is_regular_file(file, ec)) {
        const auto age = fs::file_time_type::clock::now() - fs::last_write_time(file, ec);
        if (!ec && age < ttl) {
            std::ifstream in(file, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
    }
    try {
        HttpResponse r = http_get(url, std::vector<std::pair<std::string, std::string>>{{"User-Agent", "Wabbajack/4.0"}}, 20);
        if (r.status < 200 || r.status >= 300) throw Error("network_error", "HTTP " + std::to_string(r.status) + " from " + url);
        fs::create_directories(file.parent_path(), ec);
        const fs::path tmp = file.string() + ".tmp";
        { std::ofstream os(tmp, std::ios::binary | std::ios::trunc); os << std::string(r.body); }
        fs::rename(tmp, file, ec);
        return std::string(r.body);
    } catch (const Error&) {
        if (fs::is_regular_file(file, ec)) {  // 旧缓存也比没有强
            std::ifstream in(file, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
        throw;
    }
}
}  // namespace

std::vector<GalleryEntry> fetch_gallery(std::string_view game_domain, std::string_view cache_dir) {
    const fs::path cache{cache_root(cache_dir)};
    alib6::AData repos(default_mr());
    if (!repos.load_from_memory(cached_get("https://raw.githubusercontent.com/wabbajack-tools/mod-lists/master/repositories.json", cache / "repositories.json")) || !repos.is_object())
        bad("cannot read the repository list");
    std::vector<std::pair<std::string, std::string>> repo_urls;
    for (const auto& [name, v] : repos.object())
        if (auto url = v.try_to<std::string_view>()) repo_urls.emplace_back(std::string(name), std::string(*url));

    std::vector<std::string> bodies(repo_urls.size());
    {
        std::vector<std::thread> pool;
        for (std::size_t i = 0; i < repo_urls.size(); ++i)
            pool.emplace_back([&, i] {
                try { bodies[i] = cached_get(repo_urls[i].second, cache / ("repo-" + safe_file(repo_urls[i].first) + ".json")); } catch (const Error&) {}
            });
        for (auto& t : pool) t.join();
    }
    std::vector<GalleryEntry> out;
    for (std::size_t i = 0; i < repo_urls.size(); ++i) {
        if (bodies[i].empty()) continue;
        alib6::AData list(default_mr());
        if (!list.load_from_memory(bodies[i]) || !list.is_array()) continue;
        for (const auto& m : list.array()) {
            GalleryEntry e;
            e.game = S(m, "game");
            if (!game_domain.empty() && lower(e.game) != lower(game_domain)) continue;
            e.title = S(m, "title");
            e.author = S(m, "author");
            e.description = S(m, "description");
            e.version = S(m, "version");
            e.repository = repo_urls[i].first;
            e.nsfw = B(m, "nsfw");
            e.unavailable = B(m, "force_down");
            if (const auto* l = sub(m, "links")) { e.download_url = S(*l, "download"); e.machine_url = S(*l, "machineURL"); }
            if (const auto* d = sub(m, "download_metadata")) {
                e.download_size = I(*d, "Size");
                e.archives_size = I(*d, "SizeOfArchives");
                e.installed_size = I(*d, "SizeOfInstalledFiles");
                e.archive_count = I(*d, "NumberOfArchives");
            }
            if (e.download_url.empty()) continue;
            out.push_back(std::move(e));
        }
    }
    return out;
}

void download_authored(std::string_view url_in, std::string_view dest, const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
    const std::string url(url_in);
    const std::vector<std::pair<std::string, std::string>> hdrs = {{"User-Agent", "Wabbajack/4.0"}};
    HttpResponse r = http_get(url + "/definition.json.gz", hdrs, 60);
    if (r.status < 200 || r.status >= 300) throw Error("network_error", "cannot fetch the file definition (HTTP " + std::to_string(r.status) + ")", url);
    alib6::AData def(default_mr());
    if (!def.load_from_memory(gunzip(r.body)) || !def.is_object()) bad("bad file definition");
    const std::int64_t total = I(def, "Size");
    const std::string want_hash = S(def, "Hash");
    const auto* parts = sub(def, "Parts");
    if (!parts || !parts->is_array() || parts->array().empty()) bad("file definition has no parts");
    std::vector<std::pair<std::int64_t, std::int64_t>> order;  // (offset, index)
    for (const auto& p : parts->array()) order.emplace_back(I(p, "Offset"), I(p, "Index"));
    std::sort(order.begin(), order.end());

    const fs::path d{std::string(dest)};
    std::error_code ec;
    fs::create_directories(d.parent_path(), ec);
    const fs::path tmp = d.string() + ".part";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw Error("io_error", "cannot write file", tmp.string());
        std::uint64_t done = 0;
        for (const auto& [off, idx] : order) {
            const std::string piece = d.string() + ".piece";
            const std::uint64_t base = done;
            http_download(url + "/parts/" + std::to_string(idx), piece, hdrs, [&](std::uint64_t n, std::uint64_t) {
                return !progress || progress(base + n, static_cast<std::uint64_t>(total));
            });
            std::ifstream in(piece, std::ios::binary);
            out << in.rdbuf();
            done += fs::file_size(piece, ec);
            fs::remove(piece, ec);
        }
        out.flush();
        if (!out) throw Error("io_error", "write failed", tmp.string());
    }
    if (!want_hash.empty() && wj_file_hash(tmp.string()) != want_hash) {
        fs::remove(tmp, ec);
        throw Error("network_error", "hash mismatch after downloading the .wabbajack file (deleted; try again)", d.string());
    }
    fs::rename(tmp, d, ec);
    if (ec) throw Error("io_error", "rename failed: " + ec.message(), d.string());
}

void octodiff_apply(std::string_view basis_path, std::string_view delta_path, std::string_view out_path) {
    std::ifstream delta{std::string(delta_path), std::ios::binary};
    std::ifstream basis{std::string(basis_path), std::ios::binary};
    if (!delta) throw Error("io_error", "cannot open patch", std::string(delta_path));
    if (!basis) throw Error("io_error", "cannot open basis file", std::string(basis_path));
    auto rd = [&](void* p, std::size_t n) {
        delta.read(static_cast<char*>(p), static_cast<std::streamsize>(n));
        if (static_cast<std::size_t>(delta.gcount()) != n) bad("truncated OctoDiff patch");
    };
    char magic[9];
    rd(magic, 9);
    if (std::memcmp(magic, "OCTODELTA", 9) != 0) bad("not an OctoDiff patch");
    unsigned char ver;
    rd(&ver, 1);
    if (ver != 1) bad("unsupported OctoDiff version");
    // 哈希算法名：.NET BinaryWriter 的 7 位变长长度前缀字符串
    auto read7 = [&]() {
        std::uint32_t v = 0;
        int shift = 0;
        for (;;) {
            unsigned char b;
            rd(&b, 1);
            v |= static_cast<std::uint32_t>(b & 0x7F) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
            if (shift > 28) bad("bad length prefix");
        }
        return v;
    };
    const std::uint32_t algo_len = read7();
    if (algo_len > 64) bad("bad hash algorithm name");
    std::string algo(algo_len, '\0');
    if (algo_len) rd(algo.data(), algo_len);
    std::int32_t hash_len;
    rd(&hash_len, 4);
    if (hash_len < 0 || hash_len > 1024) bad("bad hash length");
    std::string hash(static_cast<std::size_t>(hash_len), '\0');
    if (hash_len) rd(hash.data(), static_cast<std::size_t>(hash_len));
    char end[3];
    rd(end, 3);
    if (std::memcmp(end, ">>>", 3) != 0) bad("bad OctoDiff metadata terminator");

    const fs::path out_p{std::string(out_path)};
    std::error_code ec;
    fs::create_directories(out_p.parent_path(), ec);
    const fs::path tmp = out_p.string() + ".mol-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw Error("io_error", "cannot write", tmp.string());
        std::string buf;
        for (;;) {
            unsigned char cmd;
            delta.read(reinterpret_cast<char*>(&cmd), 1);
            if (delta.gcount() == 0) break;
            if (cmd == 0x60) {  // copy from basis
                std::int64_t off, len;
                rd(&off, 8);
                rd(&len, 8);
                if (off < 0 || len < 0) bad("negative copy range");
                basis.clear();
                basis.seekg(off);
                std::int64_t left = len;
                buf.resize(1 << 16);
                while (left > 0) {
                    const auto want = static_cast<std::streamsize>(std::min<std::int64_t>(left, static_cast<std::int64_t>(buf.size())));
                    basis.read(buf.data(), want);
                    if (basis.gcount() != want) bad("copy range outside the basis file");
                    out.write(buf.data(), want);
                    left -= want;
                }
            } else if (cmd == 0x80) {  // literal data
                std::int64_t len;
                rd(&len, 8);
                if (len < 0) bad("negative data length");
                std::int64_t left = len;
                buf.resize(1 << 16);
                while (left > 0) {
                    const auto want = static_cast<std::size_t>(std::min<std::int64_t>(left, static_cast<std::int64_t>(buf.size())));
                    rd(buf.data(), want);
                    out.write(buf.data(), static_cast<std::streamsize>(want));
                    left -= static_cast<std::int64_t>(want);
                }
            } else {
                bad("unknown OctoDiff command");
            }
        }
        out.flush();
        if (!out) throw Error("io_error", "write failed", tmp.string());
    }
    fs::rename(tmp, out_p, ec);
    if (ec) throw Error("io_error", "rename failed: " + ec.message(), out_p.string());
}

}  // namespace mol::wabbajack

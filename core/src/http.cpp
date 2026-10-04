#include "mol/http.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <filesystem>
#include <mutex>

namespace mol {
namespace fs = std::filesystem;
namespace {

void global_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// 把 URL 里不合法的字符（空格、非 ASCII、引号等）做百分号编码；已有的 %XX 与保留字符原样保留。
// Nexus 的 CDN 地址里会直接带空格（如 "I'm Talkin Here-93694.7z"），libcurl 会拒绝。
std::string sanitize_url(std::string_view u) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    o.reserve(u.size() + 16);
    for (unsigned char c : u) {
        const bool bad = c <= 0x20 || c >= 0x7F || c == '"' || c == '<' || c == '>' || c == '\\' || c == '^' || c == '`' || c == '{' || c == '|' || c == '}';
        if (!bad) { o.push_back(static_cast<char>(c)); continue; }
        o.push_back('%');
        o.push_back(hex[c >> 4]);
        o.push_back(hex[c & 15]);
    }
    return o;
}

struct Easy {
    CURL* h;
    curl_slist* hdrs = nullptr;
    Easy() : h(curl_easy_init()) {
        if (!h) throw Error("network_error", "curl_easy_init failed");
    }
    ~Easy() {
        if (hdrs) curl_slist_free_all(hdrs);
        curl_easy_cleanup(h);
    }
    Easy(const Easy&) = delete;
    Easy& operator=(const Easy&) = delete;
    void setup(std::string_view url, HttpHeaders headers, long timeout) {
        const std::string u = sanitize_url(url);
        curl_easy_setopt(h, CURLOPT_URL, u.c_str());
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(h, CURLOPT_MAXREDIRS, 8L);
        curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 15L);
        if (timeout > 0) curl_easy_setopt(h, CURLOPT_TIMEOUT, timeout);
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(h, CURLOPT_USERAGENT, "mo-linux");
        for (const auto& [k, v] : headers) hdrs = curl_slist_append(hdrs, (k + ": " + v).c_str());
        if (hdrs) curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
    }
};

size_t write_body(char* p, size_t sz, size_t n, void* ud) {
    static_cast<string*>(ud)->append(p, sz * n);
    return sz * n;
}
size_t read_header(char* p, size_t sz, size_t n, void* ud) {
    const std::string_view line(p, sz * n);
    constexpr std::string_view key = "retry-after:";
    if (line.size() > key.size()) {
        bool match = true;
        for (size_t i = 0; i < key.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(line[i])) != key[i]) { match = false; break; }
        if (match) {
            std::string_view v = line.substr(key.size());
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
            while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.remove_suffix(1);
            static_cast<string*>(ud)->assign(v);
        }
    }
    return sz * n;
}

[[noreturn]] void net_fail(CURLcode rc, std::string_view url) {
    throw Error("network_error", std::string("request failed: ") + curl_easy_strerror(rc), std::string(url));
}

struct DlCtx {
    std::FILE* f;
    const std::function<bool(std::uint64_t, std::uint64_t)>* progress;
    std::uint64_t base;  // 续传起点
    bool aborted = false;
};
size_t write_file(char* p, size_t sz, size_t n, void* ud) {
    auto* c = static_cast<DlCtx*>(ud);
    return std::fwrite(p, sz, n, c->f);
}
int xfer(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* c = static_cast<DlCtx*>(ud);
    if (c->progress && *c->progress) {
        const std::uint64_t total = dltotal > 0 ? c->base + static_cast<std::uint64_t>(dltotal) : 0;
        if (!(*c->progress)(c->base + static_cast<std::uint64_t>(dlnow), total)) {
            c->aborted = true;
            return 1;
        }
    }
    return 0;
}
}  // namespace

HttpResponse http_get(std::string_view url, HttpHeaders headers, long timeout_sec, mr* mem) {
    global_init();
    Easy e;
    e.setup(url, headers, timeout_sec);
    HttpResponse r(mem);
    curl_easy_setopt(e.h, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(e.h, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(e.h, CURLOPT_HEADERFUNCTION, read_header);
    curl_easy_setopt(e.h, CURLOPT_HEADERDATA, &r.retry_after);
    const CURLcode rc = curl_easy_perform(e.h);
    if (rc != CURLE_OK) net_fail(rc, url);
    curl_easy_getinfo(e.h, CURLINFO_RESPONSE_CODE, &r.status);
    return r;
}

HttpResponse http_post(std::string_view url, std::string_view body, HttpHeaders headers, long timeout_sec, mr* mem) {
    global_init();
    Easy e;
    e.setup(url, headers, timeout_sec);
    const std::string payload(body);
    curl_easy_setopt(e.h, CURLOPT_POST, 1L);
    curl_easy_setopt(e.h, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(e.h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(payload.size()));
    HttpResponse r(mem);
    curl_easy_setopt(e.h, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(e.h, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(e.h, CURLOPT_HEADERFUNCTION, read_header);
    curl_easy_setopt(e.h, CURLOPT_HEADERDATA, &r.retry_after);
    const CURLcode rc = curl_easy_perform(e.h);
    if (rc != CURLE_OK) net_fail(rc, url);
    curl_easy_getinfo(e.h, CURLINFO_RESPONSE_CODE, &r.status);
    return r;
}

std::uint64_t http_download(std::string_view url, std::string_view dest, HttpHeaders headers,
                            const std::function<bool(std::uint64_t, std::uint64_t)>& progress) {
    global_init();
    const fs::path d{std::string(dest)};
    const fs::path part = fs::path(d.string() + ".part");
    std::error_code ec;
    fs::create_directories(d.parent_path(), ec);
    std::uint64_t have = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;

    for (int attempt = 0; attempt < 2; ++attempt) {
        Easy e;
        e.setup(url, headers, 0);
        std::FILE* f = std::fopen(part.c_str(), have > 0 ? "ab" : "wb");
        if (!f) throw Error("io_error", "cannot open for writing", part.string());
        DlCtx ctx{f, &progress, have};
        curl_easy_setopt(e.h, CURLOPT_WRITEFUNCTION, write_file);
        curl_easy_setopt(e.h, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(e.h, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(e.h, CURLOPT_XFERINFOFUNCTION, xfer);
        curl_easy_setopt(e.h, CURLOPT_XFERINFODATA, &ctx);
        curl_easy_setopt(e.h, CURLOPT_LOW_SPEED_LIMIT, 1024L);  // 30 秒内低于 1KB/s 视为卡死
        curl_easy_setopt(e.h, CURLOPT_LOW_SPEED_TIME, 30L);
        if (have > 0) curl_easy_setopt(e.h, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(have));
        const CURLcode rc = curl_easy_perform(e.h);
        std::fclose(f);
        long status = 0;
        curl_easy_getinfo(e.h, CURLINFO_RESPONSE_CODE, &status);
        if (ctx.aborted) throw Error("io_error", "aborted", part.string());
        if (status == 416 && have > 0) {  // 续传位置越界：.part 作废，重来
            fs::remove(part, ec);
            have = 0;
            continue;
        }
        if (rc == CURLE_RANGE_ERROR && have > 0) {  // 服务器不支持 Range
            fs::remove(part, ec);
            have = 0;
            continue;
        }
        if (rc != CURLE_OK) net_fail(rc, url);
        if (status >= 400) {
            fs::remove(part, ec);
            throw Error("network_error", "download failed with HTTP " + std::to_string(status), std::string(url));
        }
        // 服务器忽略了 Range（200 而非 206）：curl 已把整文件追加在旧内容后，必须从头重来
        if (have > 0 && status == 200) {
            fs::remove(part, ec);
            have = 0;
            continue;
        }
        fs::rename(part, d, ec);
        if (ec) throw Error("io_error", "rename failed: " + ec.message(), d.string());
        return fs::file_size(d, ec);
    }
    throw Error("network_error", "download failed after retry", std::string(url));
}

}  // namespace mol

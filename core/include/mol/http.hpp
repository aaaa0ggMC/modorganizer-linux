#pragma once
// 最小 HTTP 客户端（libcurl）：GET 取文本、下载到文件（可续传、带进度）。尊重 http(s)_proxy 等环境变量。
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "mol/error.hpp"
#include "mol/pmr.hpp"

namespace mol {

using HttpHeaders = std::span<const std::pair<std::string, std::string>>;

struct HttpResponse {
    long status = 0;
    string body;
    string retry_after;  // Retry-After 头（可空）
    explicit HttpResponse(mr* mem = default_mr()) : body(mem), retry_after(mem) {}
};

// 传输层失败（DNS/连接/超时/TLS）→ Error{network_error}；HTTP 状态码不在此判断，交给调用方。
HttpResponse http_get(std::string_view url, HttpHeaders headers = {}, long timeout_sec = 30, mr* mem = default_mr());

// POST（Content-Type 由 headers 给出）。语义同 http_get。
HttpResponse http_post(std::string_view url, std::string_view body, HttpHeaders headers = {}, long timeout_sec = 30, mr* mem = default_mr());

// 下载 url → dest。先写 dest + ".part"，完成后原子 rename；已有 .part 则续传（服务器不支持 Range 时从头开始）。
// progress(done, total)：total 未知为 0；返回 false 中止（→ Error{io_error, "aborted"}，.part 保留以便续传）。
// HTTP 状态 >= 400 → Error{network_error}（message 含状态码）。返回最终文件大小。
std::uint64_t http_download(std::string_view url, std::string_view dest, HttpHeaders headers = {},
                            const std::function<bool(std::uint64_t, std::uint64_t)>& progress = {});

}  // namespace mol

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <thread>

#include "minitest.hpp"
#include "mock_http.hpp"
#include "mol/http.hpp"
#include "mol/nexus.hpp"

namespace fs = std::filesystem;
using namespace mol;
using namespace mockhttp;

namespace {

std::string code_of(const std::function<void()>& f) {
    try { f(); } catch (const Error& e) { return e.code; }
    return "<no error>";
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}
}  // namespace

TEST(parse_nxm_ok_and_errors) {
    const auto n = parse_nxm("nxm://skyrimspecialedition/mods/30379/files/12345?key=ab%2Bc&expires=1700000000&user_id=9");
    CHECK_EQ(std::string(n.game), std::string("skyrimspecialedition"));
    CHECK_EQ(n.mod_id, 30379);
    CHECK_EQ(n.file_id, 12345);
    CHECK_EQ(std::string(n.key), std::string("ab+c"));
    CHECK_EQ(std::string(n.expires), std::string("1700000000"));
    CHECK_EQ(code_of([] { parse_nxm("http://x/y"); }), std::string("invalid_argument"));
    CHECK_EQ(code_of([] { parse_nxm("nxm://g/mods/x/files/1"); }), std::string("invalid_argument"));
    CHECK_EQ(code_of([] { parse_nxm("nxm://g/mods/1"); }), std::string("invalid_argument"));
    CHECK_EQ(std::string(nexus_game_domain("skyrimse")), std::string("skyrimspecialedition"));
}

TEST(validate_and_files_send_the_key_and_parse) {
    Mock srv([](const Req& r) -> Resp {
        if (r.target == "/users/validate.json")
            return {200, R"({"user_id":42,"name":"tester","is_premium":true,"is_supporter":false})", ""};
        if (r.target == "/games/skyrimspecialedition/mods/30379/files.json")
            return {200, R"({"files":[{"file_id":7,"name":"SKSE64","file_name":"skse64_2_2_6.7z","version":"2.2.6","category_name":"main","size_kb":4096,"is_primary":true}],"file_updates":[]})", ""};
        return {404, "{}", ""};
    });
    NexusClient c("SECRET-KEY", "9.9", srv.base());
    const auto u = c.validate();
    CHECK_EQ(std::string(u.name), std::string("tester"));
    CHECK(u.is_premium);
    CHECK(!u.is_supporter);
    CHECK_EQ(u.user_id, 42);
    CHECK_EQ(srv.seen.back().h["apikey"], std::string("SECRET-KEY"));
    CHECK_EQ(srv.seen.back().h["application-name"], std::string("mo-linux"));
    CHECK_EQ(srv.seen.back().h["application-version"], std::string("9.9"));
    const auto files = c.mod_files("skyrimspecialedition", 30379);
    CHECK_EQ(files.size(), std::size_t{1});
    CHECK_EQ(files[0].file_id, 7);
    CHECK_EQ(std::string(files[0].file_name), std::string("skse64_2_2_6.7z"));
    CHECK_EQ(files[0].size_kb, 4096);
    CHECK(files[0].is_primary);
}

TEST(http_errors_map_to_codes_without_leaking_the_key) {
    Mock srv([](const Req& r) -> Resp {
        if (r.target.rfind("/a", 0) == 0) return {401, "{}", ""};
        if (r.target.rfind("/b", 0) == 0) return {403, "{}", ""};
        if (r.target.rfind("/c", 0) == 0) return {429, "{}", "Retry-After: 17\r\n"};
        if (r.target.rfind("/d", 0) == 0) return {500, "oops", ""};
        return {404, "{}", ""};
    });
    NexusClient c("SECRET-KEY", "1", srv.base());
    CHECK_EQ(code_of([&] { c.get_json("/a"); }), std::string("nexus_auth"));
    CHECK_EQ(code_of([&] { c.get_json("/b"); }), std::string("nexus_premium"));
    CHECK_EQ(code_of([&] { c.get_json("/c"); }), std::string("nexus_rate_limited"));
    CHECK_EQ(code_of([&] { c.get_json("/d"); }), std::string("network_error"));
    CHECK_EQ(code_of([&] { c.get_json("/e"); }), std::string("nexus_not_found"));
    try { c.get_json("/c?key=NXMKEY"); } catch (const Error& e) {
        const std::string all = std::string(e.what()) + e.path;
        CHECK(all.find("SECRET-KEY") == std::string::npos);
        CHECK(all.find("NXMKEY") == std::string::npos);
        CHECK(all.find("17") != std::string::npos);
    }
}

TEST(download_url_uses_nxm_key_for_free_users) {
    Mock srv([](const Req& r) -> Resp {
        const bool has_key = r.target.find("key=K%2B1") != std::string::npos && r.target.find("expires=99") != std::string::npos;
        if (r.target.rfind("/games/skyrimspecialedition/mods/1/files/2/download_link.json", 0) == 0) {
            if (!has_key) return {403, "{}", ""};
            return {200, R"([{"name":"CDN","short_name":"cdn","URI":"http://cdn.example/f.7z"},{"name":"B","short_name":"b","URI":"http://b.example/f.7z"}])", ""};
        }
        return {404, "{}", ""};
    });
    NexusClient c("K", "1", srv.base());
    CHECK_EQ(code_of([&] { c.download_url("skyrimspecialedition", 1, 2); }), std::string("nexus_premium"));
    NxmUrl n;
    n.key = string("K+1");
    n.expires = string("99");
    CHECK_EQ(std::string(c.download_url("skyrimspecialedition", 1, 2, &n)), std::string("http://cdn.example/f.7z"));
}

TEST(http_download_resumes_and_handles_servers_without_range) {
    std::string payload(5000, 'a');
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>('a' + i % 23);
    std::atomic<bool> support_range{true};
    Mock srv([&](const Req& r) -> Resp {
        auto it = r.h.find("range");
        if (it != r.h.end() && support_range) {
            const size_t from = std::stoul(it->second.substr(it->second.find('=') + 1));
            return {206, payload.substr(from), "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(payload.size() - 1) + "/" + std::to_string(payload.size()) + "\r\n"};
        }
        return {200, payload, ""};
    });
    const fs::path dir = fs::temp_directory_path() / ("mol_dl_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    const std::string dest = (dir / "f.bin").string();
    std::uint64_t last_done = 0;
    CHECK_EQ(http_download(srv.base() + "/f", dest, {}, [&](std::uint64_t d, std::uint64_t) { last_done = d; return true; }), std::uint64_t{5000});
    CHECK_EQ(slurp(dest), payload);
    CHECK(!fs::exists(dest + ".part"));
    CHECK_EQ(last_done, std::uint64_t{5000});

    // 续传：.part 里已有前 2000 字节
    fs::remove(dest);
    { std::ofstream(dest + ".part", std::ios::binary) << payload.substr(0, 2000); }
    CHECK_EQ(http_download(srv.base() + "/f", dest), std::uint64_t{5000});
    CHECK_EQ(slurp(dest), payload);

    // 服务器忽略 Range（总是 200）：必须从头重来，不能拼出错位文件
    fs::remove(dest);
    support_range = false;
    { std::ofstream(dest + ".part", std::ios::binary) << payload.substr(0, 2000); }
    CHECK_EQ(http_download(srv.base() + "/f", dest), std::uint64_t{5000});
    CHECK_EQ(slurp(dest), payload);

    // 中止：.part 保留
    fs::remove(dest);
    CHECK_EQ(code_of([&] { http_download(srv.base() + "/f", dest, {}, [](std::uint64_t, std::uint64_t) { return false; }); }), std::string("io_error"));
    fs::remove_all(dir);
}

TEST(transport_failure_is_network_error) {
    CHECK_EQ(code_of([] { http_get("http://127.0.0.1:1/x", {}, 5); }), std::string("network_error"));
}

TEST(key_store_roundtrip_with_0600_and_env_priority) {
    const fs::path home = fs::temp_directory_path() / ("mol_key_" + std::to_string(::getpid()));
    fs::remove_all(home);
    fs::create_directories(home);
    ::setenv("XDG_CONFIG_HOME", home.c_str(), 1);
    ::unsetenv("NEXUS_API_KEY");
    CHECK(!load_nexus_key().has_value());
    save_nexus_key("  abc123 \n");
    struct stat st{};
    CHECK_EQ(::stat((home / "mo-linux/nexus.key").c_str(), &st), 0);
    CHECK_EQ(static_cast<int>(st.st_mode & 0777), 0600);
    CHECK_EQ(std::string(*load_nexus_key()), std::string("abc123"));
    ::setenv("NEXUS_API_KEY", "fromenv", 1);
    CHECK_EQ(std::string(*load_nexus_key()), std::string("fromenv"));
    ::unsetenv("NEXUS_API_KEY");
    CHECK(remove_nexus_key());
    CHECK(!remove_nexus_key());
    CHECK_EQ(code_of([] { save_nexus_key("   "); }), std::string("invalid_argument"));
    CHECK_EQ(code_of([] { NexusClient c(""); }), std::string("nexus_auth"));
    ::unsetenv("XDG_CONFIG_HOME");
    fs::remove_all(home);
}

TEST(urls_with_spaces_and_quotes_are_sanitized) {
    std::string seen;
    Mock srv([&](const Req& r) -> Resp { seen = r.target; return {200, "ok", ""}; });
    const auto r = http_get(srv.base() + "/cdn/I'm Talkin Here-1.7z?expires=1&md5=a%2Bb", {}, 10);
    CHECK_EQ(r.status, 200L);
    CHECK_EQ(seen, std::string("/cdn/I'm%20Talkin%20Here-1.7z?expires=1&md5=a%2Bb"));  // 空格被编码，已有的 %2B 不被二次编码
}

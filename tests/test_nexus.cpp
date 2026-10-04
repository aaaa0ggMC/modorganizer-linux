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

TEST(graphql_search_info_and_collections) {
    Mock srv([](const Req& r) -> Resp {
        if (r.target != "/v2/graphql" || r.method != "POST") return {404, "{}", ""};
        CHECK_EQ(r.h.at("apikey"), std::string("K"));
        if (r.body.find("game(domainName") != std::string::npos) return {200, R"({"data":{"game":{"id":1704}}})", ""};
        if (r.body.find("mods(filter") != std::string::npos)
            return {200, R"({"data":{"mods":{"totalCount":"2","nodes":[
              {"modId":12604,"name":"SkyUI","author":"Team","endorsements":5,"downloads":"99","summary":"ui","version":"6.9","updatedAt":"2026-01-01T00:00:00Z"},
              {"modId":2,"name":"Other","author":"x","endorsements":1,"downloads":2,"summary":"","version":"1","updatedAt":""}]}}})", ""};
        if (r.body.find("mod(modId") != std::string::npos)
            return {200, R"({"data":{"mod":{"modId":"22825","name":"Wider MCM","author":"u","endorsements":3,"downloads":4,"summary":"s","version":"1.2","updatedAt":"",
              "modCategory":{"name":"User Interface"},
              "modRequirements":{"nexusRequirements":{"nodes":[{"modId":"12604","modName":"SkyUI","url":"","externalRequirement":false,"notes":""},
                                                              {"modId":"0","modName":"Some Tool","url":"https://example.org","externalRequirement":true,"notes":"n"}]},
                                 "dlcRequirements":[{"gameExpansion":{"name":"Dawnguard"}}]}}}})", ""};
        if (r.body.find("collectionsV2") != std::string::npos)
            return {200, R"({"data":{"collectionsV2":{"totalCount":7,"nodes":[{"slug":"abc","name":"Coll","summary":"sum","endorsements":9,"totalDownloads":100,
              "latestPublishedRevision":{"revisionNumber":3,"modCount":60,"totalSize":"454"}}]}}})", ""};
        return {200, R"({"errors":[{"message":"unexpected query"}]})", ""};
    });
    NexusClient c("K", "1", srv.base());
    std::int64_t total = 0;
    const auto mods = c.search_mods("skyrimspecialedition", "sky ui", "endorsements", 5, 0, &total);
    CHECK_EQ(total, std::int64_t{2});
    CHECK_EQ(mods.size(), std::size_t{2});
    CHECK_EQ(mods[0].mod_id, 12604);
    CHECK_EQ(mods[0].downloads, 99);  // 字符串形式的整数也能读
    CHECK_EQ(std::string(mods[0].name), std::string("SkyUI"));
    CHECK(srv.seen.back().body.find("nameStemmed:[{value:\\\"sky ui\\\"}]") != std::string::npos);  // 文本被正确转义进查询
    CHECK(srv.seen.back().body.find("endorsements:{direction:DESC}") != std::string::npos);
    CHECK_EQ(code_of([&] { c.search_mods("skyrimspecialedition", "x", "bogus", 5, 0); }), std::string("invalid_argument"));

    const auto info = c.mod_info("skyrimspecialedition", 22825);
    CHECK_EQ(info.summary.mod_id, 22825);
    CHECK_EQ(std::string(info.category), std::string("User Interface"));
    CHECK_EQ(info.requirements.size(), std::size_t{2});
    CHECK_EQ(info.requirements[0].mod_id, 12604);
    CHECK(!info.requirements[0].external);
    CHECK(info.requirements[1].external);
    CHECK_EQ(std::string(info.requirements[1].url), std::string("https://example.org"));
    CHECK_EQ(info.dlc_requirements.size(), std::size_t{1});
    CHECK_EQ(std::string(info.dlc_requirements[0]), std::string("Dawnguard"));

    const auto cols = c.search_collections("skyrimspecialedition", "coll", "endorsements", 5, 0, &total);
    CHECK_EQ(total, std::int64_t{7});
    CHECK_EQ(cols.size(), std::size_t{1});
    CHECK_EQ(std::string(cols[0].slug), std::string("abc"));
    CHECK_EQ(cols[0].mod_count, 60);
    CHECK_EQ(cols[0].total_size, 454);

    // GraphQL 的 errors 字段被转成异常
    CHECK_EQ(code_of([&] { c.graphql("{nothing}"); }), std::string("network_error"));
}

TEST(mod_summaries_batches_and_isolates_deleted_mods) {
    std::atomic<int> calls{0};
    Mock srv([&](const Req& r) -> Resp {
        if (r.body.find("game(domainName") != std::string::npos) return {200, R"({"data":{"game":{"id":1704}}})", ""};
        ++calls;
        if (r.body.find("modId:\\\"999\\\"") != std::string::npos) return {200, R"({"errors":[{"message":"Mod not found"}],"data":null})", ""};
        // 为请求里出现的每个 modId 回一个对象
        std::string out = R"({"data":{)";
        bool first = true;
        for (std::size_t pos = 0; (pos = r.body.find("modId:\\\"", pos)) != std::string::npos; pos += 8) {
            const std::size_t b = pos + 8, e = r.body.find('\\', b);
            const std::string id = r.body.substr(b, e - b);
            if (!first) out += ",";
            first = false;
            out += "\"m" + id + "\":{\"modId\":" + id + ",\"name\":\"M" + id + "\",\"version\":\"v" + id + "\"}";
        }
        out += "}}";
        return {200, out, ""};
    });
    NexusClient c("K", "1", srv.base());
    const std::vector<std::int64_t> ids{1, 2, 999, 3};
    const auto m = c.mod_summaries("skyrimspecialedition", ids);
    CHECK_EQ(m.size(), std::size_t{3});               // 999 被隔离跳过，其余都拿到
    CHECK_EQ(std::string(m.at(1).version), std::string("v1"));
    CHECK_EQ(std::string(m.at(3).version), std::string("v3"));
    CHECK(m.count(999) == 0);
    CHECK(calls.load() > 1);                          // 发生过二分重试
    CHECK(c.mod_summaries("skyrimspecialedition", std::span<const std::int64_t>{}).empty());
}

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "minitest.hpp"
#include "mock_http.hpp"
#include "mol/collection.hpp"
#include "mol/md5.hpp"

namespace fs = std::filesystem;
using namespace mol;
using namespace mol::collection;
using namespace mockhttp;

namespace {

struct Tmp {
    fs::path dir;
    Tmp() {
        dir = fs::temp_directory_path() / ("mol_col_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Tmp() { std::error_code ec; fs::remove_all(dir, ec); }
};
void put(const fs::path& p, const char* body = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}
bool have_zip() { return std::system("command -v zip >/dev/null 2>&1") == 0; }
void zip_dir(const fs::path& src, const fs::path& out) {
    const std::string cmd = "cd '" + src.string() + "' && zip -qr '" + out.string() + "' . >/dev/null 2>&1";
    (void)!std::system(cmd.c_str());
}
std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

// 真实集合清单的缩减版（字段与 Vortex 的 ICollection 一致）
const char* kSample = R"({
 "info":{"name":"Sample","author":"me","domainName":"skyrimspecialedition","gameVersions":["1.7.104.0"],"installInstructions":"hi"},
 "mods":[
  {"name":"Base","version":"1","optional":false,"domainName":"skyrimspecialedition","phase":0,
   "source":{"type":"nexus","modId":10,"fileId":100,"md5":"aa","fileSize":5,"logicalFilename":"Base","tag":"tagA"}},
  {"name":"Patch","version":"1","optional":false,"phase":0,
   "source":{"type":"nexus","modId":11,"fileId":110,"md5":"bb","logicalFilename":"PatchFile","tag":"tagB"}},
  {"name":"Fomod","version":"1","optional":false,"phase":0,
   "source":{"type":"nexus","modId":12,"fileId":120,"tag":"tagC"},
   "choices":{"type":"fomod","options":[{"name":"S","groups":[{"name":"G","choices":[{"name":"A","idx":0}]}]}]}},
  {"name":"Late","version":"1","optional":true,"phase":1,"source":{"type":"manual","instructions":"get it yourself","tag":"tagD"}}
 ],
 "modRules":[
  {"type":"after","source":{"logicalFileName":"Base","fileMD5":"aa"},"reference":{"logicalFileName":"PatchFile","fileMD5":"bb"}},
  {"type":"conflicts","source":{"logicalFileName":"x"},"reference":{"logicalFileName":"y"}}
 ],
 "plugins":[{"name":"Patch.esp","enabled":true}]
})";
}  // namespace

TEST(parse_sample_collection) {
    const Collection c = parse_collection(kSample);
    CHECK_EQ(c.info.name, std::string("Sample"));
    CHECK_EQ(c.info.game_versions.size(), std::size_t{1});
    CHECK_EQ(c.mods.size(), std::size_t{4});
    CHECK_EQ(c.mods[0].source.mod_id, 10);
    CHECK_EQ(c.mods[0].key(), std::string("tagA"));
    CHECK(c.mods[2].has_choices);
    CHECK(c.mods[2].choices.at("S").at("G").count("A") == 1);
    CHECK(c.mods[3].optional);
    CHECK_EQ(c.rules.size(), std::size_t{2});
    CHECK(c.has_plugins);
    bool threw = false;
    try { parse_collection("[]"); } catch (const Error& e) { threw = e.code == "invalid_argument"; }
    CHECK(threw);
}

TEST(install_order_honours_phase_and_after_rules) {
    const Collection c = parse_collection(kSample);
    const auto o = install_order(c);
    // 规则：Base 必须在 Patch 之后（after）→ Patch(1) 先于 Base(0)；Late 是 phase 1，排在最后
    std::vector<std::string> names;
    for (auto i : o) names.push_back(c.mods[i].name);
    CHECK_EQ(names.size(), std::size_t{4});
    auto pos = [&](const char* n) { return std::find(names.begin(), names.end(), std::string(n)) - names.begin(); };
    CHECK(pos("Patch") < pos("Base"));
    CHECK_EQ(names.back(), std::string("Late"));
    // 成环：互相 after → 不死循环，且保留全部 mod
    Collection cyc = c;
    Rule r;
    r.type = "after";
    r.source.logical_name = "PatchFile";
    r.source.md5 = "bb";
    r.reference.logical_name = "Base";
    r.reference.md5 = "aa";
    cyc.rules.push_back(r);
    CHECK_EQ(install_order(cyc).size(), std::size_t{4});
}

TEST(vortex_choices_to_choices) {
    const auto c = choices_from_vortex(R"({"type":"fomod","options":[{"name":"S","groups":[{"name":"G","choices":[{"name":"A","idx":0},{"name":"B","idx":1}]}]}]})");
    CHECK_EQ(c.at("S").at("G").size(), std::size_t{2});
    CHECK(choices_from_vortex(R"({"type":"other","options":[]})").empty());
    CHECK(choices_from_vortex("not json").empty());
}

TEST(pipeline_incomplete_then_resolve_then_resume) {
    if (!have_zip()) return;
    Tmp t;
    // ---- 造三个压缩包 ----
    put(t.dir / "src/base/meshes/base.nif", "BASE!");
    zip_dir(t.dir / "src/base", t.dir / "base.zip");
    put(t.dir / "src/patch/textures/p.dds", "PATCH");
    zip_dir(t.dir / "src/patch", t.dir / "patch.zip");
    fs::create_directories(t.dir / "src/fomod/fomod");
    std::ofstream(t.dir / "src/fomod/fomod/ModuleConfig.xml") << R"(<config><installSteps><installStep name="S"><optionalFileGroups><group name="G" type="SelectExactlyOne"><plugins order="Explicit">
<plugin name="A"><files><file source="a.txt" destination="chosen.txt"/></files><typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
<plugin name="B"><files><file source="b.txt" destination="chosen.txt"/></files><typeDescriptor><type name="Optional"/></typeDescriptor></plugin>
</plugins></group></optionalFileGroups></installStep></installSteps></config>)";
    put(t.dir / "src/fomod/a.txt", "A");
    put(t.dir / "src/fomod/b.txt", "B");
    zip_dir(t.dir / "src/fomod", t.dir / "fomod.zip");
    const std::string base_md5 = md5_file((t.dir / "base.zip").string()), patch_md5 = md5_file((t.dir / "patch.zip").string());
    const auto base_sz = fs::file_size(t.dir / "base.zip"), patch_sz = fs::file_size(t.dir / "patch.zip"), fomod_sz = fs::file_size(t.dir / "fomod.zip");

    // ---- 清单（Base/Patch 有 md5，Fomod 没给选择）----
    const std::string json = std::string(R"({"info":{"name":"Pipe","domainName":"skyrimspecialedition","gameVersions":["1.7.104.0"]},"mods":[
 {"name":"Base","version":"1","optional":false,"source":{"type":"nexus","modId":10,"fileId":100,"md5":")") + base_md5 + R"(","fileSize":)" + std::to_string(base_sz) + R"(,"logicalFilename":"Base","tag":"tagA"}},
 {"name":"Patch","version":"1","optional":false,"source":{"type":"nexus","modId":11,"fileId":110,"md5":")" + patch_md5 + R"(","fileSize":)" + std::to_string(patch_sz) + R"(,"logicalFilename":"Patch","tag":"tagB"}},
 {"name":"Fomod","version":"1","optional":false,"source":{"type":"nexus","modId":12,"fileId":120,"tag":"tagC"}}
 ],"modRules":[],"plugins":[]})";
    const Collection coll = parse_collection(json);

    // ---- mock Nexus：Base 可下载；Patch 的下载链接返回 403（免费账号）；Fomod 可下载 ----
    std::atomic<int> base_downloads{0};
    std::string port_base;
    Mock srv([&](const Req& r) -> Resp {
        auto file_entry = [&](int id, const char* name, std::uint64_t sz) {
            return std::string("{\"file_id\":") + std::to_string(id) + ",\"name\":\"" + name + "\",\"file_name\":\"" + name + "\",\"version\":\"1\",\"category_name\":\"main\",\"size_kb\":" +
                   std::to_string((sz + 1023) / 1024) + ",\"is_primary\":true}";
        };
        if (r.target == "/games/skyrimspecialedition/mods/10/files.json") return {200, "{\"files\":[" + file_entry(100, "base.zip", base_sz) + "]}", ""};
        if (r.target == "/games/skyrimspecialedition/mods/11/files.json") return {200, "{\"files\":[" + file_entry(110, "patch.zip", patch_sz) + "]}", ""};
        if (r.target == "/games/skyrimspecialedition/mods/12/files.json") return {200, "{\"files\":[" + file_entry(120, "fomod.zip", fomod_sz) + "]}", ""};
        if (r.target.rfind("/games/skyrimspecialedition/mods/10/files/100/download_link.json", 0) == 0) return {200, "[{\"name\":\"c\",\"short_name\":\"c\",\"URI\":\"http://" + port_base + "/dl/base.zip\"}]", ""};
        if (r.target.rfind("/games/skyrimspecialedition/mods/12/files/120/download_link.json", 0) == 0) return {200, "[{\"name\":\"c\",\"short_name\":\"c\",\"URI\":\"http://" + port_base + "/dl/fomod.zip\"}]", ""};
        if (r.target.rfind("/games/skyrimspecialedition/mods/11/files/110/download_link.json", 0) == 0) return {403, "{}", ""};
        return {404, "{}", ""};
    });
    // 第二个服务器充当 CDN：按文件名返回 t.dir 下的压缩包字节
    Mock srv2([&](const Req& r) -> Resp {
        const std::string name = r.target.substr(r.target.rfind('/') + 1);
        if (r.target.rfind("/dl/", 0) == 0) { if (name == "base.zip") ++base_downloads; return {200, slurp(t.dir / name), ""}; }
        return {404, "{}", ""};
    });
    port_base = srv2.base().substr(7);

    Instance inst;
    inst.root.assign((t.dir / "inst").string());
    inst.mods_dir.assign((t.dir / "inst/mods").string());
    inst.profiles_dir.assign((t.dir / "inst/profiles").string());
    inst.downloads_dir.assign((t.dir / "inst/downloads").string());
    inst.overwrite_dir.assign((t.dir / "inst/overwrite").string());
    inst.cfg.game.assign("skyrimse");
    inst.cfg.profile.assign("Default");
    fs::create_directories(t.dir / "inst/profiles/Default");
    put(t.dir / "game/Data/Skyrim.esm");
    inst.cfg.game_dir.assign((t.dir / "game").string());

    NexusClient client("K", "1", srv.base());
    State st;
    st.slug = "pipe";
    InstallOptions opt;
    opt.profile = "Default";

    // ---- 第一轮：Base 装好；Patch 因 403 → manual_download；Fomod → fomod_choices ----
    Report r1 = install_collection(inst, &client, coll, st, opt, "1.7.104.0");
    CHECK(!r1.complete());
    CHECK_EQ(r1.installed, std::size_t{1});
    CHECK_EQ(r1.pending.size(), std::size_t{2});
    std::map<std::string, std::string> kinds;
    for (const auto& p : r1.pending) kinds[p.key] = p.kind;
    CHECK_EQ(kinds["tagB"], std::string("manual_download"));
    CHECK_EQ(kinds["tagC"], std::string("fomod_choices"));
    CHECK(!r1.pending[0].url.empty());
    CHECK(fs::exists(t.dir / "inst/mods/Base/meshes/base.nif"));
    CHECK(fs::exists(t.dir / "inst/collections/pipe/state.json"));
    CHECK_EQ(st.mods["tagA"].status, std::string("installed"));
    CHECK_EQ(st.mods["tagB"].status, std::string("pending"));

    // ---- 用户的决定：把 Patch 的压缩包放进 downloads/（按 md5 被识别）；Fomod 用默认选择 ----
    fs::copy_file(t.dir / "patch.zip", t.dir / "inst/downloads/whatever-name.zip");
    st.overrides["tagC"].fomod_defaults = true;
    save_state(inst, st);

    // ---- 第二轮（从磁盘重新加载状态，离线也行）：全部完成，Base 不会被重新下载 ----
    State st2 = load_state(inst, "pipe");
    CHECK_EQ(st2.mods["tagA"].status, std::string("installed"));
    CHECK(st2.overrides["tagC"].fomod_defaults);
    const int before = base_downloads.load();
    Report r2 = install_collection(inst, nullptr, coll, st2, opt, "1.7.104.0");  // client=null：不联网
    CHECK(r2.complete());
    CHECK_EQ(r2.installed, std::size_t{3});
    CHECK_EQ(base_downloads.load(), before);
    CHECK(fs::exists(t.dir / "inst/mods/Patch/textures/p.dds"));
    CHECK_EQ(slurp(t.dir / "inst/mods/Fomod/chosen.txt"), std::string("A"));  // 默认 = 第一个

    // ---- 优先级：modlist 里按安装顺序排列（后装的更高）----
    const auto mods = list_mods(inst, "Default");
    CHECK_EQ(mods.size(), std::size_t{3});

    // ---- 第三轮：幂等，什么都不再做 ----
    Report r3 = install_collection(inst, nullptr, coll, st2, opt, "1.7.104.0");
    CHECK(r3.complete());
    CHECK_EQ(r3.installed, std::size_t{3});
}

TEST(md5_mismatch_deletes_the_bad_download_and_fails) {
    if (!have_zip()) return;
    Tmp t;
    put(t.dir / "src/m/meshes/a.nif", "A");
    zip_dir(t.dir / "src/m", t.dir / "m.zip");
    const auto sz = fs::file_size(t.dir / "m.zip");
    const std::string json = std::string(R"({"info":{"name":"X","domainName":"skyrimspecialedition"},"mods":[{"name":"M","version":"1","optional":false,"source":{"type":"nexus","modId":1,"fileId":2,"md5":"00000000000000000000000000000000","fileSize":)") +
                             std::to_string(sz) + R"(,"tag":"t1"}}],"modRules":[]})";
    const Collection coll = parse_collection(json);
    std::string port;
    Mock srv([&](const Req& r) -> Resp {
        if (r.target == "/games/skyrimspecialedition/mods/1/files.json")
            return {200, "{\"files\":[{\"file_id\":2,\"name\":\"m\",\"file_name\":\"m.zip\",\"version\":\"1\",\"category_name\":\"main\",\"size_kb\":1,\"is_primary\":true}]}", ""};
        if (r.target.rfind("/games/skyrimspecialedition/mods/1/files/2/download_link.json", 0) == 0)
            return {200, "[{\"name\":\"c\",\"short_name\":\"c\",\"URI\":\"http://" + port + "/dl/m.zip\"}]", ""};
        if (r.target == "/dl/m.zip") return {200, slurp(t.dir / "m.zip"), ""};
        return {404, "{}", ""};
    });
    port = srv.base().substr(7);
    Instance inst;
    inst.root.assign((t.dir / "inst").string());
    inst.mods_dir.assign((t.dir / "inst/mods").string());
    inst.profiles_dir.assign((t.dir / "inst/profiles").string());
    inst.downloads_dir.assign((t.dir / "inst/downloads").string());
    inst.overwrite_dir.assign((t.dir / "inst/overwrite").string());
    inst.cfg.game.assign("skyrimse");
    inst.cfg.profile.assign("Default");
    fs::create_directories(t.dir / "inst/profiles/Default");
    put(t.dir / "game/Data/Skyrim.esm");
    inst.cfg.game_dir.assign((t.dir / "game").string());
    NexusClient client("K", "1", srv.base());
    State st;
    st.slug = "bad";
    Report r = install_collection(inst, &client, coll, st, {}, {});
    CHECK(!r.complete());
    CHECK_EQ(r.failed, std::size_t{1});
    CHECK(st.mods["t1"].note.find("md5 mismatch") != std::string::npos);
    CHECK(!fs::exists(t.dir / "inst/downloads/m.zip"));  // 坏文件已删除，下次重新下载
    CHECK(!fs::exists(t.dir / "inst/mods/M"));
}

TEST(downloads_run_in_parallel_and_installs_stay_ordered) {
    if (!have_zip()) return;
    Tmp t;
    constexpr int N = 6;
    std::string mods_json;
    for (int i = 0; i < N; ++i) {
        put(t.dir / ("src/m" + std::to_string(i) + "/meshes/f.nif"), ("M" + std::to_string(i)).c_str());
        zip_dir(t.dir / ("src/m" + std::to_string(i)), t.dir / ("m" + std::to_string(i) + ".zip"));
        if (i) mods_json += ",";
        mods_json += R"({"name":"Mod)" + std::to_string(i) + R"(","version":"1","optional":false,"source":{"type":"nexus","modId":)" + std::to_string(100 + i) + R"(,"fileId":)" +
                     std::to_string(200 + i) + R"(,"tag":"t)" + std::to_string(i) + R"("}})";
    }
    const Collection coll = parse_collection(std::string(R"({"info":{"name":"P","domainName":"skyrimspecialedition"},"mods":[)") + mods_json + R"(],"modRules":[]})");

    std::atomic<int> active{0}, peak{0};
    std::string cdn_port;
    Mock api([&](const Req& r) -> Resp {
        for (int i = 0; i < N; ++i) {
            const std::string id = std::to_string(100 + i), fid = std::to_string(200 + i), name = "m" + std::to_string(i) + ".zip";
            if (r.target == "/games/skyrimspecialedition/mods/" + id + "/files.json")
                return {200, "{\"files\":[{\"file_id\":" + fid + ",\"name\":\"" + name + "\",\"file_name\":\"" + name + "\",\"version\":\"1\",\"category_name\":\"main\",\"size_kb\":1,\"is_primary\":true}]}", ""};
            if (r.target.rfind("/games/skyrimspecialedition/mods/" + id + "/files/" + fid + "/download_link.json", 0) == 0)
                return {200, "[{\"name\":\"c\",\"short_name\":\"c\",\"URI\":\"http://" + cdn_port + "/dl/" + name + "\"}]", ""};
        }
        return {404, "{}", ""};
    }, true);
    Mock cdn([&](const Req& r) -> Resp {
        const int now = ++active;
        int p = peak.load();
        while (now > p && !peak.compare_exchange_weak(p, now)) {}
        ::usleep(300000);  // 每个下载 300ms
        --active;
        return {200, slurp(t.dir / r.target.substr(r.target.rfind('/') + 1)), ""};
    }, true);
    cdn_port = cdn.base().substr(7);

    Instance inst;
    inst.root.assign((t.dir / "inst").string());
    inst.mods_dir.assign((t.dir / "inst/mods").string());
    inst.profiles_dir.assign((t.dir / "inst/profiles").string());
    inst.downloads_dir.assign((t.dir / "inst/downloads").string());
    inst.overwrite_dir.assign((t.dir / "inst/overwrite").string());
    inst.cfg.game.assign("skyrimse");
    inst.cfg.profile.assign("Default");
    fs::create_directories(t.dir / "inst/profiles/Default");
    put(t.dir / "game/Data/Skyrim.esm");
    inst.cfg.game_dir.assign((t.dir / "game").string());
    NexusClient client("K", "1", api.base());
    State st;
    st.slug = "par";
    InstallOptions opt;
    opt.profile = "Default";
    opt.jobs = 4;
    std::uint64_t last_total = 0;
    opt.progress = [&](std::string_view stage, std::string_view, std::uint64_t d, std::uint64_t) { if (stage == "download") last_total = d; };
    const auto t0 = std::chrono::steady_clock::now();
    const Report r = install_collection(inst, &client, coll, st, opt, {});
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.complete());
    CHECK_EQ(r.installed, std::size_t{static_cast<std::size_t>(N)});
    CHECK(peak.load() >= 3);   // 确实并发了
    CHECK(ms < 1500);          // 串行至少 6×300ms = 1.8s
    (void)last_total;
    // 安装顺序仍是清单顺序：modlist 里后面的优先级更高
    const auto mods = list_mods(inst, "Default");
    CHECK_EQ(mods.size(), std::size_t{static_cast<std::size_t>(N)});
    for (int i = 0; i < N; ++i) CHECK_EQ(std::string(mods[static_cast<std::size_t>(i)].name), "Mod" + std::to_string(i));
}

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "minitest.hpp"
#include "mock_http.hpp"
#include "mol/bsa.hpp"
#include "mol/dds.hpp"
#include "mol/wabbajack.hpp"
#include "mol/wabbajack_install.hpp"
#include "mol/xxh64.hpp"

namespace fs = std::filesystem;
using namespace mol;
using namespace mol::wabbajack;
using namespace mockhttp;

namespace {
struct Tmp {
    fs::path dir;
    Tmp() {
        dir = fs::temp_directory_path() / ("mol_wj_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~Tmp() { std::error_code ec; fs::remove_all(dir, ec); }
};
bool have_tools() { return std::system("command -v zip >/dev/null 2>&1 && command -v 7z >/dev/null 2>&1") == 0; }
void put(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << body;
}
std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}
std::string H(std::string_view data) { return wj_hash_string(xxh64(data)); }

// 手工构造 OctoDiff：META + 复制/追加命令
std::string octo(const std::vector<std::pair<char, std::string>>& cmds) {
    std::string s = "OCTODELTA";
    s.push_back(1);
    s.push_back(4);
    s += "SHA1";
    s += std::string("\x14\x00\x00\x00", 4) + std::string(20, 'h');
    s += ">>>";
    auto le64 = [&](std::int64_t v) { for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); };
    for (const auto& [t, arg] : cmds) {
        if (t == 'c') {  // 复制：arg = "offset,length"
            const auto comma = arg.find(',');
            s.push_back(0x60);
            le64(std::stoll(arg.substr(0, comma)));
            le64(std::stoll(arg.substr(comma + 1)));
        } else {
            s.push_back(static_cast<char>(0x80));
            le64(static_cast<std::int64_t>(arg.size()));
            s += arg;
        }
    }
    return s;
}
std::string code_of(const std::function<void()>& f) {
    try { f(); } catch (const Error& e) { return e.code; }
    return "<no error>";
}
}  // namespace

TEST(parse_modlist_sources_and_directives) {
    const std::string json = R"({"Name":"L","Author":"a","Version":"1.0","GameType":"SkyrimSpecialEdition","IsNSFW":false,"Description":"d",
 "Archives":[
  {"Hash":"h1","Name":"a.7z","Size":10,"State":{"$type":"HttpDownloader, Wabbajack.Lib","Url":"http://x/a.7z","Headers":["Referer: http://y"]}},
  {"Hash":"h2","Name":"b.7z","Size":20,"State":{"$type":"NexusDownloader, Wabbajack.Lib","ModID":5,"FileID":6,"GameName":"SkyrimSpecialEdition"}},
  {"Hash":"h3","Name":"c.zip","Size":30,"State":{"$type":"Wabbajack.DTOs.DownloadStates.GameFileSource, Wabbajack.DTOs","Game":"SkyrimSpecialEdition","GameFile":"Data\\Skyrim.esm"}},
  {"Hash":"h4","Name":"d.7z","Size":40,"State":{"$type":"MegaDownloader, Wabbajack.Lib","Url":"https://mega.nz/x"}}],
 "Directives":[
  {"$type":"FromArchive","ArchiveHashPath":["h1","dir\\f.txt"],"To":"mods\\M\\f.txt","Hash":"x","Size":3},
  {"$type":"InlineFile","SourceDataID":"g1","To":"a.txt","Hash":"y","Size":1},
  {"$type":"SomeFutureDirective","To":"mods\\M\\x.bsa"}]})";
    const Modlist m = parse_modlist(json);
    CHECK_EQ(m.name, std::string("L"));
    CHECK_EQ(m.archives.size(), std::size_t{4});
    CHECK_EQ(m.archives[0].src.kind, std::string("http"));
    CHECK_EQ(m.archives[0].src.headers.size(), std::size_t{1});
    CHECK_EQ(m.archives[1].src.kind, std::string("nexus"));
    CHECK_EQ(m.archives[1].src.mod_id, 5);
    CHECK_EQ(m.archives[1].src.game_domain, std::string("skyrimspecialedition"));
    CHECK_EQ(m.archives[2].src.kind, std::string("gamefile"));
    CHECK_EQ(m.archives[3].src.kind, std::string("mega"));
    CHECK(m.directives[0].kind == Kind::FromArchive);
    CHECK_EQ(m.directives[0].archive_path.size(), std::size_t{2});
    CHECK(supported(m.directives[0].kind));
    CHECK(!supported(m.directives[2].kind));
    CHECK_EQ(game_id_of(m.game_type), std::string("skyrimse"));
    CHECK_EQ(code_of([] { parse_modlist("{}"); }), std::string("invalid_argument"));
}

TEST(remap_placeholders_all_forms) {
    const std::string t = "{--||GAME_PATH_MAGIC_BACK||--} {--||GAME_PATH_MAGIC_DOUBLE_BACK||--} {--||GAME_PATH_MAGIC_FORWARD||--} {--||MO2_PATH_MAGIC_BACK||--} {--||DOWNLOAD_PATH_MAGIC_FORWARD||--}";
    const auto r = remap_placeholders(t, "/g/Skyrim", "/i", "/i/dl");
    CHECK_EQ(r, std::string("Z:\\g\\Skyrim Z:\\\\g\\\\Skyrim Z:/g/Skyrim Z:\\i Z:/i/dl"));
}

TEST(octodiff_applies_copy_and_data_commands) {
    Tmp t;
    put(t.dir / "basis", "0123456789");
    put(t.dir / "delta", octo({{'c', "2,4"}, {'d', "XY"}, {'c', "0,3"}}));
    octodiff_apply((t.dir / "basis").string(), (t.dir / "delta").string(), (t.dir / "out").string());
    CHECK_EQ(slurp(t.dir / "out"), std::string("2345XY012"));
    put(t.dir / "bad", "not a patch at all");
    CHECK_EQ(code_of([&] { octodiff_apply((t.dir / "basis").string(), (t.dir / "bad").string(), (t.dir / "o2").string()); }), std::string("invalid_argument"));
    put(t.dir / "oob", octo({{'c', "8,100"}}));
    CHECK_EQ(code_of([&] { octodiff_apply((t.dir / "basis").string(), (t.dir / "oob").string(), (t.dir / "o3").string()); }), std::string("invalid_argument"));
}

TEST(install_modlist_end_to_end_with_resume_and_pending) {
    if (!have_tools()) return;
    Tmp t;
    // 一个来自 HTTP 的压缩包，里面有两个文件（其中一个有嵌套目录）
    put(t.dir / "src/pack/dir/file.txt", "FILE");
    put(t.dir / "src/pack/other.bin", "0123456789");
    (void)!std::system(("cd '" + (t.dir / "src/pack").string() + "' && zip -qr '" + (t.dir / "pack.zip").string() + "' . >/dev/null 2>&1").c_str());
    const std::string pack_hash = wj_file_hash((t.dir / "pack.zip").string());
    const auto pack_size = fs::file_size(t.dir / "pack.zip");

    // .wabbajack：modlist + 内联数据 + 补丁
    const std::string patched_expect = "2345XY012";
    const std::string patch_bytes = octo({{'c', "2,4"}, {'d', "XY"}, {'c', "0,3"}});
    put(t.dir / "wj/inline1", "INLINE");
    put(t.dir / "wj/remap1", "path={--||GAME_PATH_MAGIC_FORWARD||--}");
    put(t.dir / "wj/patch1", patch_bytes);
    const std::string json = std::string(R"({"Name":"T","Author":"a","Version":"1","GameType":"SkyrimSpecialEdition","Description":"","IsNSFW":false,
 "Archives":[
  {"Hash":")") + pack_hash + R"(","Name":"pack.zip","Size":)" + std::to_string(pack_size) + R"(,"State":{"$type":"HttpDownloader, Wabbajack.Lib","Url":"@URL@/pack.zip","Headers":[]}},
  {"Hash":"AAAAAAAAAAA=","Name":"manual.7z","Size":5,"State":{"$type":"ManualDownloader, Wabbajack.Lib","Url":"https://example.org/m","Prompt":"get it"}}],
 "Directives":[
  {"$type":"FromArchive","ArchiveHashPath":[")" + pack_hash + R"(","dir\\file.txt"],"To":"mods\\M\\sub\\file.txt","Hash":")" + H("FILE") + R"(","Size":4},
  {"$type":"PatchedFromArchive","ArchiveHashPath":[")" + pack_hash + R"(","other.bin"],"To":"mods\\M\\patched.bin","PatchID":"patch1","FromHash":"x","Hash":")" + H(patched_expect) + R"(","Size":9},
  {"$type":"InlineFile","SourceDataID":"inline1","To":"readme.txt","Hash":")" + H("INLINE") + R"(","Size":6},
  {"$type":"RemappedInlineFile","SourceDataID":"remap1","To":"ModOrganizer.ini","Hash":"x","Size":1},
  {"$type":"FromArchive","ArchiveHashPath":["AAAAAAAAAAA=","x.txt"],"To":"mods\\N\\x.txt","Hash":"x","Size":1},
  {"$type":"SomeFutureDirective","To":"mods\\M\\x.dds"},
  {"$type":"SomeFutureDirective","To":"mods\\M\\y.dds"}]})";
    // modlist 里的 @URL@ 在启动 mock 之后替换
    std::atomic<int> hits{0};
    Mock srv([&](const Req& r) -> Resp {
        if (r.target == "/pack.zip") { ++hits; return {200, slurp(t.dir / "pack.zip"), ""}; }
        return {404, "", ""};
    });
    std::string full = json;
    full.replace(full.find("@URL@"), 5, srv.base());
    put(t.dir / "wj/modlist", full);
    (void)!std::system(("cd '" + (t.dir / "wj").string() + "' && zip -qr '" + (t.dir / "t.wabbajack").string() + "' . >/dev/null 2>&1").c_str());

    const Modlist ml = parse_modlist(read_modlist_json((t.dir / "t.wabbajack").string()));
    CHECK_EQ(ml.directives.size(), std::size_t{7});
    InstallOptions opt;
    opt.output_dir = (t.dir / "out").string();
    opt.game_dir = "/games/Skyrim";
    const Report r1 = install_modlist(ml, (t.dir / "t.wabbajack").string(), opt);
    CHECK_EQ(slurp(t.dir / "out/mods/M/sub/file.txt"), std::string("FILE"));
    CHECK_EQ(slurp(t.dir / "out/mods/M/patched.bin"), patched_expect);
    CHECK_EQ(slurp(t.dir / "out/readme.txt"), std::string("INLINE"));
    CHECK_EQ(slurp(t.dir / "out/ModOrganizer.ini"), std::string("path=Z:/games/Skyrim"));
    CHECK_EQ(hits.load(), 1);
    CHECK(!r1.complete());
    CHECK_EQ(r1.archives_done, std::int64_t{1});  // pack 完成；manual 还没
    std::map<std::string, Pending> by;
    for (const auto& p : r1.pending) by[p.kind] = p;
    CHECK(by.count("manual_download") == 1);
    CHECK(by["manual_download"].name.find("manual.7z") != std::string::npos);
    CHECK_EQ(by["unsupported"].count, std::int64_t{2});
    CHECK(r1.failures.empty());

    // 用户把手动下载的文件放进 downloads/ → 续跑：pack 不会再下载，manual 被找到并装好
    put(t.dir / "manualsrc/x.txt", "X");
    // 清单里 manual.7z 的 hash 是占位值，真实文件的 hash 对不上 → 这应当被报告为 hash 不符而不是悄悄接受
    (void)!std::system(("cd '" + (t.dir / "manualsrc").string() + "' && zip -qr '" + (t.dir / "out/downloads/manual.7z").string() + "' . >/dev/null 2>&1").c_str());
    const Report r2 = install_modlist(ml, (t.dir / "t.wabbajack").string(), opt);
    CHECK_EQ(hits.load(), 1);  // 已完成的 pack 没有重下
    CHECK(!r2.failures.empty() || !r2.pending.empty());
    bool mismatch_or_pending = false;
    for (const auto& f : r2.failures) if (f.find("manual.7z") != std::string::npos) mismatch_or_pending = true;
    for (const auto& p : r2.pending) if (p.kind == "manual_download") mismatch_or_pending = true;
    CHECK(mismatch_or_pending);
    CHECK(!fs::exists(t.dir / "out/mods/N/x.txt"));  // 校验不过的压缩包不会被用
}

TEST(install_modlist_hash_mismatch_download_is_deleted_and_reported) {
    if (!have_tools()) return;
    Tmp t;
    put(t.dir / "src/p/a.txt", "A");
    (void)!std::system(("cd '" + (t.dir / "src/p").string() + "' && zip -qr '" + (t.dir / "p.zip").string() + "' . >/dev/null 2>&1").c_str());
    Mock srv([&](const Req& r) -> Resp { return r.target == "/p.zip" ? Resp{200, slurp(t.dir / "p.zip"), ""} : Resp{404, "", ""}; });
    const std::string json = std::string(R"({"Name":"T","GameType":"SkyrimSpecialEdition","Archives":[{"Hash":"AAAAAAAAAAA=","Name":"p.zip","Size":)") + std::to_string(fs::file_size(t.dir / "p.zip")) +
                             R"(,"State":{"$type":"HttpDownloader, Wabbajack.Lib","Url":")" + srv.base() + R"(/p.zip"}}],"Directives":[{"$type":"FromArchive","ArchiveHashPath":["AAAAAAAAAAA=","a.txt"],"To":"a.txt","Hash":"x","Size":1}]})";
    put(t.dir / "wj/modlist", json);
    (void)!std::system(("cd '" + (t.dir / "wj").string() + "' && zip -qr '" + (t.dir / "t.wabbajack").string() + "' . >/dev/null 2>&1").c_str());
    InstallOptions opt;
    opt.output_dir = (t.dir / "out").string();
    const Report r = install_modlist(parse_modlist(json), (t.dir / "t.wabbajack").string(), opt);
    CHECK(!r.complete());
    CHECK(!r.failures.empty());
    CHECK(r.failures[0].find("hash mismatch") != std::string::npos);
    CHECK(!fs::exists(t.dir / "out/downloads/p.zip"));
    CHECK(!fs::exists(t.dir / "out/a.txt"));
}

TEST(directive_whose_source_is_the_archive_file_itself) {
    if (!have_tools()) return;
    Tmp t;
    const std::string payload = "THIS IS NOT AN ARCHIVE, IT IS A PLUGIN FILE";
    put(t.dir / "src.esm", payload);
    const std::string h = H(payload);
    Mock srv([&](const Req& r) -> Resp { return r.target == "/g.esm" ? Resp{200, payload, ""} : Resp{404, "", ""}; });
    const std::string json = std::string(R"({"Name":"T","GameType":"SkyrimSpecialEdition","Archives":[{"Hash":")") + h + R"(","Name":"g.esm","Size":)" + std::to_string(payload.size()) +
                             R"(,"State":{"$type":"HttpDownloader, Wabbajack.Lib","Url":")" + srv.base() + R"(/g.esm"}}],"Directives":[
        {"$type":"FromArchive","ArchiveHashPath":[")" + h + R"("],"To":"mods\\Cleaned\\g.esm","Hash":")" + h + R"(","Size":)" + std::to_string(payload.size()) + R"(}]})";
    put(t.dir / "wj/modlist", json);
    (void)!std::system(("cd '" + (t.dir / "wj").string() + "' && zip -qr '" + (t.dir / "t.wabbajack").string() + "' . >/dev/null 2>&1").c_str());
    InstallOptions opt;
    opt.output_dir = (t.dir / "out").string();
    const Report r = install_modlist(parse_modlist(json), (t.dir / "t.wabbajack").string(), opt);
    CHECK(r.complete());
    CHECK_EQ(slurp(t.dir / "out/mods/Cleaned/g.esm"), payload);
}

TEST(create_bsa_from_loose_files_and_extract_from_a_bsa_source) {
    if (!have_tools()) return;
    Tmp t;
    // 源 1：一个真正的 BSA（像游戏目录里的 CC 包），里面有两个文件；Wabbajack 会从它「解压」文件
    put(t.dir / "orig/meshes/a.nif", "NIF-A");
    put(t.dir / "orig/textures/b.dds", "DDS-B");
    bsa::write((t.dir / "game.bsa").string(), bsa::Header{105, 0x3, 0},
               {{"meshes\\a.nif", (t.dir / "orig/meshes/a.nif").string(), false}, {"textures\\b.dds", (t.dir / "orig/textures/b.dds").string(), false}});
    const std::string bsa_hash = wj_file_hash((t.dir / "game.bsa").string());
    const auto bsa_size = fs::file_size(t.dir / "game.bsa");
    // 期望的产物：用同样的文件与标志重新打包
    put(t.dir / "exp/meshes/a.nif", "NIF-A");
    put(t.dir / "exp/textures/b.dds", "DDS-B");
    bsa::write((t.dir / "expected.bsa").string(), bsa::Header{105, 0x3, 0},
               {{"meshes\\a.nif", (t.dir / "exp/meshes/a.nif").string(), false}, {"textures\\b.dds", (t.dir / "exp/textures/b.dds").string(), false}});
    const std::string want = wj_file_hash((t.dir / "expected.bsa").string());
    Mock srv([&](const Req& r) -> Resp { return r.target == "/game.bsa" ? Resp{200, slurp(t.dir / "game.bsa"), ""} : Resp{404, "", ""}; });
    const std::string json = std::string(R"({"Name":"T","GameType":"SkyrimSpecialEdition","Archives":[{"Hash":")") + bsa_hash + R"(","Name":"game.bsa","Size":)" + std::to_string(bsa_size) +
        R"(,"State":{"$type":"HttpDownloader, Wabbajack.Lib","Url":")" + srv.base() + R"(/game.bsa"}}],"Directives":[
        {"$type":"FromArchive","ArchiveHashPath":[")" + bsa_hash + R"(","meshes\\a.nif"],"To":"TEMP_BSA_FILES\\tid1\\meshes\\a.nif","Hash":")" + H("NIF-A") + R"(","Size":5},
        {"$type":"FromArchive","ArchiveHashPath":[")" + bsa_hash + R"(","textures\\b.dds"],"To":"TEMP_BSA_FILES\\tid1\\textures\\b.dds","Hash":")" + H("DDS-B") + R"(","Size":5},
        {"$type":"CreateBSA","TempID":"tid1","To":"mods\\Out\\out.bsa","Hash":")" + want + R"(","Size":0,
         "State":{"$type":"BSAState, Compression.BSA","ArchiveFlags":3,"FileFlags":0,"Magic":"BSA\u0000","Version":105},
         "FileStates":[{"$type":"BSAFileState, Compression.BSA","FlipCompression":false,"Index":0,"Path":"meshes\\a.nif"},
                       {"$type":"BSAFileState, Compression.BSA","FlipCompression":false,"Index":0,"Path":"textures\\b.dds"}]}]})";
    put(t.dir / "wj/modlist", json);
    (void)!std::system(("cd '" + (t.dir / "wj").string() + "' && zip -qr '" + (t.dir / "t.wabbajack").string() + "' . >/dev/null 2>&1").c_str());
    const Modlist ml = parse_modlist(json);
    CHECK(ml.directives[2].kind == Kind::CreateBSA);
    CHECK_EQ(ml.directives[2].bsa_files.size(), std::size_t{2});
    CHECK_EQ(ml.directives[2].bsa_flags, std::uint32_t{3});
    InstallOptions opt;
    opt.output_dir = (t.dir / "out").string();
    const Report r = install_modlist(ml, (t.dir / "t.wabbajack").string(), opt);
    CHECK(r.complete());
    CHECK_EQ(wj_file_hash((t.dir / "out/mods/Out/out.bsa").string()), want);   // 逐字节与期望一致
    CHECK(!fs::exists(t.dir / "out/TEMP_BSA_FILES"));                          // 临时文件已清理
    const auto idx = bsa::read_index((t.dir / "out/mods/Out/out.bsa").string());
    CHECK_EQ(idx.files.size(), std::size_t{2});
    // 重跑：幂等
    const Report r2 = install_modlist(ml, (t.dir / "t.wabbajack").string(), opt);
    CHECK(r2.complete());
}

TEST(transformed_texture_resizes_and_reencodes) {
    if (!have_tools()) return;
    Tmp t;
    dds::Image src;
    src.w = 64; src.h = 64; src.rgba.resize(64 * 64 * 4);
    for (int i = 0; i < 64 * 64; ++i) { src.rgba[static_cast<std::size_t>(i) * 4] = static_cast<std::uint8_t>(i % 64 * 4); src.rgba[static_cast<std::size_t>(i) * 4 + 1] = static_cast<std::uint8_t>(i / 64 * 4); src.rgba[static_cast<std::size_t>(i) * 4 + 2] = 90; src.rgba[static_cast<std::size_t>(i) * 4 + 3] = 255; }
    put(t.dir / "src/pack/textures/a.dds", dds::encode(src, dds::Format::BC1, 1));
    (void)!std::system(("cd '" + (t.dir / "src/pack").string() + "' && zip -qr '" + (t.dir / "pack.zip").string() + "' . >/dev/null 2>&1").c_str());
    const std::string h = wj_file_hash((t.dir / "pack.zip").string());
    Mock srv([&](const Req& r) -> Resp { return r.target == "/pack.zip" ? Resp{200, slurp(t.dir / "pack.zip"), ""} : Resp{404, "", ""}; });
    const std::string json = std::string(R"({"Name":"T","GameType":"SkyrimSpecialEdition","Archives":[{"Hash":")") + h + R"(","Name":"pack.zip","Size":)" + std::to_string(fs::file_size(t.dir / "pack.zip")) +
        R"(,"State":{"$type":"HttpDownloader, Wabbajack.Lib","Url":")" + srv.base() + R"(/pack.zip"}}],"Directives":[
        {"$type":"TransformedTexture","ArchiveHashPath":[")" + h + R"(","textures\\a.dds"],"To":"mods\\M\\textures\\a.dds","Hash":"x","Size":1,
         "ImageState":{"Width":32,"Height":32,"Format":"BC7_UNORM","MipLevels":1}}]})";
    put(t.dir / "wj/modlist", json);
    (void)!std::system(("cd '" + (t.dir / "wj").string() + "' && zip -qr '" + (t.dir / "t.wabbajack").string() + "' . >/dev/null 2>&1").c_str());
    InstallOptions opt;
    opt.output_dir = (t.dir / "out").string();
    const Report r = install_modlist(parse_modlist(json), (t.dir / "t.wabbajack").string(), opt);
    CHECK(r.complete());
    CHECK(!r.notes.empty());  // 说明纹理是近似重编码
    const dds::Image out = dds::decode(slurp(t.dir / "out/mods/M/textures/a.dds"));
    CHECK_EQ(out.w, 32);
    CHECK_EQ(out.h, 32);
    const std::string raw = slurp(t.dir / "out/mods/M/textures/a.dds");
    CHECK(raw.compare(84, 4, "DX10") == 0);  // BC7 → DX10 头
    // 左上角像素大致保持渐变起点（红≈0、绿≈0）
    CHECK(out.rgba[0] < 40);
    CHECK(out.rgba[1] < 40);
}

TEST(merged_patch_concatenates_installed_files_then_patches) {
    if (!have_tools()) return;
    Tmp t;
    const std::string patched = "AAAABBBBxyz";
    put(t.dir / "wj/in1", "AAAA");
    put(t.dir / "wj/in2", "BBBB");
    put(t.dir / "wj/pm", octo({{'c', "0,8"}, {'d', "xyz"}}));
    const std::string json = std::string(R"({"Name":"T","GameType":"SkyrimSpecialEdition","Archives":[],"Directives":[
        {"$type":"InlineFile","SourceDataID":"in1","To":"mods\\X\\one.txt","Hash":")") + H("AAAA") + R"(","Size":4},
        {"$type":"InlineFile","SourceDataID":"in2","To":"mods\\X\\two.txt","Hash":")" + H("BBBB") + R"(","Size":4},
        {"$type":"MergedPatch","PatchID":"pm","To":"mods\\Merged\\m.txt","Hash":")" + H(patched) + R"(","Size":11,
         "Sources":[{"RelativePath":"mods\\X\\one.txt","Hash":"x"},{"RelativePath":"mods\\X\\two.txt","Hash":"y"}]}]})";
    put(t.dir / "wj/modlist", json);
    (void)!std::system(("cd '" + (t.dir / "wj").string() + "' && zip -qr '" + (t.dir / "t.wabbajack").string() + "' . >/dev/null 2>&1").c_str());
    InstallOptions opt;
    opt.output_dir = (t.dir / "out").string();
    const Modlist ml = parse_modlist(json);
    CHECK(ml.directives[2].kind == Kind::MergedPatch);
    CHECK_EQ(ml.directives[2].merge_sources.size(), std::size_t{2});
    const Report r = install_modlist(ml, (t.dir / "t.wabbajack").string(), opt);
    CHECK(r.complete());
    CHECK_EQ(slurp(t.dir / "out/mods/Merged/m.txt"), patched);
    // 幂等
    CHECK(install_modlist(ml, (t.dir / "t.wabbajack").string(), opt).complete());
}

// nxm:// 链接处理器（免费 Nexus 账号的下载流程）与「默认实例」。
//   instance default [--set]  查看/设置默认实例
//   nxm register              把 mo-linux 注册为浏览器的 nxm:// 处理器
//   nxm handle URL            下载链接指向的文件到实例的 downloads/，并交给正在等它的集合 mod
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "mol/collection.hpp"
#include "mol/instance.hpp"
#include "mol/nexus.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {
namespace {
namespace fs = std::filesystem;
std::pmr::string P(std::string_view s, mol::mr* m) { return std::pmr::string(s, m); }
Result ok(Context& ctx) {
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    return r;
}
std::string self_exe() {
    std::error_code ec;
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string("mo-linux") : p.string();
}
}  // namespace

Result run_instance_default(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    bool changed = false;
    if (ctx.args.get_bool("--set", false)) {
        std::error_code ec;
        const fs::path root{std::string(ctx.instance_dir)};
        if (!fs::exists(root / "mo-linux.json", ec) && !fs::exists(root / "ModOrganizer.ini", ec))
            throw mol::Error("instance_not_found", "not an instance (no mo-linux.json / ModOrganizer.ini)", root.string());
        mol::set_default_instance(root.string());
        changed = true;
    }
    Result r = ok(ctx);
    r.set_data(DefaultInstanceData{.path = P(mol::default_instance_path(ctx.mem), ctx.mem), .changed = changed});
    return r;
}

Result run_nxm_register(Context& ctx) {
    const char* home = std::getenv("HOME");
    if (!home) throw mol::Error("io_error", "HOME is not set");
    const fs::path dir = fs::path(home) / ".local/share/applications";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / "mo-linux-nxm.desktop";
    const std::string exec = "\"" + self_exe() + "\" nxm handle %u";
    {
        std::ofstream os(file, std::ios::trunc);
        os << "[Desktop Entry]\nType=Application\nName=mo-linux nxm handler\nComment=Download Nexus Mods files for mo-linux\n"
           << "Exec=" << exec << "\nNoDisplay=true\nTerminal=false\nMimeType=x-scheme-handler/nxm;\n";
        if (!os) throw mol::Error("io_error", "cannot write the .desktop file", file.string());
    }
    // 尽力设置默认关联；没有 xdg-mime 就只写文件
    const int rc = std::system("xdg-mime default mo-linux-nxm.desktop x-scheme-handler/nxm >/dev/null 2>&1");
    (void)std::system("update-desktop-database \"$HOME/.local/share/applications\" >/dev/null 2>&1");
    Result r = ok(ctx);
    if (rc != 0) r.add_warning("nxm_mime", "xdg-mime failed or is missing; associate x-scheme-handler/nxm with mo-linux-nxm.desktop manually", file.string());
    r.set_data(NxmRegisterData{.desktop_file = P(file.string(), ctx.mem), .exec = P(exec, ctx.mem), .mime_registered = rc == 0});
    return r;
}

Result run_nxm_handle(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const mol::NxmUrl nxm = mol::parse_nxm(ctx.args.positionals.front(), ctx.mem);
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const auto key = mol::load_nexus_key(ctx.mem);
    if (!key) throw mol::Error("nexus_auth", "no Nexus API key (set NEXUS_API_KEY or run `nexus login`)");
    const mol::NexusClient client(*key, "0.1");
    const auto dl = mol::nexus_download(client, inst.downloads_dir, nxm.game, nxm.mod_id, nxm.file_id, &nxm);

    // 有没有集合在等这个文件：扫 collections/*/state.json 里 pending/failed 的 mod，按清单里的 modId/fileId 对上就记下压缩包
    NxmHandleData d{.instance = P(inst.root, ctx.mem), .path = P(dl.path, ctx.mem), .size = dl.size, .game = P(nxm.game, ctx.mem), .mod_id = nxm.mod_id,
                    .file_id = nxm.file_id, .matches = std::pmr::vector<NxmMatchRow>(ctx.mem)};
    std::error_code ec;
    const fs::path cdir = fs::path(std::string(inst.root)) / "collections";
    for (fs::directory_iterator it(cdir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string slug = it->path().filename().string();
        auto st = mol::collection::load_state(inst, slug);
        // 清单：优先用 state 里记的 revision 对应的缓存
        fs::path j = it->path() / ("archive-" + std::to_string(st.revision)) / "collection.json";
        if (!fs::exists(j, ec)) j = it->path() / "archive-local" / "collection.json";
        if (!fs::exists(j, ec)) continue;
        std::ifstream in(j, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto coll = mol::collection::parse_collection(text);
        bool touched = false;
        for (const auto& m : coll.mods) {
            if (m.source.type != "nexus" || m.source.mod_id != nxm.mod_id || m.source.file_id != nxm.file_id) continue;
            auto& ms = st.mods[m.key()];
            if (ms.status == "installed") continue;
            ms.archive = std::string(dl.path);  // 下次 install 会复用，并照常做 md5 校验
            touched = true;
            d.matches.push_back(NxmMatchRow{P(slug, ctx.mem), P(m.key(), ctx.mem), P(m.name, ctx.mem)});
        }
        if (touched) mol::collection::save_state(inst, st);
    }
    // 桌面通知（从浏览器启动时没有终端）：尽力而为
    {
        std::string msg = "Downloaded " + fs::path(std::string(dl.path)).filename().string();
        if (!d.matches.empty()) msg += " (for collection '" + std::string(d.matches.front().collection) + "'; run `collection install` to continue)";
        std::string cmd = "command -v notify-send >/dev/null 2>&1 && notify-send 'mo-linux' '";
        for (char c : msg) if (c != '\'') cmd.push_back(c);
        cmd += "' >/dev/null 2>&1";
        (void)std::system(cmd.c_str());
    }
    Result r = ok(ctx);
    r.set_data(std::move(d));
    return r;
}

}  // namespace cli

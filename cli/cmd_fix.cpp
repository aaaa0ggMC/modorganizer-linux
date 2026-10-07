// 修复命令（docs/PLAN-autofix.md）：
//   fix content-catalog  挪开被新版游戏写坏的 ContentCatalog.txt（D1）
//   fix vcrun            给前缀装当前的 VC++ 2015-2022 运行库（D2）
//   enb install          把 enbdev.com 的 ENB 压缩包里的 d3d11.dll / d3dcompiler_46e.dll 装成根目录型 mod（D3）
// 都会改前缀或实例：游戏运行时拒绝（farm_busy）。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "mol/casefold.hpp"
#include "mol/health.hpp"
#include "mol/http.hpp"
#include "mol/instance.hpp"
#include "mol/mod_install.hpp"
#include "mol/overwrite.hpp"
#include "mol/runner.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {
namespace {
namespace fs = std::filesystem;
namespace h = mol::health;

std::pmr::vector<std::pmr::string> runtime_rows(const std::vector<h::RuntimeDll>& v, mol::mr* mem) {
    std::pmr::vector<std::pmr::string> out(mem);
    for (const auto& d : v) out.emplace_back(d.name + " " + (d.version.empty() ? std::string("missing") : d.version));
    return out;
}

bool on_path(const char* tool) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string p(path);
    for (std::size_t b = 0; b <= p.size();) {
        std::size_t e = p.find(':', b);
        if (e == std::string::npos) e = p.size();
        if (e > b && ::access((p.substr(b, e - b) + "/" + tool).c_str(), X_OK) == 0) return true;
        b = e + 1;
    }
    return false;
}

fs::path cache_dir() {
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return fs::path(x) / "mo-linux";
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".cache/mo-linux";
}

// dir 下（递归）大小写不敏感地找 name；有多个时优先路径里带 prefer 的
fs::path find_file_ci(const fs::path& dir, std::string_view name, std::string_view prefer) {
    std::error_code ec;
    fs::path best;
    const auto want = mol::casefold(name);
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || mol::casefold(it->path().filename().string()) != want) continue;
        if (best.empty() || std::string(mol::casefold(it->path().string())).find(prefer) != std::string::npos) best = it->path();
    }
    return best;
}

}  // namespace

Result run_fix_content_catalog(Context& ctx) {
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    mol::require_farm_idle(inst);
    const std::string bak = h::move_content_catalog_aside(inst);
    Result r = make_ok(ctx);
    r.set_data(FixData{.message = std::pmr::string(bak.empty() ? "no ContentCatalog.txt in the prefix, nothing to do"
                                                               : "moved aside to " + bak + " (the game rebuilds it on the next start)", ctx.mem),
                       .changed = !bak.empty(), .backup = std::pmr::string(bak, ctx.mem), .method = std::pmr::string(ctx.mem),
                       .before = std::pmr::vector<std::pmr::string>(ctx.mem), .after = std::pmr::vector<std::pmr::string>(ctx.mem),
                       .mod_name = std::pmr::string(ctx.mem), .files = std::pmr::vector<std::pmr::string>(ctx.mem)});
    return r;
}

Result run_fix_vcrun(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    const auto before = h::vc_runtime(inst);
    FixData d{.message = std::pmr::string(ctx.mem), .changed = false, .backup = std::pmr::string(ctx.mem), .method = std::pmr::string(ctx.mem),
              .before = runtime_rows(before, ctx.mem), .after = std::pmr::vector<std::pmr::string>(ctx.mem), .mod_name = std::pmr::string(ctx.mem),
              .files = std::pmr::vector<std::pmr::string>(ctx.mem)};
    if (h::vc_runtime_ok(before) && !ctx.args.get_bool("--force", false)) {
        d.message = std::pmr::string("the prefix already has VC++ runtime " + before.front().version, ctx.mem);
        d.after = d.before;
        Result r = make_ok(ctx);
        r.set_data(std::move(d));
        return r;
    }
    mol::require_farm_idle(inst);  // 游戏在跑时不换它正在用的 dll

    constexpr std::string_view app_id = "489830";
    int rc = 0;
    if (on_path("protontricks")) {
        // -q 必须在 appid 之后：放在前面 protontricks 自己会报 unrecognized arguments
        d.method = std::pmr::string("protontricks", ctx.mem);
        mol::LaunchSpec spec(ctx.mem);
        for (std::string_view a : {std::string_view("protontricks"), app_id, std::string_view("-q"), std::string_view("vcrun2022")}) spec.argv.emplace_back(a);
        rc = mol::spawn_launch(spec, true);
    } else {
        // 自己来：下载微软的再发行包，用实例的 Proton 在这个前缀里静默安装
        d.method = std::pmr::string("redist", ctx.mem);
        const fs::path dir = cache_dir();
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path exe = dir / "vc_redist.x64.exe";
        if (!fs::is_regular_file(exe, ec) || fs::file_size(exe, ec) < 1000000) mol::http_download(h::kVcRedistUrl, exe.string());
        static constexpr std::string_view args[] = {"/install", "/quiet", "/norestart"};
        mol::LaunchOptions opt;
        opt.steam_app_id = app_id;
        opt.args = args;
        mol::LaunchSpec spec = mol::build_launch(inst, exe.string(), opt, ctx.mem);
        spec.cwd = mol::string(dir.string(), ctx.mem);
        rc = mol::spawn_launch(spec, true);
        if (rc == 3010 || rc == 1638) rc = 0;  // 需要重启 / 已装了更新的版本：都算成功
    }
    const auto after = h::vc_runtime(inst);
    d.after = runtime_rows(after, ctx.mem);
    if (!h::vc_runtime_ok(after))
        throw mol::Error("io_error", "the VC++ runtime is still too old after installing (" + std::string(d.method) + " exited with " + std::to_string(rc) + ")",
                         std::string(inst.cfg.prefix) + "/drive_c/windows/system32/msvcp140.dll");
    d.changed = true;
    d.message = std::pmr::string("VC++ runtime " + (before.front().version.empty() ? std::string("missing") : before.front().version) + " -> " +
                                     after.front().version + " (via " + std::string(d.method) + ")", ctx.mem);
    Result r = make_ok(ctx);
    r.set_data(std::move(d));
    return r;
}

Result run_enb_install(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const std::string archive(ctx.args.get("--archive", "", ctx.mem));
    if (archive.empty()) return make_usage_error("enb install: --archive FILE is required (the zip from enbdev.com)", ctx);
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    std::error_code ec;
    if (!fs::is_regular_file(archive, ec)) throw mol::Error("invalid_argument", "archive not found", archive);
    mol::require_farm_idle(inst);

    // 名字带上压缩包里的版本号（enbseries_skyrimse_v0503.zip → v0503）
    std::string ver;
    const std::string stem = fs::path(archive).stem().string();
    if (const auto v = std::string(mol::casefold(stem)).rfind("_v"); v != std::string::npos) ver = stem.substr(v + 1);
    const std::string name = ver.empty() ? std::string("ENB Binaries") : "ENB Binaries (" + ver + ")";

    const fs::path mods{std::string(inst.mods_dir)};
    const fs::path tmp = mods / (".mol-enb-" + std::to_string(::getpid()));
    const fs::path stage = mods / (".mol-enb-stage-" + std::to_string(::getpid()));
    fs::remove_all(tmp, ec);
    fs::remove_all(stage, ec);
    fs::create_directories(tmp, ec);
    fs::create_directories(stage, ec);
    std::pmr::vector<std::pmr::string> files(ctx.mem);
    try {
        mol::extract_archive(archive, tmp.string());
        // ENB 的压缩包里有 WrapperVersion/ 与 InjectorVersion/：Skyrim SE 用 Wrapper 版
        for (const char* n : {"d3d11.dll", "d3dcompiler_46e.dll"}) {
            const fs::path f = find_file_ci(tmp, n, "wrapperversion");
            if (f.empty()) {
                if (std::string_view(n) == "d3d11.dll") throw mol::Error("invalid_argument", "no d3d11.dll in this archive; is it the ENB for Skyrim SE from enbdev.com?", archive);
                continue;
            }
            fs::copy_file(f, stage / n, fs::copy_options::overwrite_existing, ec);
            if (ec) throw mol::Error("io_error", "cannot copy " + std::string(n) + ": " + ec.message(), f.string());
            files.emplace_back(n);
        }
        fs::remove_all(tmp, ec);
        const fs::path target = mods / name;
        if (fs::exists(fs::symlink_status(target, ec))) {  // 同版本重装：新文件齐了才换掉
            const fs::path old = mods / (".mol-enb-old-" + std::to_string(::getpid()));
            fs::rename(target, old, ec);
            if (ec) throw mol::Error("io_error", "cannot replace " + name + ": " + ec.message(), target.string());
            fs::remove_all(old, ec);
        }
        fs::rename(stage, target, ec);
        if (ec) throw mol::Error("io_error", "cannot move the ENB files into place: " + ec.message(), target.string());
        mol::mark_mod_root(target.string(), true);
    } catch (...) {
        fs::remove_all(tmp, ec);
        fs::remove_all(stage, ec);
        throw;
    }
    bool listed = false;
    for (const auto& m : mol::list_mods(inst, ctx.profile_override(), ctx.mem))
        if (mol::casefold(m.name) == mol::casefold(name)) listed = true;
    if (!listed) mol::add_mod(inst, name, true, ctx.profile_override());

    Result r = make_ok(ctx);
    r.set_data(FixData{.message = std::pmr::string("installed " + name + " as a root-folder mod (" + std::to_string(files.size()) + " file(s)); run `apply` or just `run`", ctx.mem),
                       .changed = true, .backup = std::pmr::string(ctx.mem), .method = std::pmr::string(ctx.mem),
                       .before = std::pmr::vector<std::pmr::string>(ctx.mem), .after = std::pmr::vector<std::pmr::string>(ctx.mem),
                       .mod_name = std::pmr::string(name, ctx.mem), .files = std::move(files)});
    return r;
}

}  // namespace cli

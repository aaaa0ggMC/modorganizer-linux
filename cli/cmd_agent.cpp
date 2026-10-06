// 面向 Agent 的三个命令：
//   schema  自描述：所有命令/选项/副作用/错误码（机器可读）
//   next    看一眼实例状态，给出建议的下一步命令（含「需要人」「需要确认」标记）
//   logs    列出/读取前缀里的游戏、SKSE、崩溃日志，方便 Agent 排错
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <vector>

#include "mol/collection.hpp"
#include "mol/doctor.hpp"
#include "mol/instance.hpp"
#include "mol/nexus.hpp"

#include "routes.hpp"

import alib6;
import std;

namespace cli {
namespace {
namespace fs = std::filesystem;

std::vector<std::string_view> split_commas(std::string_view s) {
    std::vector<std::string_view> out;
    for (std::size_t b = 0; b <= s.size();) {
        std::size_t e = s.find(',', b);
        if (e == std::string_view::npos) e = s.size();
        if (e > b) out.push_back(s.substr(b, e - b));
        b = e + 1;
    }
    return out;
}

const CommandMeta* meta_of(std::string_view name) {
    for (const auto& m : kMeta)
        if (m.name == name) return &m;
    return nullptr;
}

void put_option(alib6::AData& arr, std::ptrdiff_t& i, const OptionSpec& o) {
    auto& e = arr[i++];
    e["name"] = o.name;
    e["long"] = o.long_name;
    e["short"] = o.short_name;
    e["takes_value"] = o.takes_value;
    e["description"] = o.description;
}

}  // namespace

Result run_schema(Context& ctx) {
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    alib6::AData doc(ctx.mem);
    doc["tool"] = "mo-linux";
    doc["version"] = "0.0.1";
    doc["envelope"] = "{schema_version, ok, command, data, warnings[{code,message,path,hint}], errors[{code,message,path,hint}]} on stdout with --json; logs go to stderr";
    auto& exits = doc["exit_codes"];
    exits["0"] = "ok";
    exits["1"] = "runtime error (see errors[])";
    exits["2"] = "usage error (bad command or arguments)";
    exits["3"] = "drift: `status` found the farm out of date, or `doctor` found an error";
    exits["4"] = "incomplete: the command made progress but needs input (see data.pending); decide, record it with the matching resolve/--fomod option, run the same command again";

    auto& errs = doc["errors"];
    errs._set_array();
    std::ptrdiff_t ei = 0;
    for (const char* code : {"instance_not_found", "config_invalid", "profile_not_found", "mod_not_found", "invalid_argument", "farm_not_owned", "farm_conflict",
                             "farm_busy", "wine_busy", "io_error", "game_unavailable", "network_error", "nexus_auth", "nexus_premium", "nexus_not_found", "nexus_rate_limited",
                             "skse_mismatch", "fomod_choices_required"}) {
        auto& e = errs[ei++];
        e["code"] = code;
        e["hint"] = default_hint(code);
    }

    auto& gl = doc["global_options"];
    gl._set_array();
    std::ptrdiff_t gi = 0;
    for (const auto& o : kOptGlobal) put_option(gl, gi, o);
    for (const auto& o : kTogGlobal) put_option(gl, gi, o);

    auto& cmds = doc["commands"];
    cmds._set_array();
    std::ptrdiff_t ci = 0;
    for (const auto& route : kRoutes) {
        auto& c = cmds[ci++];
        c["name"] = route.name;
        if (const CommandMeta* m = meta_of(route.name)) {
            c["summary"] = m->summary;
            auto& ef = c["effects"];
            ef._set_array();
            std::ptrdiff_t k = 0;
            for (auto t : split_commas(m->effects)) ef[k++] = t;
            auto& nd = c["needs"];
            nd._set_array();
            k = 0;
            for (auto t : split_commas(m->needs)) nd[k++] = t;
            c["confirm"] = m->confirm;
            c["idempotent"] = m->idempotent;
        }
        auto& pos = c["positionals"];
        pos._set_array();
        if (route.positionals > 0) pos[0] = route.positional_name;
        auto& opts = c["options"];
        opts._set_array();
        std::ptrdiff_t oi = 0;
        for (const auto& o : route.specs) put_option(opts, oi, o);
    }
    r.data = std::move(doc);
    return r;
}

// checks：已算好的 doctor 结果（overview 复用，避免同一进程跑两遍体检）；nullptr = 这里自己跑。
NextData next_data(Context& ctx, const mol::Instance* inst, const mol::vector<mol::Check>* checks) {
    NextData d{.ready = false, .instance = std::pmr::string(ctx.instance_dir, ctx.mem), .steps = std::pmr::vector<NextStep>(ctx.mem)};
    auto add = [&](std::string_view id, std::string_view why, std::initializer_list<std::string_view> cmd, std::string_view effects, bool blocking, bool human, bool confirm) {
        NextStep s{.id = std::pmr::string(id, ctx.mem), .why = std::pmr::string(why, ctx.mem), .command = std::pmr::vector<std::pmr::string>(ctx.mem),
                   .effects = std::pmr::string(effects, ctx.mem), .blocking = blocking, .needs_human = human, .confirm = confirm};
        for (auto c : cmd) s.command.push_back(std::pmr::string(c, ctx.mem));
        d.steps.push_back(std::move(s));
    };
    auto finish = [&] {
        d.ready = std::none_of(d.steps.begin(), d.steps.end(), [](const NextStep& s) { return s.blocking; });
        return std::move(d);
    };

    if (!inst) {
        add("instance.init", "no instance here; this creates one and auto-detects the Steam game, Proton and prefix", {"instance", "init"}, "instance", true, false, false);
        return finish();
    }

    mol::vector<mol::Check> own(ctx.mem);
    if (!checks) {
        own = mol::run_doctor(*inst, game_info_string(ctx, *inst, "version"), ctx.mem);
        checks = &own;
    }
    for (const auto& c : *checks) {
        if (c.level == "ok") continue;
        const bool blocking = c.level == "error" || c.id == "farm.busy";  // 游戏还在跑时不能 apply/run
        if (!c.fix.empty()) {
            NextStep s{.id = std::pmr::string(c.id, ctx.mem), .why = std::pmr::string(c.message, ctx.mem), .command = std::pmr::vector<std::pmr::string>(ctx.mem),
                       .effects = std::pmr::string(ctx.mem), .blocking = blocking, .needs_human = false, .confirm = false};
            for (const auto& f : c.fix) s.command.push_back(std::pmr::string(f, ctx.mem));
            std::string cname;
            for (std::size_t i = 0; i < c.fix.size() && i < 2; ++i) {
                std::string cand = cname.empty() ? std::string(c.fix[i]) : cname + " " + std::string(c.fix[i]);
                if (meta_of(cand)) cname = cand;
                else if (cname.empty()) cname = std::string(c.fix[i]);
            }
            if (const CommandMeta* m = meta_of(cname)) { s.effects = std::pmr::string(m->effects, ctx.mem); s.confirm = m->confirm; }
            d.steps.push_back(std::move(s));
        } else {
            // 没有自动修复：把 doctor 的说明交给人/Agent 判断
            add(c.id, std::string(c.message) + (c.hint.empty() ? "" : " -> " + std::string(c.hint)), {}, "", blocking, true, false);
        }
    }

    // 集合安装是否有未完成的
    std::error_code ec;
    const fs::path cdir = fs::path(std::string(inst->root)) / "collections";
    for (fs::directory_iterator it(cdir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string slug = it->path().filename().string();
        mol::collection::State st;
        try { st = mol::collection::load_state(*inst, slug); } catch (const mol::Error&) { continue; }
        std::size_t pending = 0, failed = 0;
        for (const auto& [k, m] : st.mods) {
            if (m.status == "pending") ++pending;
            else if (m.status == "failed") ++failed;
        }
        if (pending + failed > 0)
            add("collection." + slug, std::to_string(pending) + " pending, " + std::to_string(failed) + " failed mod(s) in collection '" + slug + "': inspect with `collection status`, decide with `collection resolve`, then re-run `collection install`",
                {"collection", "status", slug}, "read", true, false, false);
    }

    // 需要 Nexus 的步骤但没有 key → 需要人
    const bool wants_nexus = std::any_of(d.steps.begin(), d.steps.end(), [](const NextStep& s) {
        return !s.command.empty() && (s.command[0] == "skse" || s.command[0] == "nexus" || s.command[0] == "collection");
    });
    if (wants_nexus && !mol::load_nexus_key()) {
        NextStep s{.id = std::pmr::string("nexus.login", ctx.mem),
                   .why = std::pmr::string("later steps download from Nexus; ask the user for a personal API key, then pipe it to `mo-linux nexus login`", ctx.mem),
                   .command = std::pmr::vector<std::pmr::string>(ctx.mem), .effects = std::pmr::string("network", ctx.mem), .blocking = true, .needs_human = true, .confirm = false};
        s.command.push_back(std::pmr::string("nexus", ctx.mem));
        s.command.push_back(std::pmr::string("login", ctx.mem));
        d.steps.insert(d.steps.begin(), std::move(s));
    }

    if (std::none_of(d.steps.begin(), d.steps.end(), [](const NextStep& s) { return s.blocking; })) {
        const bool has_skse = mol::root_provides(*inst, "skse64_loader.exe", ctx.mem);
        if (has_skse) add("run.skse", "everything checks out; start the game through SKSE (this launches the game: confirm with the user first)", {"run", "--skse", "--detach"}, "instance,farm,prefix,launch", false, false, true);
        else add("run", "everything checks out; start the game (this launches the game: confirm with the user first)", {"run", "--detach"}, "instance,farm,prefix,launch", false, false, true);
    }
    return finish();
}

Result run_next(Context& ctx) {
    std::optional<mol::Instance> inst;
    try {
        inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    } catch (const mol::Error& e) {
        if (e.code != "instance_not_found") throw;
    }
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(next_data(ctx, inst ? &*inst : nullptr, nullptr));
    return r;
}

Result run_logs(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const auto inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);
    std::int64_t tail = 80;
    if (ctx.args.has("--tail")) {
        const std::string t(ctx.args.get("--tail", "", ctx.mem));
        if (t.empty() || t.find_first_not_of("0123456789") != std::string::npos || t.size() > 6) return make_usage_error("logs: --tail needs a non-negative integer", ctx);
        tail = std::min<std::int64_t>(std::stoll(t), 2000);
    }
    // 游戏/SKSE 日志位置：<前缀>/drive_c/users/<user>/Documents/My Games/Skyrim Special Edition/SKSE
    const fs::path dir = fs::path(std::string(inst.cfg.prefix)) / "drive_c/users" / std::string(inst.cfg.prefix_user) / "Documents/My Games/Skyrim Special Edition/SKSE";
    LogsData d{.dir = std::pmr::string(dir.string(), ctx.mem), .files = std::pmr::vector<LogFileRow>(ctx.mem), .name = std::pmr::string(ctx.mem), .tail = std::pmr::string(ctx.mem)};
    std::error_code ec;
    struct F { std::string name; std::int64_t size, mtime; };
    std::vector<F> fl;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto t = std::chrono::clock_cast<std::chrono::system_clock>(fs::last_write_time(it->path(), ec));
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
        fl.push_back({it->path().filename().string(), static_cast<std::int64_t>(it->file_size(ec)), static_cast<std::int64_t>(secs)});
    }
    std::sort(fl.begin(), fl.end(), [](const F& a, const F& b) { return a.mtime != b.mtime ? a.mtime > b.mtime : a.name < b.name; });
    for (const auto& f : fl) d.files.push_back(LogFileRow{std::pmr::string(f.name, ctx.mem), f.size, f.mtime});

    const std::string want(ctx.args.get("--file", "", ctx.mem));
    if (!want.empty()) {
        if (want.find('/') != std::string::npos || want.find('\\') != std::string::npos || want == "." || want == "..")
            return make_usage_error("logs: --file must be a plain file name inside the log directory", ctx);
        const fs::path p = dir / want;
        if (!fs::is_regular_file(p, ec)) throw mol::Error("mod_not_found", "no such log file: " + want, dir.string());
        std::ifstream in(p, std::ios::binary);
        std::deque<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            if (static_cast<std::int64_t>(lines.size()) > tail) lines.pop_front();
        }
        std::string joined;
        for (const auto& l : lines) joined += l + "\n";
        d.name = std::pmr::string(want, ctx.mem);
        d.tail = std::pmr::string(joined, ctx.mem);
    }
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(std::move(d));
    return r;
}

}  // namespace cli

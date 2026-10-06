// fomod inspect：只读。解出压缩包里的 FOMOD 配置，按（可选的）已有选择求值，报告各步骤/组/插件的状态与将安装的文件。
// GUI 的用法：先不带 --choices 调用拿到默认状态；用户每改一组选择就带上累计的 choices 再调用，
// 以便获得更新后的步骤可见性与插件类型（FOMOD 的后续步骤依赖前面步骤设置的标志）。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "mol/fomod.hpp"
#include "mol/instance.hpp"
#include "mol/mod_install.hpp"

#include "commands.hpp"

import alib6;
import std;

namespace cli {

Result run_fomod_inspect(Context& ctx) {
    if (!ctx.args.ok()) return make_usage_error(ctx.args.error, ctx);
    const mol::string archive = ctx.args.positionals.front();
    const mol::string choices_file = ctx.args.get("--choices", "", ctx.mem);
    const mol::string images_dir = ctx.args.get("--images", "", ctx.mem);
    const mol::Instance inst = mol::load_instance(ctx.instance_dir, ctx.profile_override(), ctx.mem);

    mol::fomod::Choices choices;
    if (!choices_file.empty()) {
        std::ifstream in{std::string(choices_file), std::ios::binary};
        if (!in) throw mol::Error("io_error", "cannot read the choices file", std::string(choices_file));
        choices = mol::fomod::parse_choices_json(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
    }

    FomodInspectData d{.has_fomod = false, .module_name = std::pmr::string(ctx.mem),
                       .steps = std::pmr::vector<FomodStepRow>(ctx.mem), .files = std::pmr::vector<FomodFileRow>(ctx.mem)};
    if (const auto cfg = mol::read_archive_fomod(inst, archive)) {
        d.has_fomod = true;
        d.module_name = std::pmr::string(cfg->module_name, ctx.mem);
        mol::fomod::Env env;
        env.file_state = mol::fomod_file_state(inst, ctx.profile_override(), ctx.mem);
        if (auto v = game_info_string(ctx, inst, "version"); !v.empty()) env.game_version = v;
        if (auto v = game_info_string(ctx, inst, "scriptExtender", "version"); !v.empty()) env.script_extender_version = v;
        const auto r = mol::fomod::resolve(*cfg, choices, true, env);
        std::map<std::string, std::string> image_paths;
        if (!images_dir.empty()) {
            std::vector<std::string> imgs;
            for (const auto& s : r.steps)
                for (const auto& g : s.groups)
                    for (const auto& p : g.plugins)
                        if (!p.image.empty()) imgs.emplace_back(p.image);
            image_paths = mol::extract_fomod_images(inst, archive, imgs, images_dir);
        }
        for (const auto& s : r.steps) {
            FomodStepRow sr{.name = std::pmr::string(s.name, ctx.mem), .key = std::pmr::string(s.key, ctx.mem), .visible = s.visible, .groups = std::pmr::vector<FomodGroupRow>(ctx.mem)};
            for (const auto& g : s.groups) {
                FomodGroupRow gr{.name = std::pmr::string(g.name, ctx.mem), .key = std::pmr::string(g.key, ctx.mem), .type = std::pmr::string(mol::fomod::to_string(g.type), ctx.mem),
                                 .explicit_choice = g.explicit_choice, .plugins = std::pmr::vector<FomodPluginRow>(ctx.mem)};
                for (const auto& p : g.plugins)
                    gr.plugins.push_back(FomodPluginRow{.name = std::pmr::string(p.name, ctx.mem), .description = std::pmr::string(p.description, ctx.mem),
                                                        .image = std::pmr::string(p.image, ctx.mem),
                                                        .image_path = std::pmr::string(image_paths.count(std::string(p.image)) ? image_paths.at(std::string(p.image)) : std::string(), ctx.mem),
                                                        .type = std::pmr::string(mol::fomod::to_string(p.type), ctx.mem),
                                                        .selected = p.selected});
                sr.groups.push_back(std::move(gr));
            }
            d.steps.push_back(std::move(sr));
        }
        for (const auto& f : r.files)
            d.files.push_back(FomodFileRow{.source = std::pmr::string(f.source, ctx.mem), .destination = std::pmr::string(f.destination, ctx.mem),
                                           .folder = f.folder, .priority = f.priority});
    }
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(std::move(d));
    return r;
}

}  // namespace cli

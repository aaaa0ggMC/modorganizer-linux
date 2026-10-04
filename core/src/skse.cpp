#include "mol/skse.hpp"

#include <algorithm>

#include "mol/doctor.hpp"
#include "mol/mod_install.hpp"

namespace mol {

SkseResult install_skse(const Instance& inst, std::string_view game_version, const NexusClient& client,
                        const std::function<bool(std::uint64_t, std::uint64_t)>& progress, mr* mem) {
    SkseResult res(mem);
    res.game_version = string(game_version, mem);
    res.runtime_dll = skse_dll_name(game_version, mem);
    if (res.runtime_dll.empty())
        throw Error("invalid_argument", "cannot derive the SKSE64 runtime from game version '" + std::string(game_version) + "'");

    if (root_provides(inst, res.runtime_dll, mem) && root_provides(inst, "skse64_loader.exe", mem)) return res;  // 已就绪

    const string domain = nexus_game_domain(inst.cfg.game, mem);
    // 选文件：主文件（is_primary）优先，其次类别为 MAIN 的 file_id 最大者。
    const auto files = client.mod_files(domain, kSkseNexusModId, mem);
    const NexusFile* pick = nullptr;
    for (const auto& f : files)
        if (f.is_primary && (!pick || f.file_id > pick->file_id)) pick = &f;
    if (!pick)
        for (const auto& f : files)
            if (f.category == "MAIN" && (!pick || f.file_id > pick->file_id)) pick = &f;
    if (!pick) throw Error("mod_not_found", "no suitable SKSE64 main file found on Nexus");
    res.file_id = pick->file_id;
    res.file_name = pick->file_name;

    const auto dl = nexus_download(client, inst.downloads_dir, domain, kSkseNexusModId, pick->file_id, nullptr, progress, mem);
    res.downloaded = !dl.reused;

    // mod 名：SKSE64；已被占用则报错（用户需处理，不能覆盖）。
    const auto inst_res = install_archive(inst, dl.path, "SKSE64", true, {}, mem);
    res.mod_name = inst_res.name;
    res.installed = true;

    if (!root_provides(inst, res.runtime_dll, mem))
        throw Error("skse_mismatch",
                    "the latest SKSE64 on Nexus (" + std::string(pick->version) + ") does not provide " + std::string(res.runtime_dll) +
                        " for game version " + std::string(game_version) + "; the installed mod 'SKSE64' was kept but will not load",
                    std::string(res.runtime_dll));
    return res;
}

}  // namespace mol

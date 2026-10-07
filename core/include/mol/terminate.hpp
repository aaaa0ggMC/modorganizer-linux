#pragma once
// terminate：结束占用某个实例的所有进程（游戏、残留的 steam.exe steam://run/…、winedevice.exe、wineserver……）。
// 「占用」= 满足任一：
//   * 工作目录或命令行落在农场里（与 farm_busy 的判定相同）；
//   * 环境变量 WINEPREFIX / STEAM_COMPAT_DATA_PATH 指向本实例的前缀（Proton/Wine 拉起的所有 Windows 进程都带着它）。
// 永远不碰：自己和自己的祖先进程、交互式 shell（终端恰好 cd 在农场里）、不属于当前用户的进程。
// 本机的 Steam 客户端不带前缀的环境变量，所以不会被选中。
#include <string>
#include <vector>

#include "mol/instance.hpp"

namespace mol {

struct InstanceProcess {
    int pid = 0;
    std::string command;  // 命令行（参数以空格连接，截断到 160 字节）
    std::string reason;   // farm_cwd | farm_cmdline | prefix_env
};

// 只读：列出占用实例的进程（按 pid 排序）。
std::vector<InstanceProcess> instance_processes(const Instance& inst);

struct TerminateResult {
    std::vector<InstanceProcess> found;   // 开始时找到的
    std::vector<int> terminated;          // SIGTERM 后自己退出的
    std::vector<int> killed;              // 等满 timeout 仍在、被 SIGKILL 的
    std::vector<int> remaining;           // SIGKILL 之后仍然在的（不该发生；权限之类）
    bool wineserver_killed = false;       // 执行了 `wineserver -k`
};

// dry_run：只列出，不发信号。否则 SIGTERM → 最多等 timeout_ms → SIGKILL → 对本实例的前缀执行 `wineserver -k`。
TerminateResult terminate_instance(const Instance& inst, int timeout_ms = 10000, bool dry_run = false);

}  // namespace mol

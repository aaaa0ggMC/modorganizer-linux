// mo-linux version 子命令。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include "commands.hpp"

import alib6;
import std;

namespace cli {

Result run_version(Context& ctx) {
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(VersionData{
        .name = mol::string("mo-linux", ctx.mem),
        .version = mol::string("0.0.1", ctx.mem),
    });
    return r;
}

}  // namespace cli

// mo-linux CLI 入口（骨架；子命令由后续工作包实现）。
import alib6;
import std;

int main(int argc, char** argv) {
    using namespace alib6;
    Command cmd;
    cmd.register_toggle({.name = "json", .short_name = "-j", .long_name = "--json",
                         .description = "Emit machine-readable JSON"});
    cmd.add_route("version", [](const Command::CommandInput&) -> Command::CommandOutput {
        std::println("mo-linux 0.0.1");
        return Command::CommandOutput::with_code(0);
    });
    auto outs = cmd.from_args(argc, const_cast<const char**>(argv));
    int code = 0;
    for (auto& o : outs) { (void)o; }
    return code;
}

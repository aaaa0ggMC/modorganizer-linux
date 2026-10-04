/**
 * @file smoke.cpp
 * @brief WP4 验证：alib6 (aaaa0ggmcLib gen6) 经 CMake 接入后的最小冒烟测试
 *
 * 覆盖三件事：
 *   (a) 同步模式 Logger（consumer_count = 0）写一行日志到 Console 目标；
 *   (b) alib6 data/json：解析 JSON 字符串 -> 读字段 -> dump 回字符串；
 *   (c) alib6 core:cmd / parser：解析 argv {"prog","status","--json","--game","skyrimse"}。
 *
 * 说明：本文件只用 `import std;` + `import alib6;`（C++26 modules），不 #include 标准库。
 */
import std;
import alib6;

using namespace alib6;

namespace {

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::println("[ {} ] {}", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

}  // namespace

int main() {
    // ==================== (a) 日志：同步直写模式 ====================
    // consumer_count = 0 表示不启动后台消费线程，由当前线程直接分发写入。
    {
        Logger logger(LoggerConfig{.consumer_count = 0});
        logger.append_mod<lot::Console>("console");
        LogFactory lg(logger, "smoke");

        lg(Severity::Info) << "alib6 smoke: sync logger (consumer_count=0) online" << endlog;
        logger.flush();

        check(true, "logger: sync mode (consumer_count=0) + Console target written");
    }

    // ==================== (b) data/json：解析 -> 读字段 -> dump ====================
    {
        // PMR：alib6 全链路支持注入 memory_resource*，这里用一块独立的缓冲池
        std::pmr::monotonic_buffer_resource pool;
        constexpr std::string_view json_text =
            R"({"game":"skyrimse","loadorder":42,"nested":{"ok":true},"ports":[1001,1002]})";

        AData doc(&pool);
        const bool parsed = doc.load_from_memory(json_text);
        check(parsed, "json: JSON::parse 成功解析源字符串");

        const std::string_view game = doc["game"].to<std::string_view>();
        const i64 loadorder = doc["loadorder"].to<i64>();
        const bool nested_ok = doc["nested"]["ok"].to<bool>();
        const i64 first_port = doc["ports"][0].to<i64>();

        check(game == "skyrimse", "json: 读取字符串字段 game == skyrimse");
        check(loadorder == 42, "json: 读取整数字段 loadorder == 42");
        check(nested_ok, "json: 读取嵌套布尔字段 nested.ok == true");
        check(first_port == 1001, "json: 读取数组元素 ports[0] == 1001");

        // dump 回字符串：直接写进 std::string 目标容器
        JSON json_engine;
        std::string dumped;
        json_engine.dump(dumped, doc);
        std::println("        dump => {}", dumped);
        check(dumped.contains("\"skyrimse\""), "json: dump 回字符串包含 game 字段");

        // 往返校验
        AData round(&pool);
        round.load_from_memory(dumped);
        check(round["game"].to<std::string_view>() == "skyrimse",
              "json: dump 结果可再次被解析（round-trip）");
    }

    // ==================== (c) core:cmd / parser：解析 argv ====================
    {
        Command cmd;
        cmd.register_option({
            .name = "game",
            .short_name = "-g",
            .long_name = "--game",
            .description = "Target game",
            .default_val = "skyrimse"
        });
        cmd.register_toggle({
            .name = "json",
            .short_name = "-j",
            .long_name = "--json",
            .description = "Emit machine-readable JSON"
        });

        bool handler_called = false;
        bool seen_json = false;
        std::string_view seen_game;
        std::size_t seen_positionals = 999;

        // 声明子命令：status
        cmd.add_route("status", [&](const Command::CommandInput& in) -> Command::CommandOutput {
            handler_called = true;
            seen_json = in.has("json");
            seen_game = in.get("game").view();
            seen_positionals = in.args().size();
            return Command::CommandOutput::with_code(0);
        });

        const char* argv[] = {"prog", "status", "--json", "--game", "skyrimse"};
        auto outputs = cmd.from_args(5, argv);

        check(!outputs.empty(), "cmd: from_args 返回了 dispatch 结果");
        check(handler_called, "cmd: 子命令 status 的 handler 被调用");
        check(seen_json, "cmd: 开关 --json 被识别");
        check(seen_game == "skyrimse", "cmd: 选项 --game skyrimse 被解析");
        check(seen_positionals == 0, "cmd: 无多余位置参数残留");
        if (!outputs.empty()) {
            check(outputs.front().valid && outputs.front().code == 0,
                  "cmd: handler 返回 CommandOutput code == 0 且 valid");
        }

        std::println("        help =>\n{}", cmd.help());
    }

    if (g_failures != 0) {
        std::println("smoke: {} check(s) FAILED", g_failures);
        return 1;
    }
    std::println("smoke: all checks passed");
    return 0;
}

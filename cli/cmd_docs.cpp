// mo-linux docs [TOPIC]：构建时用 #embed 把文档编进二进制，装好的单个可执行文件就能看完整文档
// （不需要源码树；Agent 也可以 `mo-linux docs agent` 读到同一份说明）。
// 改了 docs/*.md 只需重新编译本文件（CMake 里登记了 OBJECT_DEPENDS）。
// 混用约束：所有 #include 在 import 之前（详见 cli/cmd_common.hpp 文件头）。
#include "commands.hpp"

import alib6;
import std;

namespace cli {
namespace {

constexpr unsigned char kGuide[] = {
#embed "../docs/GUIDE.md"
};
constexpr unsigned char kAgent[] = {
#embed "../docs/AGENT.md"
};
constexpr unsigned char kCli[] = {
#embed "../docs/CLI.md"
};
constexpr unsigned char kHandbook[] = {
#embed "../HANDBOOK.md"
};
constexpr unsigned char kReadme[] = {
#embed "../README.md"
};

struct Doc {
    std::string_view name;
    std::string_view file;
    std::string_view title;
    std::span<const unsigned char> bytes;
};
constexpr Doc kDocs[] = {
    {"guide", "docs/GUIDE.md", "操作指南：安装、日常使用、集合、工具运行（COW）、降级、排错", kGuide},
    {"agent", "docs/AGENT.md", "Agent guide: protocol, pending kinds, what needs the user", kAgent},
    {"cli", "docs/CLI.md", "CLI 规格：每个命令的参数、JSON data、退出码、错误码", kCli},
    {"handbook", "HANDBOOK.md", "设计手册：架构、决策、验证状态、风险", kHandbook},
    {"readme", "README.md", "项目简介", kReadme},
};

std::string_view text_of(const Doc& d) {
    return {reinterpret_cast<const char*>(d.bytes.data()), d.bytes.size()};
}

}  // namespace

Result run_docs(Context& ctx) {
    DocsData d{.topics = std::pmr::vector<DocTopicRow>(ctx.mem),
               .topic = mol::string("", ctx.mem),
               .markdown = mol::string("", ctx.mem)};
    for (const auto& doc : kDocs)
        d.topics.push_back(DocTopicRow{.name = mol::string(doc.name, ctx.mem),
                                       .file = mol::string(doc.file, ctx.mem),
                                       .title = mol::string(doc.title, ctx.mem),
                                       .bytes = static_cast<std::int64_t>(doc.bytes.size())});
    if (!ctx.args.positionals.empty()) {
        std::string want(ctx.args.positionals.front());
        for (auto& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (want.ends_with(".md")) want.resize(want.size() - 3);
        const Doc* hit = nullptr;
        for (const auto& doc : kDocs)
            if (doc.name == want || doc.file == want) hit = &doc;
        if (!hit) {
            std::string names;
            for (const auto& doc : kDocs) names += (names.empty() ? "" : ", ") + std::string(doc.name);
            return make_usage_error("docs: unknown topic '" + std::string(ctx.args.positionals.front()) + "' (one of: " + names + ")", ctx);
        }
        d.topic = mol::string(hit->name, ctx.mem);
        d.markdown = mol::string(text_of(*hit), ctx.mem);
    }
    Result r(ctx.mem);
    r.ok = true;
    r.exit_code = 0;
    r.command = ctx.command;
    r.set_data(std::move(d));
    return r;
}

}  // namespace cli

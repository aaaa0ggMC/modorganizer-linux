// CLI envelope 序列化（alib6 反射）与文本渲染的单测。
//
// 混用约束（见 cli/cmd_common.hpp）：所有 #include 在 import 之前。
// 直接 #include 被测的 .cpp（根 CMake 的 test_cli_* 目标已链接 mol_alib6）。
#include <string>

#include "minitest.hpp"
#include "../cli/args.cpp"
#include "../cli/cmd_common.cpp"
#include "../cli/output.cpp"

import alib6;
import std;

namespace {

using namespace cli;

std::pmr::monotonic_buffer_resource& arena() {
    static std::pmr::monotonic_buffer_resource pool;
    return pool;
}

mol::mr* pool_mem() { return &arena(); }

Result make_version_result() {
    Result r(pool_mem());
    r.ok = true;
    r.exit_code = 0;
    r.command = "version";
    r.set_data(VersionData{
        .name = mol::string("mo-linux", pool_mem()),
        .version = mol::string("0.0.1", pool_mem()),
    });
    return r;
}

std::string serialize(const Result& r) {
    return std::string(serialize_envelope(r, pool_mem()));
}

}  // namespace

// ---------------------------------------------------------------------------
TEST(envelope_success_is_compact_and_key_sorted) {
    const std::string out = serialize(make_version_result());
    CHECK_EQ(out,
             std::string("{\"command\":\"version\",\"data\":{\"name\":\"mo-linux\",\"version\":"
                         "\"0.0.1\"},\"errors\":[],\"ok\":true,\"schema_version\":1,\"warnings\":[]}"));
}

TEST(envelope_failure_has_null_data_and_errors) {
    Result r(pool_mem());
    r.ok = false;
    r.exit_code = 1;
    r.command = "apply";
    r.data.set_null();
    r.add_error("farm_not_owned", "root is not empty", "/tmp/farm");
    const std::string out = serialize(r);
    CHECK_EQ(out,
             std::string("{\"command\":\"apply\",\"data\":null,\"errors\":[{\"code\":"
                         "\"farm_not_owned\",\"message\":\"root is not empty\",\"path\":"
                         "\"/tmp/farm\"}],\"ok\":false,\"schema_version\":1,\"warnings\":[]}"));
}

TEST(envelope_warnings_are_serialized) {
    Result r(pool_mem());
    r.ok = true;
    r.exit_code = 0;
    r.command = "plan";
    r.set_data(PlanData{});
    r.add_warning("merge_warning", "kind mismatch", "Data/a.dds");
    const std::string out = serialize(r);
    CHECK(out.find("\"warnings\":[{\"code\":\"merge_warning\",\"message\":\"kind mismatch\",\"path\":"
                   "\"Data/a.dds\"}]") != std::string::npos);
    // ok=true 时 data 必须是对象（这里是空 plan）
    CHECK(out.find("\"data\":{") != std::string::npos);
}

TEST(nested_pmr_vector_of_struct_round_trips_through_reflection) {
    Result r(pool_mem());
    r.ok = true;
    r.command = "conflicts";
    mol::mr* mem = pool_mem();
    ConflictsData d{
        .conflicts = mol::vector<ConflictRow>(mem),
        .count = 1,
    };
    ConflictRow row{
        .path = mol::string("Data/a.dds", mem),
        .winner = mol::string("ModB", mem),
        .losers = mol::vector<mol::string>(mem),
    };
    row.losers.push_back(mol::string("ModA", mem));
    d.conflicts.push_back(std::move(row));
    r.set_data(d);

    const std::string out = serialize(r);
    CHECK(out.find("\"conflicts\":[{\"losers\":[\"ModA\"],\"path\":\"Data/a.dds\","
                   "\"winner\":\"ModB\"}]") != std::string::npos);
    CHECK(out.find("\"count\":1") != std::string::npos);
}

TEST(exit_code_mapping) {
    CHECK_EQ(exit_code_from_issues(mol::vector<Err>{}), 0);
    mol::vector<Err> one{mol::allocator_type(pool_mem())};
    one.push_back(Err{mol::string("invalid_argument", pool_mem()),
                      mol::string("bad", pool_mem()),
                      mol::string("", pool_mem())});
    CHECK_EQ(exit_code_from_issues(one), 2);
    one.clear();
    one.push_back(Err{mol::string("farm_not_owned", pool_mem()),
                      mol::string("boom", pool_mem()),
                      mol::string("", pool_mem())});
    CHECK_EQ(exit_code_from_issues(one), 1);
}

// ---------------------------------------------------------------------------
// 文本渲染：不含 JSON，一行摘要
// ---------------------------------------------------------------------------
TEST(text_render_version_has_no_json) {
    const auto lines = render_text(make_version_result(), pool_mem());
    CHECK_EQ(lines.size(), std::size_t{1});
    CHECK_EQ(std::string(lines.front()), std::string("mo-linux 0.0.1"));
    for (const auto& l : lines) {
        CHECK(l.find('{') == mol::string::npos);
        CHECK(l.find('"') == mol::string::npos);
    }
}

TEST(text_render_failure_is_empty_on_stdout) {
    Result r(pool_mem());
    r.ok = false;
    r.command = "mods list";
    r.add_error("instance_not_found", "nope", "/x");
    const auto lines = render_text(r, pool_mem());
    CHECK(lines.empty());
    // 诊断走 render_diagnostics（stderr）
    const auto diag = render_diagnostics(r, pool_mem());
    CHECK_EQ(diag.size(), std::size_t{1});
    CHECK(std::string(diag.front()).find("instance_not_found") != std::string::npos);
}

TEST(text_render_plan_summary) {
    Result r(pool_mem());
    r.ok = true;
    r.command = "plan";
    mol::mr* mem = pool_mem();
    PlanData d{
        .ops = mol::vector<OpRow>(mem),
        .count = 2,
        .counts = OpCounts{.mkdir = 1, .link = 1},
        .warnings = 1,
    };
    d.ops.push_back(OpRow{.kind = mol::string("mkdir", mem), .path = mol::string("Data", mem),
                          .target = mol::string("", mem)});
    d.ops.push_back(OpRow{.kind = mol::string("link", mem), .path = mol::string("Data/a.dds", mem),
                          .target = mol::string("/mods/A/Data/a.dds", mem)});
    r.set_data(d);
    const auto lines = render_text(r, mem);
    CHECK(!lines.empty());
    CHECK_EQ(std::string(lines.front()),
             std::string("plan: 2 op(s) [mkdir=1 link=1 relink=0 remove=0 rmdir=0], warnings=1"));
    CHECK_EQ(lines.size(), std::size_t{3});  // 摘要 + 2 条 op
}

TEST(text_render_status_drift_and_sync) {
    mol::mr* mem = pool_mem();
    {
        Result r(mem);
        r.ok = true;
        r.command = "status";
        r.set_data(StatusData{.in_sync = false,
                              .pending = 3,
                              .farm_path = mol::string("/inst/farm", mem),
                              .farm_exists = false});
        const auto lines = render_text(r, mem);
        CHECK_EQ(std::string(lines.front()), std::string("drift: 3 pending op(s)"));
    }
    {
        Result r(mem);
        r.ok = true;
        r.command = "status";
        r.set_data(StatusData{.in_sync = true,
                              .pending = 0,
                              .farm_path = mol::string("/inst/farm", mem),
                              .farm_exists = true});
        const auto lines = render_text(r, mem);
        CHECK_EQ(std::string(lines.front()), std::string("in sync"));
    }
}

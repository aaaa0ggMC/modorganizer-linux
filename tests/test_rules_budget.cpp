#include "minitest.hpp"
#include "mol/rules.hpp"
using namespace mol::rules;
TEST(native_pattern_and_finalizer_escape_are_closed) {
    Context ctx;
    Source sources[] = {{"safe", R"(return {api_version=1,id='safe',check=function(c)
        assert(string.match==nil and string.gsub==nil and string.gmatch==nil)
        assert(setmetatable==nil and getmetatable==nil and pcall==nil and xpcall==nil)
        assert(string.find('abc','b',1,true)==2)
        return {{id='ok',level='ok',message='bounded'}} end})"}};
    auto rows = evaluate(sources, ctx);
    CHECK_EQ(rows.size(), 1u);
    CHECK(rows[0].id == "lua.safe.ok");
}
TEST(top_level_oom_does_not_abort_later_sources) {
    Context ctx;
    Source sources[] = {
        {"oom", "local t={} while true do t[#t+1]=string.rep('x',10000) end"},
        {"good", "return {api_version=1,id='good',check=function(c) return {{id='ok',level='ok',message='ok'}} end}"}};
    auto rows = evaluate(sources, ctx);
    CHECK_EQ(rows.size(), 2u);
    CHECK(rows[0].id == "lua.runtime");
    CHECK(rows[1].id == "lua.good.ok");
}

TEST(large_pack_fits_default_budget) {
    Context ctx;
    ctx.game = "skyrimse";
    for (int i = 0; i < 3000; ++i) {
        mol::ModInfo m;
        m.name = "ordinary mod " + std::to_string(i);
        m.enabled = true;
        m.exists = true;
        ctx.mods.push_back(std::move(m));
    }
    auto rows = evaluate(builtin_sources(), ctx);
    for (auto &r : rows)
        CHECK(r.id != "lua.runtime");
}
TEST(one_mod_matching_two_option_labels_is_not_a_conflict) {
    Context ctx;
    ctx.game = "skyrimse";
    mol::ModInfo m;
    m.name = "ENB Performance Ultra comparison";
    m.exists = true;
    m.enabled = true;
    ctx.mods.push_back(std::move(m));
    auto rows = evaluate(builtin_sources(), ctx);
    CHECK(rows.empty());
}
TEST(check_output_requires_a_dense_array) {
    Context ctx;
    Source source{
        "dictionary",
        "return {api_version=1,id='bad',check=function(c) return {key={id='a',level='warn',message='bad'}} end}"};
    auto rows = evaluate(std::span(&source, 1), ctx);
    CHECK_EQ(rows.size(), 1u);
    CHECK(rows[0].id == "lua.runtime");
}

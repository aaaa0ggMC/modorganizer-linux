#include "mol/rules.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sol/sol.hpp>
#include <stdexcept>
#include <string>
namespace mol::rules {
namespace {
struct Budget {
    std::size_t used = 0, limit;
    int ticks;
    bool exhausted = false;
};
void *alloc(void *ud, void *ptr, std::size_t old, std::size_t size) {
    auto &b = *static_cast<Budget *>(ud);
    if (!ptr)
        old = 0;
    if (!size) {
        b.used -= old;
        std::free(ptr);
        return nullptr;
    }
    if (size > old && size - old > b.limit - std::min(b.used, b.limit))
        return nullptr;
    auto *p = std::realloc(ptr, size);
    if (p)
        b.used = b.used - old + size;
    return p;
}
void hook(lua_State *L, lua_Debug *) {
    auto *b = *static_cast<Budget **>(lua_getextraspace(L));
    if (b->exhausted || --b->ticks <= 0) {
        b->exhausted = true;
        luaL_error(L, "instruction budget exhausted");
    }
}
int literal_find(lua_State *L) {
    std::size_t n = 0, pn = 0;
    const char *s = luaL_checklstring(L, 1, &n);
    const char *p = luaL_checklstring(L, 2, &pn);
    if (!lua_toboolean(L, 4))
        return luaL_error(L, "string.find requires plain=true");
    auto start = luaL_optinteger(L, 3, 1);
    if (start < 0)
        start = std::max<lua_Integer>(1, static_cast<lua_Integer>(n) + start + 1);
    if (pn > 4096 || n > 128 * 1024)
        return luaL_error(L, "literal search size limit exceeded");
    // Linear search avoids adversarial quadratic work in an uninterruptible C call.
    std::array<std::size_t, 4096> prefix{};
    for (std::size_t i = 1, j = 0; i < pn; ++i) {
        while (j && p[i] != p[j])
            j = prefix[j - 1];
        if (p[i] == p[j])
            ++j;
        prefix[i] = j;
    }
    auto begin = static_cast<std::size_t>(std::max<lua_Integer>(1, start) - 1);
    auto pos = std::string_view::npos;
    if (!pn && begin <= n)
        pos = begin;
    for (std::size_t i = begin, j = 0; pn && i < n; ++i) {
        while (j && s[i] != p[j])
            j = prefix[j - 1];
        if (s[i] == p[j])
            ++j;
        if (j == pn) {
            pos = i + 1 - pn;
            break;
        }
    }
    if (pos == std::string_view::npos) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, pos + 1);
    lua_pushinteger(L, pos + pn);
    return 2;
}
int bounded_table(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    if (lua_rawlen(L, 1) > 16384)
        return luaL_error(L, "table sequence limit exceeded");
    int argc = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, argc, LUA_MULTRET);
    return lua_gettop(L);
}
struct State {
    Budget budget;
    lua_State *L;
    explicit State(Limits lim)
        : budget{0, std::clamp(lim.memory_bytes, std::size_t(256 * 1024), std::size_t(32 * 1024 * 1024)),
                 std::max(1, lim.instructions / 1000)},
          L(lua_newstate(alloc, &budget)) {
        if (!L)
            throw std::runtime_error("cannot allocate Lua state");
        *static_cast<Budget **>(lua_getextraspace(L)) = &budget;
        // Protected-call functions could catch budget exceptions indefinitely.
        luaL_requiref(L, "_G", luaopen_base, 1);
        lua_pop(L, 1);
        for (auto n : {"dofile", "loadfile", "load", "collectgarbage", "pcall", "xpcall", "print", "warn",
                       "setmetatable", "getmetatable"}) {
            lua_pushnil(L);
            lua_setglobal(L, n);
        }
        luaL_requiref(L, "string", luaopen_string, 1);
        // Lua patterns run in native C and are not interruptible by instruction hooks.
        for (auto n : {"match", "gmatch", "gsub", "dump", "format"}) {
            lua_pushnil(L);
            lua_setfield(L, -2, n);
        }
        lua_pushcfunction(L, literal_find);
        lua_setfield(L, -2, "find");
        lua_pop(L, 1);
        luaL_requiref(L, "table", luaopen_table, 1);
        lua_pushnil(L);
        lua_setfield(L, -2, "move");
        for (auto n : {"sort", "insert", "remove", "concat", "unpack"}) {
            lua_getfield(L, -1, n);
            lua_pushcclosure(L, bounded_table, 1);
            lua_setfield(L, -2, n);
        }
        lua_pop(L, 1);
        luaL_requiref(L, "math", luaopen_math, 1);
        lua_pop(L, 1);
        lua_sethook(L, hook, LUA_MASKCOUNT, 1000);
    }
    ~State() { lua_close(L); }
    void finish() {
        lua_sethook(L, nullptr, 0, 0);
        budget.limit = SIZE_MAX;
    }
};
bool identifier(const std::string &s) {
    return !s.empty() && s.size() <= 128 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
               c == '-';
    });
}
std::string str(const sol::table &t, const char *key, bool optional = false) {
    auto o = t.raw_get<sol::object>(key);
    if (optional && !o.valid())
        return {};
    if (o.get_type() != sol::type::string)
        throw std::runtime_error("invalid text field");
    auto s = o.as<std::string>();
    if (s.size() > 4096 || s.find('\0') != std::string::npos)
        throw std::runtime_error("oversized text field");
    return s;
}
sol::table plain(sol::object o) {
    if (o.get_type() != sol::type::table)
        throw std::runtime_error("expected table");
    auto t = o.as<sol::table>();
    if (t[sol::metatable_key].valid())
        throw std::runtime_error("metatables are not supported");
    return t;
}
sol::table definition(sol::state_view &lua, Source source, Limits lim) {
    if (source.text.size() > lim.source_bytes || source.text.empty() || source.text.front() == '\x1b')
        throw std::runtime_error("invalid source");
    auto loaded = lua.load(source.text, "rule", sol::load_mode::text);
    if (!loaded.valid())
        throw std::runtime_error("invalid Lua source");
    sol::protected_function fn = loaded;
    auto result = fn();
    if (!result.valid())
        throw std::runtime_error("Lua evaluation failed");
    lua_sethook(lua.lua_state(), nullptr, 0, 0);
    (*static_cast<Budget **>(lua_getextraspace(lua.lua_state())))->limit = SIZE_MAX;
    auto t = plain(result.get<sol::object>());
    auto v = t.raw_get<sol::object>("api_version");
    if (v.get_type() != sol::type::number || v.as<double>() != 1)
        throw std::runtime_error("unsupported API version");
    return t;
}
// Populate inputs under lua_pcall so allocator exhaustion is a protected Lua failure.
int context(lua_State *L) {
    auto *ctx = static_cast<const Context *>(lua_touserdata(L, 1));
    lua_newtable(L);
    auto field = [&](const char *k, std::string_view v) {
        lua_pushlstring(L, v.data(), v.size());
        lua_setfield(L, -2, k);
    };
    field("game", ctx->game);
    field("game_version", ctx->game_version);
    lua_newtable(L);
    for (auto &f : ctx->facts) {
        lua_pushlstring(L, f.key.data(), f.key.size());
        lua_pushlstring(L, f.value.data(), f.value.size());
        lua_rawset(L, -3);
    }
    lua_setfield(L, -2, "facts");
    lua_newtable(L);
    int n = 0;
    for (auto &m : ctx->mods) {
        lua_newtable(L);
        field("name", m.name);
        field("version", m.version);
        lua_pushboolean(L, m.enabled);
        lua_setfield(L, -2, "enabled");
        lua_pushboolean(L, m.exists);
        lua_setfield(L, -2, "exists");
        lua_pushinteger(L, m.nexus_id);
        lua_setfield(L, -2, "nexus_id");
        lua_rawseti(L, -2, ++n);
    }
    lua_setfield(L, -2, "mods");
    // impact：启用且存在的 mod 的影响面（数组）
    lua_newtable(L);
    {
        int n = 0;
        for (const auto& im : ctx->impact) {
            lua_newtable(L);
            auto field_s = [&](const char* k, std::string_view v) {
                lua_pushlstring(L, v.data(), v.size());
                lua_setfield(L, -2, k);
            };
            field_s("mod", std::string_view(im.mod.data(), im.mod.size()));
            field_s("summary", std::string_view(im.summary.data(), im.summary.size()));
            lua_pushboolean(L, im.packed_suspect);
            lua_setfield(L, -2, "packed_suspect");
            lua_newtable(L);
            int k = 0;
            for (const auto& i : im.injections) {
                lua_newtable(L);
                field_s("kind", i.kind);
                field_s("path", i.path);
                field_s("loaded_by", i.loaded_by);
                field_s("reach", i.reach);
                lua_rawseti(L, -2, ++k);
            }
            lua_setfield(L, -2, "injections");
            lua_newtable(L);
            const auto& c = im.caps;
            lua_pushboolean(L, c.writes_files);       lua_setfield(L, -2, "writes_files");
            lua_pushboolean(L, c.spawns_processes);   lua_setfield(L, -2, "spawns_processes");
            lua_pushboolean(L, c.network);            lua_setfield(L, -2, "network");
            lua_pushboolean(L, c.registry);           lua_setfield(L, -2, "registry");
            lua_pushboolean(L, c.memory_patch);       lua_setfield(L, -2, "memory_patch");
            lua_pushboolean(L, c.chain_loads);        lua_setfield(L, -2, "chain_loads");
            lua_pushboolean(L, c.unknown);            lua_setfield(L, -2, "unknown");
            lua_setfield(L, -2, "caps");
            lua_rawseti(L, -2, ++n);
        }
    }
    lua_setfield(L, -2, "impact");
    return 1;
}
void failure(vector<Check> &out, std::string_view name = {}) {
    Check c(out.get_allocator());
    c.id = "lua.runtime";
    c.level = "warn";
    c.message = "A Lua rule failed validation or exceeded its resource budget";
    c.hint = "Check the rule API/schema and resource limits; other rules continue independently";
    if (identifier(std::string(name))) {
        c.hint += ": ";
        c.hint += name;
    }
    out.push_back(std::move(c));
}
} // namespace
vector<Check> evaluate(std::span<const Source> sources, const Context &ctx, Limits lim, mr *mem) {
    vector<Check> out(mem);
    for (auto source : sources) {
        try {
            State state(lim);
            sol::state_view lua(state.L);
            auto def = definition(lua, source, lim);
            auto rid = str(def, "id");
            if (!identifier(rid))
                throw std::runtime_error("id");
            auto fnobj = def.raw_get<sol::object>("check");
            if (fnobj.get_type() != sol::type::function)
                throw std::runtime_error("check");
            state.budget.limit = std::clamp(lim.memory_bytes, std::size_t(256 * 1024), std::size_t(32 * 1024 * 1024));
            lua_sethook(state.L, hook, LUA_MASKCOUNT, 1000);
            lua_pushcfunction(state.L, context);
            lua_pushlightuserdata(state.L, const_cast<Context *>(&ctx));
            if (lua_pcall(state.L, 1, 1, 0) != LUA_OK)
                throw std::runtime_error("context budget");
            state.finish(); // Host bookkeeping must not panic outside a protected Lua call.
            sol::table input = sol::stack::get<sol::table>(state.L, -1);
            lua_pop(state.L, 1);
            sol::protected_function fn = fnobj;
            state.budget.limit = std::clamp(lim.memory_bytes, std::size_t(256 * 1024), std::size_t(32 * 1024 * 1024));
            lua_sethook(state.L, hook, LUA_MASKCOUNT, 1000);
            auto res = fn(input);
            if (!res.valid() || state.budget.exhausted)
                throw std::runtime_error("rule budget");
            state.finish();
            auto rows = plain(res.get<sol::object>());
            vector<Check> pending(mem);
            const auto count = rows.size();
            if (count > lim.checks)
                throw std::runtime_error("check limit");
            std::size_t entries = 0;
            for (auto &&pair : rows) {
                if (++entries > count || pair.first.get_type() != sol::type::number)
                    throw std::runtime_error("checks must be a dense array");
                auto key = pair.first.as<double>();
                if (key < 1 || key > count || key != static_cast<double>(static_cast<std::size_t>(key)))
                    throw std::runtime_error("invalid check index");
            }
            if (entries != count)
                throw std::runtime_error("sparse checks");
            for (std::size_t index = 1; index <= count; ++index) {
                auto row = plain(rows.raw_get<sol::object>(index));
                Check c(mem);
                auto id = str(row, "id");
                if (!identifier(id))
                    throw std::runtime_error("id");
                c.id = "lua." + rid + "." + id;
                c.level = str(row, "level");
                c.message = str(row, "message");
                c.hint = str(row, "hint", true);
                if (c.level != "ok" && c.level != "warn" && c.level != "error")
                    throw std::runtime_error("level");
                auto action = row.raw_get<sol::object>("action");
                if (action.valid()) {
                    auto a = plain(action);
                    auto type = str(a, "type"), name = str(a, "name");
                    bool allowed = std::any_of(ctx.facts.begin(), ctx.facts.end(), [](auto &f) {
                        return f.key == "preferences.allow_disable_mod" && f.value == "true";
                    });
                    bool exists = std::any_of(ctx.mods.begin(), ctx.mods.end(), [&](auto &m) {
                        return std::string_view(m.name) == name && m.enabled && m.exists && !m.separator;
                    });
                    if (type != "disable_mod" || !allowed || !exists || name.starts_with('-') ||
                        name.find('\n') != std::string::npos)
                        throw std::runtime_error("action");
                    c.fix.emplace_back("mods");
                    c.fix.emplace_back("disable");
                    c.fix.emplace_back(name);
                }
                pending.push_back(std::move(c));
            }
            for (auto &c : pending)
                out.push_back(std::move(c));
        } catch (const std::exception &) {
            failure(out, source.name);
        }
    }
    return out;
}
GameDescriptor describe(const Source &source, Limits lim, mr *mem) {
    State state(lim);
    sol::state_view lua(state.L);
    auto def = definition(lua, source, lim);
    state.finish();
    GameDescriptor d(mem);
    d.id = str(def, "id");
    d.executable = str(def, "executable");
    d.data_directory = str(def, "data_directory");
    d.plugin_format = str(def, "plugin_format");
    if (!identifier(std::string(d.id)))
        throw std::runtime_error("invalid game id");
    for (auto &s : {d.executable, d.data_directory}) {
        auto p = std::filesystem::path(std::string(s));
        if (s.empty() || p.is_absolute() || s.find('\\') != string::npos || s.find(':') != string::npos)
            throw std::runtime_error("relative path required");
        for (auto &component : p)
            if (component == "..")
                throw std::runtime_error("path traversal");
    }
    if (d.plugin_format != "none" && d.plugin_format != "tes")
        throw std::runtime_error("unknown plugin format");
    return d;
}
vector<Check> run(const Instance &inst, std::string_view version, mr *mem) {
    auto ctx = collect_context(inst, version, mem);
    auto out = evaluate(builtin_sources(), ctx, {}, mem);
    std::error_code ec;
    auto dir = std::filesystem::path(std::string(inst.root)) / "rules";
    std::vector<std::filesystem::path> paths;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".lua" && it->is_regular_file(ec))
            paths.push_back(it->path());
        if (paths.size() > 32) {
            failure(out);
            return out;
        }
    }
    std::sort(paths.begin(), paths.end());
    for (auto &path : paths) {
        std::ifstream f(path, std::ios::binary);
        std::string text(128 * 1024 + 1, '\0');
        f.read(text.data(), text.size());
        text.resize(f.gcount());
        if (!f && !f.eof()) {
            failure(out);
            continue;
        }
        auto name = path.filename().string();
        Source source{name, text};
        auto more = evaluate(std::span(&source, 1), ctx, {}, mem);
        for (auto &c : more)
            out.push_back(std::move(c));
    }
    return out;
}
} // namespace mol::rules

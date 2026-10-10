#pragma once
#include "mol/doctor.hpp"
#include "mol/impact.hpp"
#include <span>
namespace mol::rules {
struct Fact {
    using allocator_type = mol::allocator_type;
    string key, value;
    explicit Fact(allocator_type a = {}) : key(a), value(a) {}
    Fact(const Fact &o, allocator_type a) : key(o.key, a), value(o.value, a) {}
    Fact(Fact &&o, allocator_type a) : key(std::move(o.key), a), value(std::move(o.value), a) {}
    Fact(const Fact &) = default;
    Fact(Fact &&) = default;
    Fact &operator=(const Fact &) = default;
    Fact &operator=(Fact &&) = default;
};
struct Context {
    using allocator_type = mol::allocator_type;
    string game, game_version;
    vector<Fact> facts;
    vector<ModInfo> mods;
    std::vector<impact::ModImpact> impact;  // 启用且存在的 mod 的影响面（带缓存）；规则只读
    explicit Context(allocator_type a = {}) : game(a), game_version(a), facts(a), mods(a) {}
    explicit Context(mr *mem) : Context(allocator_type(mem)) {}
    Context(const Context &o, allocator_type a)
        : game(o.game, a), game_version(o.game_version, a), facts(o.facts, a), mods(o.mods, a),
          impact(o.impact) {}
    Context(Context &&o, allocator_type a)
        : game(std::move(o.game), a), game_version(std::move(o.game_version), a), facts(std::move(o.facts), a),
          mods(std::move(o.mods), a), impact(std::move(o.impact)) {}
    Context(const Context &) = default;
    Context(Context &&) = default;
    Context &operator=(const Context &) = default;
    Context &operator=(Context &&) = default;
};
struct Source {
    std::string_view name, text;
};
struct Limits {
    std::size_t memory_bytes = 8 * 1024 * 1024;
    std::size_t source_bytes = 128 * 1024;
    int instructions = 2000000;
    std::size_t checks = 64;
};
// Pure evaluation: no filesystem, process, package, or arbitrary CLI access.
// Each source returns {api_version=1,id="...",check=function(ctx) return {...} end}.
// ctx={game,game_version,facts={key=value},mods={{name,enabled,exists,nexus_id,...}}}.
// Checks: {id,level,message,hint?,action?}; only action={type="disable_mod",name=...}
// with facts["preferences.allow_disable_mod"]=="true" can produce a fix argv.
vector<Check> evaluate(std::span<const Source>, const Context &, Limits = {}, mr *mem = default_mr());
Context collect_context(const Instance &, std::string_view game_version, mr *mem = default_mr());
vector<Check> run(const Instance &, std::string_view game_version, mr *mem = default_mr());
std::span<const Source> builtin_sources();
// Game descriptors do not depend on MO2. Relative paths only; execution stays in C++.
struct GameDescriptor {
    using allocator_type = mol::allocator_type;
    string id, executable, data_directory, plugin_format;
    explicit GameDescriptor(allocator_type a = {}) : id(a), executable(a), data_directory(a), plugin_format(a) {}
    explicit GameDescriptor(mr *mem) : GameDescriptor(allocator_type(mem)) {}
    GameDescriptor(const GameDescriptor &o, allocator_type a)
        : id(o.id, a), executable(o.executable, a), data_directory(o.data_directory, a),
          plugin_format(o.plugin_format, a) {}
    GameDescriptor(GameDescriptor &&o, allocator_type a)
        : id(std::move(o.id), a), executable(std::move(o.executable), a),
          data_directory(std::move(o.data_directory), a), plugin_format(std::move(o.plugin_format), a) {}
    GameDescriptor(const GameDescriptor &) = default;
    GameDescriptor(GameDescriptor &&) = default;
    GameDescriptor &operator=(const GameDescriptor &) = default;
    GameDescriptor &operator=(GameDescriptor &&) = default;
};
GameDescriptor describe(const Source &, Limits = {}, mr *mem = default_mr());
} // namespace mol::rules

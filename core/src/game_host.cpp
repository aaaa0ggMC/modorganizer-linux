#include "mol/game_host.hpp"

#include <dlfcn.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>

namespace mol {
namespace {
namespace fs = std::filesystem;

struct Api {
    void* (*create)(const char*, const char*, const char*, const char*, char**) = nullptr;
    void (*destroy)(void*) = nullptr;
    char* (*info)(void*) = nullptr;
    void (*free_)(char*) = nullptr;
};

fs::path exe_dir() {
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path{} : p.parent_path();
}
}  // namespace

string find_game_host_lib(std::string_view explicit_path, mr* mem) {
    std::vector<fs::path> cand;
    if (!explicit_path.empty()) cand.emplace_back(std::string(explicit_path));
    if (const char* e = std::getenv("MOL_GAME_LIB"); e && *e) cand.emplace_back(e);
    if (auto d = exe_dir(); !d.empty()) {
        cand.push_back(d / "libmo-game.so");
        cand.push_back(d / ".." / "lib" / "libmo-game.so");
        cand.push_back(d / "host" / "libmo-game.so");
    }
    for (const auto& c : cand) {
        std::error_code ec;
        if (fs::exists(c, ec)) return string(fs::weakly_canonical(c, ec).string(), mem);
    }
    return string(mem);
}

namespace detail {
struct HostImpl {
    void* handle = nullptr;
    Api api;
    ~HostImpl() {
        if (handle) ::dlclose(handle);
    }
};

struct GameImpl {
    std::shared_ptr<HostImpl> host;
    void* game = nullptr;
    ~GameImpl() {
        if (game && host) host->api.destroy(game);
    }
};
}  // namespace detail

}  // namespace mol

namespace mol {

GameHost::GameHost(std::unique_ptr<detail::HostImpl> i) : impl_(std::move(i)) {}
GameHost::~GameHost() = default;
GameHost::GameHost(GameHost&&) noexcept = default;
GameHost& GameHost::operator=(GameHost&&) noexcept = default;

GameHost GameHost::open(std::string_view lib_path) {
    string path = find_game_host_lib(lib_path);
    if (path.empty())
        throw Error("game_unavailable", "libmo-game.so not found (set MOL_GAME_LIB or build with MOL_BUILD_HOST=ON)");
    auto impl = std::make_unique<detail::HostImpl>();
    impl->handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!impl->handle) {
        const char* e = ::dlerror();
        throw Error("game_unavailable", std::string("dlopen failed: ") + (e ? e : "?"), std::string(path));
    }
    auto sym = [&](const char* n) {
        void* s = ::dlsym(impl->handle, n);
        if (!s) throw Error("game_unavailable", std::string("missing symbol ") + n, std::string(path));
        return s;
    };
    impl->api.create = reinterpret_cast<decltype(Api::create)>(sym("mo_game_create"));
    impl->api.destroy = reinterpret_cast<decltype(Api::destroy)>(sym("mo_game_destroy"));
    impl->api.info = reinterpret_cast<decltype(Api::info)>(sym("mo_game_info_json"));
    impl->api.free_ = reinterpret_cast<decltype(Api::free_)>(sym("mo_free"));
    return GameHost(std::move(impl));
}

Game::Game(std::unique_ptr<detail::GameImpl> i) : impl_(std::move(i)) {}
Game::~Game() = default;
Game::Game(Game&&) noexcept = default;
Game& Game::operator=(Game&&) noexcept = default;

Game GameHost::create(std::string_view id, std::string_view dir, std::string_view prefix, std::string_view user) const {
    // 注意：host 的生命周期要覆盖 Game；用 shared_ptr 共享 Impl（GameHost 自身持 unique_ptr，这里借一个非拥有的视图）。
    auto gi = std::make_unique<detail::GameImpl>();
    gi->host = std::shared_ptr<detail::HostImpl>(impl_.get(), [](detail::HostImpl*) {});  // 非拥有
    char* err = nullptr;
    const std::string sid(id), sdir(dir), spfx(prefix), susr(user);
    gi->game = impl_->api.create(sid.c_str(), sdir.empty() ? nullptr : sdir.c_str(),
                                 spfx.empty() ? nullptr : spfx.c_str(), susr.c_str(), &err);
    if (!gi->game) {
        std::string msg = err ? err : "unknown error";
        if (err) impl_->api.free_(err);
        throw Error("game_unavailable", msg);
    }
    return Game(std::move(gi));
}

string Game::info_json(mr* mem) const {
    char* p = impl_->host->api.info(impl_->game);
    if (!p) return string(mem);
    string s(p, mem);
    impl_->host->api.free_(p);
    return s;
}

}  // namespace mol

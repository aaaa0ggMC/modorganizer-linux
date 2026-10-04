// mo-linux CLI 的纯逻辑部分：路径工具 + ParsedArgs 访问器。
// 选项解析已全部交给 alib6 Command（见 cli/parse.cpp 的 CommandInput 适配器）。
#include "args.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace cli {

namespace {

std::string normalize_unix(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

}  // namespace

mol::string abs_path(std::string_view p, mol::mr* mem) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path in{std::string(p)};
    fs::path abs = in.is_absolute() ? in : fs::absolute(in, ec);
    if (ec) abs = in;
    std::string s = abs.lexically_normal().generic_string();
    if (s.empty()) s = ".";
    return mol::string(normalize_unix(std::move(s)), mem);
}

mol::string resolve_instance_dir(const GlobalOptions& g, mol::mr* mem) {
    if (g.has_instance) return abs_path(g.instance, mem);
    if (const char* env = std::getenv("MOL_INSTANCE"); env != nullptr && *env != '\0') {
        return abs_path(env, mem);
    }
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    if (ec) return mol::string(".", mem);
    return mol::string(normalize_unix(cwd.generic_string()), mem);
}

bool path_exists(std::string_view p) {
    struct stat st {};
    return ::stat(std::string(p).c_str(), &st) == 0;
}

// ---------------------------------------------------------------------------
// ParsedArgs 访问器
// ---------------------------------------------------------------------------
bool ParsedArgs::has(std::string_view name) const {
    for (const auto& [key, value] : options) {
        if (key == name) return true;
    }
    return false;
}

mol::string ParsedArgs::get(std::string_view name, std::string_view def, mol::mr* mem) const {
    for (const auto& [key, value] : options) {
        if (key == name) return mol::string(value, mem);
    }
    return mol::string(def, mem);
}

bool ParsedArgs::get_bool(std::string_view name, bool def) const {
    for (const auto& [key, value] : options) {
        if (key == name) return !value.empty() && value != "0" && value != "false";
    }
    return def;
}

}  // namespace cli

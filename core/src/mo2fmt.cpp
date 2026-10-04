// MO2 实例目录磁盘格式的解析/写出（纯 C++，不依赖 Qt）。
//
// 实现约定（见 mol/mo2fmt.hpp 头注释）：
//  * 公共 API 一律 mol::string / mol::vector（std::pmr），结果用末参数 mem 分配；
//  * 内部临时对象走栈上 std::pmr::monotonic_buffer_resource（上游 mem 兜底），
//    只有放进返回值的字符串才从 mem 分配；
//  * 行解析一律用 std::string_view 切片，仅在放结果时构造 mol::string。
#include "mol/mo2fmt.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace mol {
namespace {

// ---------------------------------------------------------------------------
// 内部小工具：ASCII 判定/折叠、行切分、文件 IO。
// WP1 负责 mol/casefold.hpp；这里为避免跨 WP 依赖，自带一个 static ASCII 折叠。
// ---------------------------------------------------------------------------
inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
inline bool ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
inline bool ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r' || c == '\n';
}

// ASCII 小写化，非 ASCII 字节原样。
void ascii_fold_into(std::string_view s, string& out) {
    out.clear();
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(ascii_lower(c));
    }
}

std::string_view strip_bom(std::string_view s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) {
        s.remove_prefix(3);
    }
    return s;
}

std::string_view chomp(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    return line;
}

std::string_view trim_ws(std::string_view s) {
    while (!s.empty() && ascii_space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && ascii_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

// UTF-8 字节原样转为系统路径（仅在系统调用边界出现）。
std::filesystem::path fs_path(std::string_view file) {
    return std::filesystem::path(file.begin(), file.end());
}

// 整文件读入 out。文件不存在 → out 清空；其它 IO 错误 → runtime_error。
void load_file(std::string_view file, string& out) {
    std::ifstream in(fs_path(file), std::ios::binary);
    if (!in.is_open()) {
        out.clear();
        return;
    }
    if (in.seekg(0, std::ios::end)) {
        const std::streamoff n = in.tellg();
        if (n > 0) {
            out.reserve(static_cast<std::size_t>(n));
        }
        in.seekg(0, std::ios::beg);
    }
    in.clear();
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        throw std::runtime_error("mo2fmt: I/O error while reading file");
    }
}

// 原子写：同目录临时文件 + rename。
template <class Body>
void atomic_write(std::string_view file, Body&& body) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto target = fs_path(file);
    auto tmp = target;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("mo2fmt: failed to create temporary file");
        }
        body(out);
        out.close();
        if (out.fail()) {
            fs::remove(tmp, ec);
            throw std::runtime_error("mo2fmt: failed to write file");
        }
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw std::runtime_error("mo2fmt: failed to rename temporary file: " + ec.message());
    }
}

template <class LineFn>
void for_each_line(std::string_view text, LineFn&& fn) {
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t j = text.find('\n', i);
        const std::string_view raw =
            (j == std::string_view::npos) ? text.substr(i) : text.substr(i, j - i);
        fn(raw);
        if (j == std::string_view::npos) {
            break;
        }
        i = j + 1;
    }
}

// 栈上 monotonic buffer：读文件与行切分的所有临时对象都从这里分配，
// 上游是调用方的 mem（不会悄悄落到全局 new）。
class ScratchBuf {
public:
    explicit ScratchBuf(mr* upstream)
        : buffer_(), mono_(buffer_.data(), buffer_.size(), upstream), text_(&mono_) {}

    string& text() { return text_; }
    mr* scratch() { return &mono_; }

private:
    static constexpr std::size_t kStackBytes = 4096;
    std::array<std::byte, kStackBytes> buffer_;
    std::pmr::monotonic_buffer_resource mono_;
    string text_;
};

// 短名字用的栈上暂存（折叠后的区段/键名），同样不落全局 new。
class FoldBuf {
public:
    explicit FoldBuf(mr* upstream)
        : buffer_(), mono_(buffer_.data(), buffer_.size(), upstream), value_(&mono_) {}

    // 返回折叠后的视图，仅在本次函数内有效。
    std::string_view fold(std::string_view s) {
        ascii_fold_into(s, value_);
        return value_;
    }

private:
    static constexpr std::size_t kStackBytes = 512;
    std::array<std::byte, kStackBytes> buffer_;
    std::pmr::monotonic_buffer_resource mono_;
    string value_;
};

std::size_t line_count(std::string_view text) {
    return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
}

}  // namespace

// ---------------------------------------------------------------------------
// modlist.txt：文件第一条为最高优先级；返回低→高（文件顺序反转）。
// ---------------------------------------------------------------------------
vector<ModEntry> read_modlist(std::string_view file, mr* mem) {
    ScratchBuf scratch(mem);
    string& data = scratch.text();
    load_file(file, data);
    const std::string_view text = strip_bom(data);

    vector<ModEntry> out(mem);
    out.reserve(line_count(text));
    for_each_line(text, [&](std::string_view raw) {
        const std::string_view line = chomp(raw);
        if (line.empty() || line.front() == '#') {
            return;
        }
        bool enabled = false;
        switch (line.front()) {
            case '+': enabled = true; break;
            case '-': enabled = false; break;
            default:  return;  // '*'-前缀或其它不可识别前缀
        }
        const std::string_view name = line.substr(1);
        if (name.empty()) {
            return;
        }
        ModEntry entry(mem);
        entry.name.assign(name);
        entry.enabled = enabled;
        // 分隔符名以 "_separator" 结尾（名称本身原样保留，不去后缀）。
        entry.separator = name.size() >= 10 && name.substr(name.size() - 10) == "_separator";
        out.push_back(std::move(entry));
    });
    std::reverse(out.begin(), out.end());
    return out;
}

void write_modlist(std::string_view file, std::span<const ModEntry> low_to_high) {
    atomic_write(file, [&](std::ofstream& out) {
        out << "# This file was automatically generated by Mod Organizer.\n";
        // 输入为低→高，文件第一条是最高优先级，故反向写出。
        for (auto it = low_to_high.rbegin(); it != low_to_high.rend(); ++it) {
            out.put(it->enabled ? '+' : '-');
            out.write(it->name.data(), static_cast<std::streamsize>(it->name.size()));
            out.put('\n');
        }
    });
}

// ---------------------------------------------------------------------------
// plugins.txt / loadorder.txt
// ---------------------------------------------------------------------------
vector<PluginEntry> read_plugins_txt(std::string_view file, mr* mem) {
    ScratchBuf scratch(mem);
    string& data = scratch.text();
    load_file(file, data);
    const std::string_view text = strip_bom(data);

    vector<PluginEntry> out(mem);
    out.reserve(line_count(text));
    for_each_line(text, [&](std::string_view raw) {
        const std::string_view line = chomp(raw);
        if (line.empty() || line.front() == '#') {
            return;
        }
        std::string_view name = line;
        bool enabled = false;
        if (line.front() == '*') {
            enabled = true;
            name.remove_prefix(1);
        }
        if (name.empty()) {
            return;
        }
        // 插件名按行原文保留（mods 目录下的文件名可含空格）。
        PluginEntry entry(mem);
        entry.name.assign(name);
        entry.enabled = enabled;
        out.push_back(std::move(entry));
    });
    return out;
}

void write_plugins_txt(std::string_view file, std::span<const PluginEntry> v) {
    atomic_write(file, [&](std::ofstream& out) {
        out << "# This file was automatically generated by Mod Organizer.\n";
        for (const auto& e : v) {
            if (e.enabled) {
                out.put('*');
            }
            out.write(e.name.data(), static_cast<std::streamsize>(e.name.size()));
            out.put('\n');
        }
    });
}

vector<string> read_loadorder_txt(std::string_view file, mr* mem) {
    ScratchBuf scratch(mem);
    string& data = scratch.text();
    load_file(file, data);
    const std::string_view text = strip_bom(data);

    vector<string> out(mem);
    out.reserve(line_count(text));
    for_each_line(text, [&](std::string_view raw) {
        const std::string_view line = chomp(raw);
        if (line.empty() || line.front() == '#') {
            return;
        }
        // loadorder 条目按行读入（空白按 QString 惯例去掉两端空白）。
        string name(trim_ws(line), mem);
        out.push_back(std::move(name));
    });
    return out;
}

// ---------------------------------------------------------------------------
// Ini（QSettings 风格子集）
// ---------------------------------------------------------------------------
Ini Ini::parse(std::string_view text, mr* mem) {
    ScratchBuf scratch(mem);
    FoldBuf fold(mem);

    Ini ini(mem);
    // 预先给出 "" 区段：[General] 之前的无区段键。
    std::pmr::map<string, Section>::iterator current =
        ini.data_.emplace(string(mem), Section(mem)).first;

    for_each_line(strip_bom(text), [&](std::string_view raw) {
        const std::string_view line = trim_ws(chomp(raw));
        if (line.empty() || line.front() == ';' || line.front() == '#') {
            return;
        }
        if (line.front() == '[') {
            if (line.size() < 2 || line.back() != ']') {
                return;
            }
            const std::string_view name = trim_ws(line.substr(1, line.size() - 2));
            current = ini.data_.emplace(string(fold.fold(name), mem), Section(mem)).first;
            return;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            return;
        }
        const std::string_view key = trim_ws(line.substr(0, eq));
        if (key.empty()) {
            return;
        }
        std::string_view value = trim_ws(line.substr(eq + 1));
        // @ByteArray(x) 去壳；其它 @ 前缀原样保留；反斜杠不做转义折叠。
        constexpr std::string_view kByteArray = "@ByteArray(";
        if (value.size() >= kByteArray.size() + 1 &&
            std::string_view(value.data(), kByteArray.size()) == kByteArray && value.back() == ')') {
            value.remove_prefix(kByteArray.size());
            value.remove_suffix(1);
        }
        current->second.insert_or_assign(string(fold.fold(key), mem), string(value, mem));
    });
    return ini;
}

Ini Ini::load(std::string_view file, mr* mem) {
    ScratchBuf scratch(mem);
    load_file(file, scratch.text());
    return parse(scratch.text(), mem);
}

std::optional<string> Ini::get(std::string_view section, std::string_view key, mr* mem) const {
    FoldBuf fold(mem);
    const auto it = data_.find(string(fold.fold(section), mem));
    if (it == data_.end()) {
        return std::nullopt;
    }
    const auto jt = it->second.find(string(fold.fold(key), mem));
    if (jt == it->second.end()) {
        return std::nullopt;
    }
    return std::optional<string>(std::in_place, jt->second, mem);
}

// ---------------------------------------------------------------------------
// Wine 路径
// ---------------------------------------------------------------------------
string wine_to_unix(std::string_view winpath, std::string_view prefix, mr* mem) {
    string out(mem);

    // 已是 Unix 绝对路径：原样返回。
    if (!winpath.empty() && winpath.front() == '/') {
        out.assign(winpath);
        return out;
    }

    char drive = 0;
    std::string_view rest = winpath;
    if (winpath.size() >= 2 && ascii_alpha(winpath[0]) && winpath[1] == ':') {
        drive = ascii_lower(winpath[0]);
        rest.remove_prefix(2);
    }

    if (drive == 'c') {
        out.append(prefix);
        out.append("/drive_c");
    } else if (drive == 'z') {
        // 首个成分会自带前导 '/'；后面 rest 为空时补成根目录 "/"。
    } else if (drive != 0) {
        out.append(prefix);
        out.append("/dosdevices/");
        out.push_back(drive);
        out.push_back(':');
    }
    // 盘符大小写不敏感；大小写已折叠。其余成分原样保留，不折叠 ".."。

    // 逐段追加：单/双反斜杠（以及正斜杠）都视为分隔符，连续分隔符合并为一段。
    std::size_t i = 0;
    const std::size_t n = rest.size();
    while (i < n) {
        while (i < n && (rest[i] == '\\' || rest[i] == '/')) {
            ++i;
        }
        const std::size_t start = i;
        while (i < n && rest[i] != '\\' && rest[i] != '/') {
            ++i;
        }
        if (i > start) {
            out.push_back('/');
            out.append(rest.substr(start, i - start));
        }
    }
    if (drive == 'z' && out.empty()) {
        out.push_back('/');
    }
    return out;
}

}  // namespace mol

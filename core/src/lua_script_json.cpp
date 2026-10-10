// lua_script_json.hpp 的实现：最小 JSON + 事件行/状态序列化。
// 协议面很小（扁平对象 + 一层嵌套），手写比引依赖好审计；输入一律按不可信文本处理。
#include "lua_script_json.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace mol::script {
namespace {

using namespace std::chrono;

class Parser {
  public:
    explicit Parser(std::string_view text) : t_(text) {}
    bool parse(JValue& out) {
        skip();
        if (!value(out)) return false;
        skip();
        return i_ == t_.size();  // 拒绝尾部垃圾
    }

  private:
    void skip() {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r')) ++i_;
    }
    bool lit(char c) {
        if (i_ < t_.size() && t_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }
    bool value(JValue& out) {
        if (i_ >= t_.size()) return false;
        switch (t_[i_]) {
            case '{': return object(out);
            case '[': return array(out);
            case '"': out.t = JValue::T::Str;
                return string(out.str);
            case 't':
            case 'f': return boolean(out);
            case 'n': return null(out);
            default: return number(out);
        }
    }
    bool object(JValue& out) {
        out.t = JValue::T::Obj;
        if (!lit('{')) return false;
        skip();
        if (lit('}')) return true;
        for (;;) {
            skip();
            std::string key;
            if (t_[i_] != '"' || !string(key)) return false;
            skip();
            if (!lit(':')) return false;
            skip();
            JValue v;
            if (!value(v)) return false;
            out.obj.emplace_back(std::move(key), std::move(v));
            skip();
            if (lit(',')) continue;
            return lit('}');
        }
    }
    bool array(JValue& out) {
        out.t = JValue::T::Arr;
        if (!lit('[')) return false;
        skip();
        if (lit(']')) return true;
        for (;;) {
            skip();
            JValue v;
            if (!value(v)) return false;
            out.arr.push_back(std::move(v));
            skip();
            if (lit(',')) continue;
            return lit(']');
        }
    }
    bool string(std::string& out) {
        if (!lit('"')) return false;
        out.clear();
        while (i_ < t_.size()) {
            const char c = t_[i_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i_ >= t_.size()) return false;
            const char e = t_[i_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (i_ + 4 > t_.size()) return false;
                    unsigned code = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = t_[i_++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else return false;
                    }
                    // 代理对：与下一个 \uXXXX 合成
                    if (code >= 0xd800 && code <= 0xdbff && i_ + 6 <= t_.size() && t_[i_] == '\\' &&
                        t_[i_ + 1] == 'u') {
                        i_ += 2;
                        unsigned lo = 0;
                        bool ok = true;
                        for (int k = 0; k < 4; ++k) {
                            const char h = t_[i_++];
                            lo <<= 4;
                            if (h >= '0' && h <= '9') lo |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') lo |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') lo |= static_cast<unsigned>(h - 'A' + 10);
                            else ok = false;
                        }
                        if (ok && lo >= 0xdc00 && lo <= 0xdfff)
                            code = 0x10000 + ((code - 0xd800) << 10) + (lo - 0xdc00);
                    }
                    // 编码成 UTF-8
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xc0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                    } else if (code < 0x10000) {
                        out.push_back(static_cast<char>(0xe0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                    } else {
                        out.push_back(static_cast<char>(0xf0 | (code >> 18)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                    }
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool boolean(JValue& out) {
        if (t_.compare(i_, 4, "true") == 0) {
            i_ += 4;
            out.t = JValue::T::Bool;
            out.b = true;
            return true;
        }
        if (t_.compare(i_, 5, "false") == 0) {
            i_ += 5;
            out.t = JValue::T::Bool;
            out.b = false;
            return true;
        }
        return false;
    }
    bool null(JValue&) {
        if (t_.compare(i_, 4, "null") == 0) {
            i_ += 4;
            return true;
        }
        return false;
    }
    bool number(JValue& out) {
        const std::size_t start = i_;
        if (i_ < t_.size() && (t_[i_] == '-' || t_[i_] == '+')) ++i_;
        bool digits = false;
        while (i_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[i_]))) {
            ++i_;
            digits = true;
        }
        if (i_ < t_.size() && t_[i_] == '.') {
            ++i_;
            while (i_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[i_]))) {
                ++i_;
                digits = true;
            }
        }
        if (!digits) return false;
        if (i_ < t_.size() && (t_[i_] == 'e' || t_[i_] == 'E')) {
            ++i_;
            if (i_ < t_.size() && (t_[i_] == '-' || t_[i_] == '+')) ++i_;
            bool ed = false;
            while (i_ < t_.size() && std::isdigit(static_cast<unsigned char>(t_[i_]))) {
                ++i_;
                ed = true;
            }
            if (!ed) return false;
        }
        out.t = JValue::T::Num;
        out.num = std::strtod(std::string(t_.substr(start, i_ - start)).c_str(), nullptr);
        return true;
    }

    std::string_view t_;
    std::size_t i_ = 0;
};

}  // namespace

const JValue* JValue::find(std::string_view key) const {
    if (t != T::Obj) return nullptr;
    for (const auto& [k, v] : obj)
        if (k == key) return &v;
    return nullptr;
}

std::string JValue::as_string() const {
    if (t == T::Str) return str;
    if (t == T::Bool) return b ? "true" : "false";
    if (t == T::Num) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.14g", num);
        return buf;
    }
    return {};
}

bool JValue::as_bool(bool def) const {
    if (t == T::Bool) return b;
    if (t == T::Num) return num != 0;
    if (t == T::Str) return str == "true" || str == "1";
    return def;
}

bool json_parse(std::string_view text, JValue& out) {
    if (text.size() > 1024 * 1024) return false;  // 协议消息不该更大
    Parser p(text);
    return p.parse(out);
}

std::string json_escape_append(std::string& out, std::string_view s) {
    static const char* hex = "0123456789abcdef";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 15]);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

std::string json_quote(std::string_view s) {
    std::string out = "\"";
    json_escape_append(out, s);
    out.push_back('"');
    return out;
}

std::string json_dump(const JValue& v) {
    switch (v.t) {
        case JValue::T::Null: return "null";
        case JValue::T::Bool: return v.b ? "true" : "false";
        case JValue::T::Num: {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.14g", v.num);
            return buf;
        }
        case JValue::T::Str: return json_quote(v.str);
        case JValue::T::Arr: {
            std::string out = "[";
            for (std::size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out.push_back(',');
                out += json_dump(v.arr[i]);
            }
            return out + "]";
        }
        case JValue::T::Obj: {
            std::string out = "{";
            for (std::size_t i = 0; i < v.obj.size(); ++i) {
                if (i) out.push_back(',');
                out += json_quote(v.obj[i].first);
                out.push_back(':');
                out += json_dump(v.obj[i].second);
            }
            return out + "}";
        }
    }
    return "null";
}

std::string body_to_value(std::string_view body) {
    JValue v;
    if (json_parse(body, v) && (v.t == JValue::T::Str || v.t == JValue::T::Bool || v.t == JValue::T::Num))
        return v.as_string();
    return std::string(body);
}

std::int64_t now_ms() {
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string event_started(const Status& st) {
    return "{\"event\":\"started\",\"ns\":" + json_quote(st.ns) + ",\"script\":" + json_quote(st.script) +
           ",\"root\":" + json_quote(st.root) + ",\"http_url\":" + json_quote(st.http_url) +
           ",\"token\":" + json_quote(st.token) + "}";
}

std::string event_log(std::string_view line) {
    return "{\"event\":\"log\",\"line\":" + json_quote(line) + "}";
}

std::string event_op(const Status& st) {
    std::string out = "{\"event\":\"op\",\"op\":" + json_quote(st.op.name) + ",\"detail\":" +
                      json_quote(st.op.detail) + ",\"since_ms\":" + std::to_string(st.op.since_ms) +
                      ",\"timeout_ms\":" + std::to_string(st.op.timeout_ms) + "}";
    return out;
}

std::string event_done(const RunResult& r) {
    std::string out = "{\"event\":\"done\",\"ok\":" + std::string(r.ok ? "true" : "false") +
                      ",\"ns\":" + json_quote(r.ns) + ",\"root\":" + json_quote(r.root) +
                      ",\"http_url\":" + json_quote(r.http_url) + ",\"landlock\":" + json_quote(r.landlock) +
                      ",\"fs_ops\":" + std::to_string(r.fs_ops) +
                      ",\"proc_runs\":" + std::to_string(r.proc_runs) +
                      ",\"net_requests\":" + std::to_string(r.net_requests) + ",\"error\":" +
                      json_quote(r.error) + "}";
    return out;
}

std::string status_to_json(const Status& st) {
    std::string out = "{\"ns\":" + json_quote(st.ns) + ",\"script\":" + json_quote(st.script) +
                      ",\"instance\":" + json_quote(st.instance) + ",\"root\":" + json_quote(st.root) +
                      ",\"http_url\":" + json_quote(st.http_url) + ",\"run_state\":" + json_quote(st.run_state) +
                      ",\"op\":" + json_quote(st.op.name) + ",\"op_detail\":" + json_quote(st.op.detail) +
                      ",\"op_since_ms\":" + std::to_string(st.op.since_ms) +
                      ",\"op_timeout_ms\":" + std::to_string(st.op.timeout_ms) +
                      ",\"lua_source\":" + json_quote(st.lua_source) +
                      ",\"lua_line\":" + std::to_string(st.lua_line) + ",\"frames\":[";
    for (std::size_t i = 0; i < st.frames.size(); ++i) {
        if (i) out.push_back(',');
        out += json_quote(st.frames[i]);
    }
    out += "]";
    out += ",\"instructions\":" + std::to_string(st.instructions);
    out += ",\"fs_ops\":" + std::to_string(st.fs_ops);
    out += ",\"proc_runs\":" + std::to_string(st.proc_runs);
    out += ",\"net_requests\":" + std::to_string(st.net_requests);
    out += ",\"bytes_written\":" + std::to_string(st.bytes_written);
    out += ",\"landlock\":" + json_quote(st.landlock);
    out += ",\"started_ms\":" + std::to_string(st.started_ms);
    out += ",\"finished_ms\":" + std::to_string(st.finished_ms);
    out += ",\"error\":" + json_quote(st.error);
    out += ",\"log\":[";
    for (std::size_t i = 0; i < st.log.size(); ++i) {
        if (i) out.push_back(',');
        out += json_quote(st.log[i]);
    }
    out += "]";
    out += ",\"state\":{";
    bool first = true;
    for (const auto& [k, v] : st.state) {
        if (!first) out.push_back(',');
        first = false;
        out += json_quote(k);
        out.push_back(':');
        out += json_quote(v);
    }
    out += "}}";
    return out;
}

std::string result_to_json(const RunResult& r) {
    std::string out = "{\"ok\":" + std::string(r.ok ? "true" : "false") + ",\"ns\":" + json_quote(r.ns) +
                      ",\"root\":" + json_quote(r.root) + ",\"http_url\":" + json_quote(r.http_url) +
                      ",\"landlock\":" + json_quote(r.landlock) + ",\"fs_ops\":" + std::to_string(r.fs_ops) +
                      ",\"proc_runs\":" + std::to_string(r.proc_runs) +
                      ",\"net_requests\":" + std::to_string(r.net_requests) + ",\"error\":" +
                      json_quote(r.error) + ",\"log\":[";
    for (std::size_t i = 0; i < r.log.size(); ++i) {
        if (i) out.push_back(',');
        out += json_quote(r.log[i]);
    }
    out += "],\"state\":{";
    bool first = true;
    for (const auto& [k, v] : r.state) {
        if (!first) out.push_back(',');
        first = false;
        out += json_quote(k);
        out.push_back(':');
        out += json_quote(v);
    }
    out += "}}";
    return out;
}

}  // namespace mol::script

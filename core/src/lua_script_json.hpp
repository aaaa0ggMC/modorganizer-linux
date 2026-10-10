#pragma once
// lua_script*.cpp 内部共用：最小 JSON 值/解析/序列化、Status/RunResult 的事件行。
// 故意不用 alib6 JSON：本模块保持纯文本 TU（模块/文本混用约束见 cli/cmd_common.hpp），
// 且协议面很小（扁平和一层嵌套），手写更好审计。
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mol/lua_script.hpp"

namespace mol::script {

// ---- 最小 JSON ---------------------------------------------------------------
struct JValue {
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JValue> arr;
    std::vector<std::pair<std::string, JValue>> obj;
    const JValue* find(std::string_view key) const;
    std::string as_string() const;  // Str 原样；Bool/Num 转文本；其它空
    bool as_bool(bool def = false) const;
};
bool json_parse(std::string_view text, JValue& out);  // 完整文档；拒绝尾部垃圾
std::string json_dump(const JValue& v);

std::string json_quote(std::string_view s);  // 含引号、转义控制字符
inline std::string json_kv(std::string_view k, std::string_view v) {
    return json_quote(k) + ":" + json_quote(v);
}
// HTTP body → state 值：能解析成 JSON 标量就用它，否则用原文（浏览器表单友好）。
std::string body_to_value(std::string_view body);

// ---- 事件行 / 序列化 ----------------------------------------------------------
std::string event_started(const Status& st);
std::string event_log(std::string_view line);
std::string event_op(const Status& st);
std::string event_done(const RunResult& r);
std::string status_to_json(const Status& st);   // 含 state/log 快照
std::string result_to_json(const RunResult& r);

// ---- 小工具 -------------------------------------------------------------------
std::int64_t now_ms();  // steady_clock 毫秒（进程启动为 0）
std::string json_escape_append(std::string& out, std::string_view s);

}  // namespace mol::script

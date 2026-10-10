#pragma once
// lua_script_http.cpp 的内部声明（不进公共头：state_json 只服务于 HTTP 序列化）。
#pragma once
#include <string>

#include "mol/lua_script.hpp"

namespace mol::script {

// Status 里的 state 映射 → JSON 对象文本。
std::string state_json(const Status& st);

}  // namespace mol::script

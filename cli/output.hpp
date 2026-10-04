#pragma once
// envelope 序列化（alib6 反射 + 紧凑 JSON）与文本渲染。
//
// 设计（WP8 返工后）：不再手写 JSON 值树/序列化器。命令结果定义成普通结构体
// （cli/results.hpp，字段名对齐 docs/CLI.md），用 alib6 C++26 反射 to_adata()
// 一次转成 AData，再由 JSON 以「紧凑 + 键字典序」配置 dump —— 输出单行且确定。
//
// 混用约束：本头文件包含 cli/cmd_common.hpp（内有 import alib6），
// 因此它必须是 TU 中最后一个 #include（详见 cmd_common.hpp 文件头）。
#include "cmd_common.hpp"

namespace cli {

// envelope 结构体（反射序列化；data 成员是 AData，to_adata 会原样拷贝）
struct Envelope {
    int schema_version = 1;
    bool ok = false;
    std::pmr::string command;
    alib6::AData data;  // ok=false 时保持 null
    std::pmr::vector<Err> errors;
    std::pmr::vector<Err> warnings;

    explicit Envelope(mol::allocator_type a = {})
        : command(a), data(a.resource()), errors(a), warnings(a) {}
};

Envelope make_envelope(const Result& r, mol::mr* mem);

// 紧凑单行 JSON（键按字典序 → 输出确定；GUI 只解析这种形态）
std::pmr::string serialize_envelope(const Result& r, mol::mr* mem);

// 文本模式：stdout 的一行摘要 + 必要列表（不保证稳定，GUI 不得解析）；失败时为空。
std::pmr::vector<std::pmr::string> render_text(const Result& r, mol::mr* mem);

// 文本模式：warnings/errors 的诊断行（内容与 envelope 一致，供 stderr 输出）。
std::pmr::vector<std::pmr::string> render_diagnostics(const Result& r, mol::mr* mem);

}  // namespace cli

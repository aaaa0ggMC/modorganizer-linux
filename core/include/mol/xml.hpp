#pragma once
// 极小的 XML DOM（基于 expat）：够读 FOMOD 的 ModuleConfig.xml / info.xml。
// 支持 UTF-8/UTF-16（含 BOM）/ISO-8859-1/windows-1252 声明；忽略命名空间前缀以外的细节（元素名原样，大小写敏感）。
#include <string_view>
#include <utility>

#include "mol/error.hpp"
#include "mol/pmr.hpp"

namespace mol {

struct XmlNode {
    using allocator_type = mol::allocator_type;
    string name;
    string text;  // 直接文本（首尾空白已去掉；多段文本按出现顺序拼接）
    vector<std::pair<string, string>> attrs;
    vector<XmlNode> children;

    explicit XmlNode(allocator_type a = {}) : name(a), text(a), attrs(a), children(a) {}
    XmlNode(const XmlNode& o, allocator_type a) : name(o.name, a), text(o.text, a), attrs(o.attrs, a), children(o.children, a) {}
    XmlNode(XmlNode&& o, allocator_type a) : name(std::move(o.name), a), text(std::move(o.text), a), attrs(std::move(o.attrs), a), children(std::move(o.children), a) {}
    XmlNode(const XmlNode&) = default;
    XmlNode(XmlNode&&) = default;
    XmlNode& operator=(const XmlNode&) = default;
    XmlNode& operator=(XmlNode&&) = default;

    // 属性值；不存在返回 nullptr。
    const string* attr(std::string_view key) const;
    // 第一个同名子元素；不存在返回 nullptr。
    const XmlNode* child(std::string_view name) const;
};

// 解析失败 → Error{invalid_argument}。
XmlNode parse_xml(std::string_view bytes, mr* mem = default_mr());

}  // namespace mol

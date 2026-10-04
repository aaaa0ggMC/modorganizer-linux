#include "mol/xml.hpp"

#include <expat.h>

#include <cstring>

namespace mol {
namespace {

struct Builder {
    mr* mem;
    XmlNode root;
    std::vector<XmlNode*> stack;
    bool have_root = false;
    explicit Builder(mr* m) : mem(m), root(m) {}
};

void XMLCALL on_start(void* ud, const XML_Char* name, const XML_Char** atts) {
    auto* b = static_cast<Builder*>(ud);
    XmlNode* n;
    if (b->stack.empty()) {
        n = &b->root;
        b->have_root = true;
    } else {
        b->stack.back()->children.emplace_back(XmlNode(b->mem));
        n = &b->stack.back()->children.back();
    }
    n->name = string(name, b->mem);
    for (int i = 0; atts[i]; i += 2) n->attrs.emplace_back(string(atts[i], b->mem), string(atts[i + 1], b->mem));
    b->stack.push_back(n);
}
void XMLCALL on_end(void* ud, const XML_Char*) {
    auto* b = static_cast<Builder*>(ud);
    XmlNode* n = b->stack.back();
    b->stack.pop_back();
    auto& t = n->text;
    std::size_t s = 0, e = t.size();
    while (s < e && (t[s] == ' ' || t[s] == '\t' || t[s] == '\r' || t[s] == '\n')) ++s;
    while (e > s && (t[e - 1] == ' ' || t[e - 1] == '\t' || t[e - 1] == '\r' || t[e - 1] == '\n')) --e;
    t = string(t.substr(s, e - s), b->mem);
}
void XMLCALL on_text(void* ud, const XML_Char* s, int len) {
    auto* b = static_cast<Builder*>(ud);
    if (!b->stack.empty()) b->stack.back()->text.append(s, static_cast<std::size_t>(len));
}

// windows-1252 / latin1 → Unicode（0x80-0x9F 用 CP1252 的映射，其余同码位）
int XMLCALL unknown_enc(void*, const XML_Char* name, XML_Encoding* info) {
    auto ieq = [](const char* a, const char* b) {
        for (; *a && *b; ++a, ++b)
            if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
        return *a == *b;
    };
    if (!ieq(name, "windows-1252") && !ieq(name, "cp1252") && !ieq(name, "iso-8859-15")) return XML_STATUS_ERROR;
    static const unsigned short hi[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                          0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                          0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
    for (int i = 0; i < 256; ++i) info->map[i] = (i >= 0x80 && i < 0xA0) ? hi[i - 0x80] : i;
    info->data = nullptr;
    info->convert = nullptr;
    info->release = nullptr;
    return XML_STATUS_OK;
}
}  // namespace

const string* XmlNode::attr(std::string_view key) const {
    for (const auto& [k, v] : attrs)
        if (k == key) return &v;
    return nullptr;
}
const XmlNode* XmlNode::child(std::string_view n) const {
    for (const auto& c : children)
        if (c.name == n) return &c;
    return nullptr;
}

XmlNode parse_xml(std::string_view bytes, mr* mem) {
    Builder b(mem);
    XML_Parser p = XML_ParserCreate(nullptr);
    if (!p) throw Error("io_error", "XML_ParserCreate failed");
    XML_SetUserData(p, &b);
    XML_SetElementHandler(p, on_start, on_end);
    XML_SetCharacterDataHandler(p, on_text);
    XML_SetUnknownEncodingHandler(p, unknown_enc, nullptr);
    const bool ok = XML_Parse(p, bytes.data(), static_cast<int>(bytes.size()), 1) == XML_STATUS_OK;
    std::string err;
    if (!ok) err = std::string(XML_ErrorString(XML_GetErrorCode(p))) + " at line " + std::to_string(XML_GetCurrentLineNumber(p));
    XML_ParserFree(p);
    if (!ok) throw Error("invalid_argument", "invalid XML: " + err);
    if (!b.have_root) throw Error("invalid_argument", "invalid XML: no root element");
    return std::move(b.root);
}

}  // namespace mol

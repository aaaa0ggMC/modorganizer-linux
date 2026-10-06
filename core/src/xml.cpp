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

namespace {
// 一次解析；失败返回 expat 错误码与说明（b 里的半成品由调用方丢弃）
XML_Error parse_once(std::string_view bytes, const char* encoding, Builder& b, std::string& err) {
    XML_Parser p = XML_ParserCreate(encoding);
    if (!p) throw Error("io_error", "XML_ParserCreate failed");
    XML_SetUserData(p, &b);
    XML_SetElementHandler(p, on_start, on_end);
    XML_SetCharacterDataHandler(p, on_text);
    XML_SetUnknownEncodingHandler(p, unknown_enc, nullptr);
    XML_Error code = XML_ERROR_NONE;
    if (XML_Parse(p, bytes.data(), static_cast<int>(bytes.size()), 1) != XML_STATUS_OK) {
        code = XML_GetErrorCode(p);
        err = std::string(XML_ErrorString(code)) + " at line " + std::to_string(XML_GetCurrentLineNumber(p));
    }
    XML_ParserFree(p);
    return code;
}

void put_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
}

// 声明的编码与实际不符时（MO2 能装、expat 报 "encoding specified in XML declaration is incorrect"）：
// 按 BOM / 字节模式判断真实编码，转成 UTF-8，再把声明里的 encoding="…" 去掉
std::string reencode_as_utf8(std::string_view in) {
    std::string out;
    const auto u = [&](std::size_t i) { return static_cast<unsigned char>(in[i]); };
    int utf16 = 0;  // 1 = LE，2 = BE
    std::size_t start = 0;
    if (in.size() >= 2 && u(0) == 0xFF && u(1) == 0xFE) { utf16 = 1; start = 2; }
    else if (in.size() >= 2 && u(0) == 0xFE && u(1) == 0xFF) { utf16 = 2; start = 2; }
    else if (in.size() >= 2 && u(0) == '<' && u(1) == 0) utf16 = 1;
    else if (in.size() >= 2 && u(0) == 0 && u(1) == '<') utf16 = 2;
    else if (in.size() >= 3 && u(0) == 0xEF && u(1) == 0xBB && u(2) == 0xBF) start = 3;
    if (utf16) {
        for (std::size_t i = start; i + 1 < in.size(); i += 2) {
            std::uint32_t c = utf16 == 1 ? (u(i) | (u(i + 1) << 8)) : ((u(i) << 8) | u(i + 1));
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < in.size()) {
                const std::uint32_t lo = utf16 == 1 ? (u(i + 2) | (u(i + 3) << 8)) : ((u(i + 2) << 8) | u(i + 3));
                if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
            }
            put_utf8(out, c);
        }
    } else {
        out.assign(in.substr(start));
    }
    // <?xml … encoding="…" … ?> → 去掉 encoding 属性
    if (out.rfind("<?xml", 0) == 0) {
        const auto end = out.find("?>");
        const auto e = out.find("encoding", 0);
        if (end != std::string::npos && e != std::string::npos && e < end) {
            auto q = out.find_first_of("\"'", e);
            if (q != std::string::npos && q < end) {
                const auto q2 = out.find(out[q], q + 1);
                if (q2 != std::string::npos && q2 < end) out.erase(e, q2 + 1 - e);
            }
        }
    }
    return out;
}
}  // namespace

XmlNode parse_xml(std::string_view bytes, mr* mem) {
    std::string err;
    {
        Builder b(mem);
        const XML_Error code = parse_once(bytes, nullptr, b, err);
        if (code == XML_ERROR_NONE) {
            if (!b.have_root) throw Error("invalid_argument", "invalid XML: no root element");
            return std::move(b.root);
        }
        if (code != XML_ERROR_INCORRECT_ENCODING && code != XML_ERROR_UNKNOWN_ENCODING && code != XML_ERROR_INVALID_TOKEN)
            throw Error("invalid_argument", "invalid XML: " + err);
    }
    // 编码类错误：转成 UTF-8 重试；内容仍不是合法 UTF-8（多半是没声明的 windows-1252）→ 按 Latin-1 读
    const std::string fixed = reencode_as_utf8(bytes);
    for (const char* enc : {"UTF-8", "ISO-8859-1"}) {
        Builder b(mem);
        std::string e2;
        if (parse_once(fixed, enc, b, e2) == XML_ERROR_NONE && b.have_root) return std::move(b.root);
    }
    throw Error("invalid_argument", "invalid XML: " + err);
}

}  // namespace mol

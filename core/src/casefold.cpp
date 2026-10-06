#include "mol/casefold.hpp"

namespace mol {

// ASCII 小写化；非 ASCII 字节原样保留。
string casefold(std::string_view s, mr* mem) {
    string out(s, mem);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return out;
}

string html_unescape(std::string_view s, mr* mem) {
    string out(mem);
    out.reserve(s.size());
    auto put_utf8 = [&](unsigned long cp) {
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else { out.push_back(static_cast<char>(0xF0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out.push_back(s[i]); continue; }
        const std::size_t semi = s.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 10) { out.push_back('&'); continue; }
        const std::string_view ent = s.substr(i + 1, semi - i - 1);
        bool ok = true;
        if (ent == "amp") out.push_back('&');
        else if (ent == "lt") out.push_back('<');
        else if (ent == "gt") out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos") out.push_back('\'');
        else if (ent.size() >= 2 && ent[0] == '#') {
            const bool hex = ent[1] == 'x' || ent[1] == 'X';
            const std::string_view digits = ent.substr(hex ? 2 : 1);
            unsigned long cp = 0;
            ok = !digits.empty();
            for (char c : digits) {
                int d = (c >= '0' && c <= '9') ? c - '0' : (hex && c >= 'a' && c <= 'f') ? c - 'a' + 10 : (hex && c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                if (d < 0 || cp > 0x10FFFF) { ok = false; break; }
                cp = cp * (hex ? 16 : 10) + static_cast<unsigned long>(d);
            }
            ok = ok && cp > 0 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
            if (ok) put_utf8(cp);
        } else {
            ok = false;
        }
        if (!ok) { out.push_back('&'); continue; }
        i = semi;
    }
    return out;
}

}  // namespace mol

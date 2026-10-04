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

}  // namespace mol

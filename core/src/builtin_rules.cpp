#include "mol/rules.hpp"
namespace mol::rules {
namespace {
constexpr unsigned char skyrim[] = {
#embed "../../rules/skyrim.lua"
,0};
const Source sources[]={{"skyrim",std::string_view(reinterpret_cast<const char*>(skyrim),sizeof(skyrim)-1)}};
}
std::span<const Source> builtin_sources() { return sources; }
}

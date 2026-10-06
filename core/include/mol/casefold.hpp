#pragma once
#include <string_view>

#include "mol/pmr.hpp"

namespace mol {

// ASCII 小写化；非 ASCII 字节原样保留（Skyrim 资源路径实际都是 ASCII）。
string casefold(std::string_view s, mr* mem = default_mr());

// 解开 HTML 字符引用：&amp; &lt; &gt; &quot; &apos; &#NN; &#xHH;（转成 UTF-8）。Nexus 的 API/集合清单里
// mod 名是 HTML 转义过的（"JK&#39;s …"），拿来当显示名/目录名前要解开。不认识的 & 序列原样保留。
string html_unescape(std::string_view s, mr* mem = default_mr());

}  // namespace mol

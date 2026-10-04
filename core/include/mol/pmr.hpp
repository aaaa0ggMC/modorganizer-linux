#pragma once
// 全项目 PMR 约定：
//  * 公共 API 不出现 std::string / std::vector / std::filesystem::path 作为返回值或成员，
//    一律 mol::string / mol::vector<T>（std::pmr）；路径用 UTF-8 mol::string，
//    仅在系统调用边界临时转 std::filesystem::path。
//  * 返回容器的函数末参数为 `mr* mem = std::pmr::get_default_resource()`，结果全部用 mem 分配。
//  * 含 pmr 成员的 struct 必须是 allocator-aware：声明 allocator_type，并提供
//    (allocator_type)、(const S&, allocator_type)、(S&&, allocator_type) 构造，
//    这样放进 mol::vector<S> 时分配器自动向下传播。
//  * 内部临时对象用 std::pmr::monotonic_buffer_resource 之类，不要悄悄落到全局 new。
#include <memory_resource>
#include <string>
#include <vector>

namespace mol {

using mr = std::pmr::memory_resource;
using string = std::pmr::string;
template <class T>
using vector = std::pmr::vector<T>;
using allocator_type = std::pmr::polymorphic_allocator<>;

inline mr* default_mr() { return std::pmr::get_default_resource(); }

}  // namespace mol

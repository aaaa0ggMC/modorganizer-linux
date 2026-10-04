#pragma once
// 极简并行 for：用 jobs 个线程处理 [0, n)。fn 不得抛异常（调用方自行捕获）；jobs<=1 或 n<=1 时在当前线程顺序执行。
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

namespace mol {

inline void parallel_for(std::size_t n, unsigned jobs, const std::function<void(std::size_t)>& fn) {
    if (n == 0) return;
    const unsigned workers = static_cast<unsigned>(std::min<std::size_t>(std::max(1u, jobs), n));
    if (workers <= 1) {
        for (std::size_t i = 0; i < n; ++i) fn(i);
        return;
    }
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned w = 0; w < workers; ++w)
        pool.emplace_back([&] {
            for (std::size_t i = next.fetch_add(1); i < n; i = next.fetch_add(1)) fn(i);
        });
    for (auto& t : pool) t.join();
}

// 下载并发数：显式值 > 环境变量 MOL_JOBS > 默认 4；夹到 [1, 16]。
inline unsigned download_jobs(unsigned requested = 0) {
    unsigned j = requested;
    if (j == 0) {
        if (const char* e = std::getenv("MOL_JOBS")) j = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    }
    if (j == 0) j = 4;
    return std::clamp(j, 1u, 16u);
}

}  // namespace mol

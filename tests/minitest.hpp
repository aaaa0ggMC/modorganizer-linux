// 极简测试框架：无外部依赖。用法见 tests/test_*.cpp
#pragma once
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace minitest {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& cases() { static std::vector<Case> c; return c; }
inline int& failures() { static int f = 0; return f; }
struct Reg { Reg(const char* n, std::function<void()> f) { cases().push_back({n, std::move(f)}); } };
}

#define TEST(name) \
  static void name(); \
  static minitest::Reg reg_##name(#name, name); \
  static void name()

#define CHECK(cond) \
  do { if (!(cond)) { ++minitest::failures(); \
    std::fprintf(stderr, "  CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); } } while (0)

#define CHECK_EQ(a, b) \
  do { auto&& _a = (a); auto&& _b = (b); if (!(_a == _b)) { ++minitest::failures(); \
    std::fprintf(stderr, "  CHECK_EQ failed: %s == %s (%s:%d)\n", #a, #b, __FILE__, __LINE__); } } while (0)

int main() {
  for (auto& c : minitest::cases()) {
    int before = minitest::failures();
    std::fprintf(stderr, "[ RUN  ] %s\n", c.name);
    c.fn();
    std::fprintf(stderr, "[ %s ] %s\n", minitest::failures() == before ? " OK " : "FAIL", c.name);
  }
  std::fprintf(stderr, "%d failure(s)\n", minitest::failures());
  return minitest::failures() ? 1 : 0;
}

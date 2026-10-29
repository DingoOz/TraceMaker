#include <chrono>
#include <cstdio>
#include <random>
#include "gpu/field.hpp"
int main() {
  const int w = 450, h = 450, L = 4; const std::size_t n = (std::size_t)w * h;
  std::mt19937 r(1);
  std::vector<std::uint8_t> pass(n * L), via(n), tgt(n * L, 0);
  for (auto& x : pass) x = r() % 100 < 85; for (auto& x : via) x = r() % 100 < 60; tgt[n / 2 + w / 2] = 1;
  tmk::gpu::FieldProblem p{w, h, L, 40000, 56569, 400000, pass.data(), via.data(), tgt.data()};
  std::vector<std::int32_t> a, b;
  for (int rep = 0; rep < 2; ++rep) {
    auto t0 = std::chrono::steady_clock::now(); int ra = tmk::gpu::field_cpu(p, a);
    auto t1 = std::chrono::steady_clock::now(); int rb = tmk::gpu::field_cpu_reference(p, b);
    auto t2 = std::chrono::steady_clock::now();
    std::printf("fast %.1f ms (%d rounds)  ref %.1f ms (%d rounds) same %d\n", std::chrono::duration<double, std::milli>(t1 - t0).count(), ra,
                std::chrono::duration<double, std::milli>(t2 - t1).count(), rb, a == b);
  }
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>

#include "gpu/field.hpp"

namespace tmk::gpu {
namespace {

// One sweep along all lines of a direction (dx, dy) in one orientation: d[next] = min(d[next], d[cur] + cost).
bool sweep(const FieldProblem& p, std::int32_t* d, int layer, int dx, int dy, std::int32_t cost) {
  bool changed = false;
  const int w = p.w, h = p.h;
  const std::uint8_t* pass = p.pass + static_cast<std::size_t>(layer) * static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  std::int32_t* dl = d + static_cast<std::size_t>(layer) * static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  // Enumerate line starts so that every cell is visited once in sweep order.
  auto relax_line = [&](int x, int y) {
    std::int32_t prev = kFieldInf;
    while (x >= 0 && y >= 0 && x < w && y < h) {
      const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x);
      if (!pass[i]) {
        prev = kFieldInf;
      } else {
        if (prev < kFieldInf && prev + cost < dl[i]) {
          dl[i] = prev + cost;
          changed = true;
        }
        prev = dl[i];
      }
      x += dx;
      y += dy;
    }
  };
  if (dy == 0) {  // rows
    for (int y = 0; y < h; ++y) relax_line(dx > 0 ? 0 : w - 1, y);
  } else if (dx == 0) {  // columns
    for (int x = 0; x < w; ++x) relax_line(x, dy > 0 ? 0 : h - 1);
  } else {  // diagonals: start on the two edges the direction comes from
    const int sx = dx > 0 ? 0 : w - 1, sy = dy > 0 ? 0 : h - 1;
    for (int x = 0; x < w; ++x) relax_line(x, sy);
    for (int y = 0; y < h; ++y)
      if (y != sy) relax_line(sx, y);
  }
  return changed;
}

bool via_relax(const FieldProblem& p, std::int32_t* d) {
  bool changed = false;
  const std::size_t n = static_cast<std::size_t>(p.w) * static_cast<std::size_t>(p.h);
  for (std::size_t i = 0; i < n; ++i) {
    if (!p.via_pass[i]) continue;
    std::int32_t best = kFieldInf;
    for (int l = 0; l < p.layers; ++l) best = std::min(best, d[static_cast<std::size_t>(l) * n + i]);
    if (best >= kFieldInf) continue;
    for (int l = 0; l < p.layers; ++l) {
      std::int32_t& v = d[static_cast<std::size_t>(l) * n + i];
      if (p.pass[static_cast<std::size_t>(l) * n + i] && best + p.via < v) {
        v = best + p.via;
        changed = true;
      }
    }
  }
  return changed;
}

}  // namespace

int field_cpu(const FieldProblem& p, std::vector<std::int32_t>& out) {
  const std::size_t n = static_cast<std::size_t>(p.w) * static_cast<std::size_t>(p.h);
  out.assign(n * static_cast<std::size_t>(p.layers), kFieldInf);
  for (std::size_t i = 0; i < out.size(); ++i)
    if (p.target[i]) out[i] = 0;
  static const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};
  int rounds = 0;
  for (bool changed = true; changed;) {
    changed = false;
    ++rounds;
    for (int l = 0; l < p.layers; ++l)
      for (const auto& dd : dirs) changed |= sweep(p, out.data(), l, dd[0], dd[1], (dd[0] && dd[1]) ? p.diag : p.step);
    if (p.layers > 1) changed |= via_relax(p, out.data());
  }
  return rounds;
}

#if !TM_HAVE_CUDA
GpuStatus field_cuda(int, const FieldProblem&, std::vector<std::int32_t>& out) {
  out.clear();
  return {false, "built without CUDA"};
}
#endif

}  // namespace tmk::gpu

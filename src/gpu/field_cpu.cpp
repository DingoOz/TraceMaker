// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>

#include "gpu/field.hpp"

namespace tmk::gpu {
namespace {

// One sweep along all lines of a direction (dx, dy) in one orientation: d[next] = min(d[next], d[cur] + cost).
// The line-by-line reference; field_cpu sweeps the same lines a row at a time (sweep_rows).
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

// The same sweep for a direction that changes row (dy != 0), one row at a time. Each line of the direction crosses each
// row once, in the order relax_line visits it, and the lines do not share cells, so every cell gets the value the
// line-by-line sweep gives it; the inner loop has no dependence between cells and vectorises. A cell is relaxed from its
// predecessor on the line only when both may be entered (relax_line restarts a line after a blocked cell), and a
// predecessor at kFieldInf gives kFieldInf + cost, which never improves a cell (kFieldInf is a quarter of the int32 range).
bool sweep_rows(const FieldProblem& p, std::int32_t* d, int layer, int dx, int dy, std::int32_t cost) {
  const int w = p.w, h = p.h;
  const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  const std::uint8_t* pass = p.pass + static_cast<std::size_t>(layer) * n;
  std::int32_t* dl = d + static_cast<std::size_t>(layer) * n;
  const int x0 = dx > 0 ? 1 : 0, x1 = dx < 0 ? w - 1 : w;  // cells whose predecessor lies inside the row before
  int changed = 0;
  for (int y = dy > 0 ? 1 : h - 2; y >= 0 && y < h; y += dy) {
    std::int32_t* row = dl + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
    const std::int32_t* prev = dl + static_cast<std::size_t>(y - dy) * static_cast<std::size_t>(w);
    const std::uint8_t* pr = pass + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
    const std::uint8_t* pp = pass + static_cast<std::size_t>(y - dy) * static_cast<std::size_t>(w);
    for (int x = x0; x < x1; ++x) {
      const std::int32_t c = prev[x - dx] + cost;
      const int better = static_cast<int>(pr[x] != 0) & static_cast<int>(pp[x - dx] != 0) & static_cast<int>(c < row[x]);
      row[x] = better ? c : row[x];
      changed |= better;
    }
  }
  return changed != 0;
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

// via_relax without branches, a row of cells at a time: each cell's best layer is taken before any of its layers
// change, as there, and a cell at kFieldInf on every layer gives kFieldInf + via (at most 1e9, so no overflow), which
// improves nothing.
bool via_relax_rows(const FieldProblem& p, std::int32_t* d, std::vector<std::int32_t>& best) {
  const std::size_t n = static_cast<std::size_t>(p.w) * static_cast<std::size_t>(p.h);
  best.assign(d, d + n);
  for (int l = 1; l < p.layers; ++l) {
    const std::int32_t* dl = d + static_cast<std::size_t>(l) * n;
    for (std::size_t i = 0; i < n; ++i) best[i] = std::min(best[i], dl[i]);
  }
  int changed = 0;
  for (int l = 0; l < p.layers; ++l) {
    std::int32_t* dl = d + static_cast<std::size_t>(l) * n;
    const std::uint8_t* pl = p.pass + static_cast<std::size_t>(l) * n;
    for (std::size_t i = 0; i < n; ++i) {
      const std::int32_t c = best[i] + p.via;
      const int better = static_cast<int>(p.via_pass[i] != 0) & static_cast<int>(pl[i] != 0) & static_cast<int>(c < dl[i]);
      dl[i] = better ? c : dl[i];
      changed |= better;
    }
  }
  return changed != 0;
}

// The row sweeps (dy == 0) without relax_line's general stepping: the same cells in the same order.
bool sweep_row_lines(const FieldProblem& p, std::int32_t* d, int layer, int dx, std::int32_t cost) {
  const int w = p.w, h = p.h;
  const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  bool changed = false;
  for (int y = 0; y < h; ++y) {
    std::int32_t* row = d + static_cast<std::size_t>(layer) * n + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
    const std::uint8_t* pr = p.pass + static_cast<std::size_t>(layer) * n + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
    std::int32_t prev = kFieldInf;
    for (int x = dx > 0 ? 0 : w - 1; x >= 0 && x < w; x += dx) {
      if (!pr[x]) {
        prev = kFieldInf;
        continue;
      }
      if (prev + cost < row[x]) {  // prev at kFieldInf never improves a cell
        row[x] = prev + cost;
        changed = true;
      }
      prev = row[x];
    }
  }
  return changed;
}

}  // namespace

namespace {
template <class Sweep, class Via>
int field_rounds(const FieldProblem& p, std::vector<std::int32_t>& out, Sweep&& sweep_dir, Via&& via_dir) {
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
      for (const auto& dd : dirs) changed |= sweep_dir(p, out.data(), l, dd[0], dd[1], (dd[0] && dd[1]) ? p.diag : p.step);
    if (p.layers > 1) changed |= via_dir(p, out.data());
  }
  return rounds;
}
}  // namespace

int field_cpu(const FieldProblem& p, std::vector<std::int32_t>& out) {
  std::vector<std::int32_t> best;
  return field_rounds(
      p, out,
      [](const FieldProblem& q, std::int32_t* d, int l, int dx, int dy, std::int32_t c) {
        return dy == 0 ? sweep_row_lines(q, d, l, dx, c) : sweep_rows(q, d, l, dx, dy, c);
      },
      [&best](const FieldProblem& q, std::int32_t* d) { return via_relax_rows(q, d, best); });
}

int field_cpu_reference(const FieldProblem& p, std::vector<std::int32_t>& out) { return field_rounds(p, out, sweep, via_relax); }

#if !TM_HAVE_CUDA && !TM_HAVE_METAL
GpuStatus field_gpu(int, const FieldProblem&, std::vector<std::int32_t>& out) {
  out.clear();
  return {false, "built without GPU support"};
}
#endif

}  // namespace tmk::gpu

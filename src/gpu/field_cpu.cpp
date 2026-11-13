// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>

#include "gpu/field.hpp"

namespace tmk::gpu {
namespace {

// One sweep along all lines of a direction (dx, dy) in one orientation: d[next] = min(d[next], d[cur] + cost).
// The line-by-line reference; field_cpu sweeps the same lines a row at a time and skips the rows that cannot change.
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

// Rows that cannot change are skipped (doc 05 §83, M23 E5). Along a row, a step depends on the row alone: if the row has not changed
// since the same sweep (layer, direction) last processed it, the step finds nothing to improve. Across rows, row y is relaxed from the
// row before it on the line: values only fall, so after a step row y stays at most that row's values plus the cost, and the step can
// improve something only if the row before changed since. The via step redoes a row when any layer of it changed. Every skipped step is one that would have changed nothing, so the field, each
// sweep's change flag and the round count are the line-by-line reference's (tests/test_gpu.cpp). In the later rounds the changes
// are local (round obstacles), and most rows are skipped.
int field_cpu(const FieldProblem& p, std::vector<std::int32_t>& out) {
  const int w = p.w, h = p.h, L = p.layers;
  const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  out.assign(n * static_cast<std::size_t>(L), kFieldInf);
  for (std::size_t i = 0; i < out.size(); ++i)
    if (p.target[i]) out[i] = 0;
  if (w <= 0 || h <= 0 || L <= 0) return 1;
  static const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};
  const auto H = static_cast<std::size_t>(h);
  std::vector<std::int64_t> changed_at(static_cast<std::size_t>(L) * H, 0);    // tick of the row's last change (0: the targets)
  std::vector<std::int64_t> swept_at(static_cast<std::size_t>(L) * 8 * H, -1);  // tick at which (layer, direction) last processed it
  std::vector<std::int64_t> via_at(H, -1);                                     // tick at which the via step last processed the row
  std::vector<std::int32_t> best(static_cast<std::size_t>(w));
  std::int64_t tick = 0;
  int rounds = 0;
  for (bool changed = true; changed;) {
    changed = false;
    ++rounds;
    for (int l = 0; l < L; ++l) {
      std::int32_t* dl = out.data() + static_cast<std::size_t>(l) * n;
      const std::uint8_t* pass = p.pass + static_cast<std::size_t>(l) * n;
      std::int64_t* ch = changed_at.data() + static_cast<std::size_t>(l) * H;
      for (int k = 0; k < 8; ++k) {
        const int dx = dirs[k][0], dy = dirs[k][1];
        const std::int32_t cost = (dx && dy) ? p.diag : p.step;
        std::int64_t* sw = swept_at.data() + (static_cast<std::size_t>(l) * 8 + static_cast<std::size_t>(k)) * H;
        ++tick;
        if (dy == 0) {  // along rows: a row depends on itself only
          for (int y = 0; y < h; ++y) {
            if (ch[y] <= sw[y]) continue;
            sw[y] = tick;
            std::int32_t* row = dl + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
            const std::uint8_t* pr = pass + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
            std::int32_t prev = kFieldInf;
            bool row_changed = false;
            for (int x = dx > 0 ? 0 : w - 1; x >= 0 && x < w; x += dx) {
              if (!pr[x]) {
                prev = kFieldInf;
                continue;
              }
              if (prev + cost < row[x]) {
                row[x] = prev + cost;
                row_changed = true;
              }
              prev = row[x];
            }
            if (row_changed) ch[y] = tick, changed = true;
          }
          continue;
        }
        // Across rows (as sweep_rows): row y from row y - dy, in the sweep's row order.
        const int x0 = dx > 0 ? 1 : 0, x1 = dx < 0 ? w - 1 : w;
        for (int y = dy > 0 ? 1 : h - 2; y >= 0 && y < h; y += dy) {
          if (ch[y - dy] <= sw[y]) continue;  // row y only fell since: still at most prev + cost wherever it was
          sw[y] = tick;
          std::int32_t* row = dl + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
          const std::int32_t* prev = dl + static_cast<std::size_t>(y - dy) * static_cast<std::size_t>(w);
          const std::uint8_t* pr = pass + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
          const std::uint8_t* pp = pass + static_cast<std::size_t>(y - dy) * static_cast<std::size_t>(w);
          int row_changed = 0;
          for (int x = x0; x < x1; ++x) {
            const std::int32_t c = prev[x - dx] + cost;
            const int better = static_cast<int>(pr[x] != 0) & static_cast<int>(pp[x - dx] != 0) & static_cast<int>(c < row[x]);
            row[x] = better ? c : row[x];
            row_changed |= better;
          }
          if (row_changed) ch[y] = tick, changed = true;
        }
      }
    }
    if (L > 1) {  // via step (as via_relax_rows), a row at a time: the row on every layer
      ++tick;
      for (int y = 0; y < h; ++y) {
        bool dirty = false;
        for (int l = 0; l < L && !dirty; ++l) dirty = changed_at[static_cast<std::size_t>(l) * H + static_cast<std::size_t>(y)] > via_at[static_cast<std::size_t>(y)];
        if (!dirty) continue;
        via_at[static_cast<std::size_t>(y)] = tick;
        const std::size_t r0 = static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        std::copy(out.begin() + static_cast<std::ptrdiff_t>(r0), out.begin() + static_cast<std::ptrdiff_t>(r0 + static_cast<std::size_t>(w)), best.begin());
        for (int l = 1; l < L; ++l) {
          const std::int32_t* dl = out.data() + static_cast<std::size_t>(l) * n + r0;
          for (int x = 0; x < w; ++x) best[static_cast<std::size_t>(x)] = std::min(best[static_cast<std::size_t>(x)], dl[x]);
        }
        const std::uint8_t* vp = p.via_pass + r0;
        for (int l = 0; l < L; ++l) {
          std::int32_t* dl = out.data() + static_cast<std::size_t>(l) * n + r0;
          const std::uint8_t* pl = p.pass + static_cast<std::size_t>(l) * n + r0;
          int row_changed = 0;
          for (int x = 0; x < w; ++x) {
            const std::int32_t c = best[static_cast<std::size_t>(x)] + p.via;
            const int better = static_cast<int>(vp[x] != 0) & static_cast<int>(pl[x] != 0) & static_cast<int>(c < dl[x]);
            dl[x] = better ? c : dl[x];
            row_changed |= better;
          }
          if (row_changed) changed_at[static_cast<std::size_t>(l) * H + static_cast<std::size_t>(y)] = tick, changed = true;
        }
      }
    }
  }
  return rounds;
}

int field_cpu_reference(const FieldProblem& p, std::vector<std::int32_t>& out) { return field_rounds(p, out, sweep, via_relax); }

#if !TM_HAVE_CUDA && !TM_HAVE_METAL
GpuStatus field_gpu(int, const FieldProblem&, std::vector<std::int32_t>& out) {
  out.clear();
  return {false, "built without GPU support"};
}
#endif

}  // namespace tmk::gpu

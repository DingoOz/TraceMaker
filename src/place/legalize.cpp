#include "place/legalize.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace tmk::place {

namespace {

// Lattice offsets sorted by Euclidean length: step `h` within 3 mm, 0.25 mm within 15 mm, 1 mm beyond.
std::vector<Point> ring_offsets(Coord h, Coord max_r) {
  struct Band {
    Coord r, step;
  };
  const Band bands[] = {{3'000'000, h}, {15'000'000, std::max<Coord>(h, 250'000)}, {max_r, std::max<Coord>(h, 1'000'000)}};
  std::vector<Point> out;
  Coord inner = -1;
  for (const Band& b : bands) {
    if (b.r <= inner) continue;
    const Coord k = b.r / b.step;
    for (Coord iy = -k; iy <= k; ++iy)
      for (Coord ix = -k; ix <= k; ++ix) {
        const Point d{ix * b.step, iy * b.step};
        const double r = std::hypot(static_cast<double>(d.x), static_cast<double>(d.y));
        if (r > static_cast<double>(b.r) || r <= static_cast<double>(inner)) continue;
        out.push_back(d);
      }
    inner = b.r;
  }
  auto len2 = [](Point d) { return static_cast<geom::i128>(d.x) * d.x + static_cast<geom::i128>(d.y) * d.y; };
  std::sort(out.begin(), out.end(), [&](Point a, Point b) {
    const auto la = len2(a), lb = len2(b);
    if (la != lb) return la < lb;
    return a.y != b.y ? a.y < b.y : a.x < b.x;
  });
  return out;
}

}  // namespace

LegaliseStats legalise(const Problem& p, Placement& pl, bool only_illegal, Coord cell) {
  LegaliseStats st;
  const Coord w = p.region.x1 - p.region.x0, h = p.region.y1 - p.region.y0;
  if (cell <= 0) {
    cell = 50'000;
    while ((w / cell + 3) * (h / cell + 3) > 6'000'000) cell *= 2;
  }
  Legality L(p);
  Raster R(p, cell);
  const std::size_t n = p.parts.size();
  std::vector<std::uint8_t> keep(n, 0);
  for (std::size_t i = 0; i < n; ++i) keep[i] = p.parts[i].movable ? 0 : 1;
  if (only_illegal) {
    L.reset(pl);
    for (std::size_t i = 0; i < n; ++i) {
      if (!p.parts[i].movable) continue;
      const int ii = static_cast<int>(i);
      if (L.inside_ok(ii, pl.pos[i], pl.rot[i]) && L.find_conflict(ii, pl.pos[i], pl.rot[i]) < 0) keep[i] = 1;
    }
  }
  L.clear();
  for (std::size_t i = 0; i < n; ++i)
    if (keep[i]) {
      L.insert(static_cast<int>(i), pl.pos[i], pl.rot[i]);
      R.add(static_cast<int>(i), pl.pos[i], pl.rot[i], +1);
      if (p.parts[i].movable) ++st.kept;
    } else if (only_illegal) {
      // Reserve the current spot of every part still to be re-placed: a part that finds no better position
      // falls back to it, and then conflicts only with what it conflicted with before (nothing new).
      L.insert(static_cast<int>(i), pl.pos[i], pl.rot[i]);
      R.add(static_cast<int>(i), pl.pos[i], pl.rot[i], +1);
    }
  std::vector<int> order;
  for (std::size_t i = 0; i < n; ++i)
    if (!keep[i]) order.push_back(static_cast<int>(i));
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    return p.parts[z(a)].area != p.parts[z(b)].area ? p.parts[z(a)].area > p.parts[z(b)].area : a < b;
  });
  const Coord max_r = static_cast<Coord>(std::hypot(static_cast<double>(w), static_cast<double>(h))) + 2'000'000;
  const std::vector<Point> offs = ring_offsets(cell, max_r);
  double sum_disp = 0;
  for (int i : order) {
    const Point target = pl.pos[z(i)];
    const Point home = pl.pos[z(i)];
    const std::uint8_t home_rot = pl.rot[z(i)];
    if (only_illegal) {
      L.remove(i);
      R.add(i, home, home_rot, -1);
    }
    bool found = false;
    Point at{};
    int at_rot = pl.rot[z(i)];
    for (int k = 0; k < 4 && !found; ++k) {
      const int r = (pl.rot[z(i)] + k) & 3;
      long budget = 400;
      for (const Point d : offs) {
        const Point q = target + d;
        ++st.raster_checks;
        bool ok = false;
        if (R.free(i, q, r)) {
          ++st.exact_checks;
          ok = L.legal(i, q, r);  // always confirmed exactly (CLAUDE.md rule 1)
          if (!ok) ++st.raster_disagree;
        } else if (budget > 0) {
          --budget;
          ++st.exact_checks;
          ok = L.legal(i, q, r);
        }
        if (ok) {
          found = true;
          at = q;
          at_rot = r;
          break;
        }
      }
    }
    if (!found) {
      ++st.failed;
      st.failures.push_back(p.parts[z(i)].ref);
      // Back to where it was (refine: its reserved spot; full: the input position, reported as a failure).
      pl.pos[z(i)] = only_illegal ? home : p.parts[z(i)].pos0;
      pl.rot[z(i)] = only_illegal ? home_rot : 0;
      L.insert(i, pl.pos[z(i)], pl.rot[z(i)]);  // still an obstacle for the parts that follow
      R.add(i, pl.pos[z(i)], pl.rot[z(i)], +1);
      continue;
    }
    const double disp = std::hypot(static_cast<double>(at.x - target.x), static_cast<double>(at.y - target.y)) / 1e6;
    st.max_disp_mm = std::max(st.max_disp_mm, disp);
    sum_disp += disp;
    pl.pos[z(i)] = at;
    pl.rot[z(i)] = static_cast<std::uint8_t>(at_rot);
    L.insert(i, at, at_rot);
    R.add(i, at, at_rot, +1);
    ++st.placed;
  }
  st.mean_disp_mm = st.placed ? sum_disp / st.placed : 0.0;
  return st;
}

}  // namespace tmk::place

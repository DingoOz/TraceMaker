#include "place/legalize.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
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

LegaliseStats legalise(const Problem& p, Placement& pl, bool only_illegal, Coord cell, const std::vector<int>* first) {
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
  // Largest extent first (a spread-out footprint without courtyard is hard to fit late, whatever its area).
  auto extent = [&](int i) {
    const Box& b = p.parts[z(i)].geom[0].body;
    return static_cast<long double>(b.x1 - b.x0) * static_cast<long double>(b.y1 - b.y0);
  };
  std::sort(order.begin(), order.end(), [&](int a, int b) { return extent(a) != extent(b) ? extent(a) > extent(b) : a < b; });
  if (first && !first->empty()) {
    // Parts that failed in an earlier attempt go first (in the given order).
    std::vector<int> head;
    for (int f : *first)
      if (std::find(order.begin(), order.end(), f) != order.end() && std::find(head.begin(), head.end(), f) == head.end()) head.push_back(f);
    std::vector<int> rest;
    for (int i : order)
      if (std::find(head.begin(), head.end(), i) == head.end()) rest.push_back(i);
    order = head;
    order.insert(order.end(), rest.begin(), rest.end());
  }
  const Coord max_r = static_cast<Coord>(std::hypot(static_cast<double>(w), static_cast<double>(h))) + 2'000'000;
  const std::vector<Point> offs = ring_offsets(cell, max_r);
  // Refine mode only fixes conflicts locally: a part whose nearest legal spot is farther than this stays put.
  constexpr Coord kRefineReach = 5'000'000;
  const auto reach2 = static_cast<geom::i128>(kRefineReach) * kRefineReach;
  std::vector<Point> target(n), home(n);
  std::vector<std::uint8_t> home_rot(n), evictions(n, 0), placed(n, 0);
  for (std::size_t i = 0; i < n; ++i) {
    target[i] = home[i] = pl.pos[i];
    home_rot[i] = pl.rot[i];
  }
  std::deque<int> work(order.begin(), order.end());
  std::vector<int> conflicts;
  long eviction_budget = 4 * static_cast<long>(order.size()) + 20;

  // Big parts (≥ 2% of the board area): before committing one, check that every big part still waiting has at
  // least one raster-free spot left (coarse lattice, any rotation); otherwise try the next candidate.
  const long double region_area = static_cast<long double>(w) * static_cast<long double>(h);
  std::vector<std::uint8_t> big(n, 0);
  for (int i : order) big[z(i)] = extent(i) >= 0.02L * region_area ? 1 : 0;
  auto has_spot = [&](int j) {
    const Coord step = std::max<Coord>(500'000, cell * 10);
    for (Coord y = p.region.y0; y <= p.region.y1; y += step)
      for (Coord x = p.region.x0; x <= p.region.x1; x += step)
        for (int r = 0; r < 4; ++r)
          if (R.free(j, Point{x, y} - Point{(p.parts[z(j)].geom[z(r)].body.x0 + p.parts[z(j)].geom[z(r)].body.x1) / 2,
                                            (p.parts[z(j)].geom[z(r)].body.y0 + p.parts[z(j)].geom[z(r)].body.y1) / 2}, r))
            return true;
    return false;
  };
  auto room_for_big = [&](int i, Point q, int r) {
    R.add(i, q, r, +1);
    bool ok = true;
    for (int j : work)
      if (j != i && big[z(j)] && !placed[z(j)] && !has_spot(j)) {
        ok = false;
        break;
      }
    R.add(i, q, r, -1);
    return ok;
  };

  // Nearest legal position (lattice rings around the target, all four rotations, own rotation first).
  auto search = [&](int i, Point& at, int& at_rot) {
    int lookahead_rejects = 0;
    bool have_fallback = false;
    Point fallback{};
    int fallback_rot = 0;
    for (int k = 0; k < 4; ++k) {
      const int r = (pl.rot[z(i)] + k) & 3;
      long budget = 400;
      for (const Point d : offs) {
        if (only_illegal && static_cast<geom::i128>(d.x) * d.x + static_cast<geom::i128>(d.y) * d.y > reach2) break;
        const Point q = target[z(i)] + d;
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
        if (ok && big[z(i)] && lookahead_rejects < 100 && !room_for_big(i, q, r)) {
          ++lookahead_rejects;
          ++st.lookahead_rejects;
          if (!have_fallback) {
            fallback = q;
            fallback_rot = r;
            have_fallback = true;
          }
          ok = false;
        }
        if (ok) {
          at = q;
          at_rot = r;
          return true;
        }
      }
    }
    if (have_fallback) {  // every spot starves some other big part: take the nearest legal one anyway
      at = fallback;
      at_rot = fallback_rot;
      return true;
    }
    return false;
  };
  // Full mode, no free spot: take the spot whose conflicting movable parts have the least total area (never a
  // fixed part), evict them and queue them again (rip-up and re-place).
  auto evict_for = [&](int i, Point& at, int& at_rot) {
    double best = 1e300;
    std::vector<int> best_conf;
    int tried = 0;
    for (const Point d : offs) {
      if (++tried > 1500) break;
      const Point q = target[z(i)] + d;
      for (int r = 0; r < 4; ++r) {
        if (!L.inside_ok(i, q, r)) continue;
        conflicts.clear();
        L.conflicts(i, q, r, conflicts);
        double cost = 0;
        bool ok = !conflicts.empty();
        for (int c2 : conflicts) {
          if (!p.parts[z(c2)].movable || evictions[z(c2)] >= 2) ok = false;
          cost += static_cast<double>(p.parts[z(c2)].area);
        }
        if (!ok) continue;
        cost += static_cast<double>(p.parts[z(i)].area) * 1e-3 * std::hypot(static_cast<double>(d.x), static_cast<double>(d.y)) / 1e6;
        if (cost < best) {
          best = cost;
          best_conf = conflicts;
          at = q;
          at_rot = r;
        }
      }
    }
    if (best_conf.empty()) return false;
    for (int c2 : best_conf) {
      L.remove(c2);
      R.add(c2, pl.pos[z(c2)], pl.rot[z(c2)], -1);
      if (placed[z(c2)]) --st.placed;
      placed[z(c2)] = 0;
      ++evictions[z(c2)];
      work.push_back(c2);
      --eviction_budget;
    }
    ++st.evictions;
    return true;
  };

  while (!work.empty()) {
    const int i = work.front();
    work.pop_front();
    if (only_illegal) {
      L.remove(i);
      R.add(i, home[z(i)], home_rot[z(i)], -1);
    }
    Point at{};
    int at_rot = pl.rot[z(i)];
    bool found = search(i, at, at_rot);
    if (!found && !only_illegal && eviction_budget > 0) found = evict_for(i, at, at_rot);
    if (!found) {
      // Back to where it was (refine: its reserved spot; full: the input position, reported as a failure).
      pl.pos[z(i)] = only_illegal ? home[z(i)] : p.parts[z(i)].pos0;
      pl.rot[z(i)] = only_illegal ? home_rot[z(i)] : 0;
      L.insert(i, pl.pos[z(i)], pl.rot[z(i)]);  // still an obstacle for the parts that follow
      R.add(i, pl.pos[z(i)], pl.rot[z(i)], +1);
      continue;
    }
    pl.pos[z(i)] = at;
    pl.rot[z(i)] = static_cast<std::uint8_t>(at_rot);
    L.insert(i, at, at_rot);
    R.add(i, at, at_rot, +1);
    placed[z(i)] = 1;
    ++st.placed;
  }
  for (int i : order)
    if (!placed[z(i)]) {
      ++st.failed;
      st.failures.push_back(p.parts[z(i)].ref);
      st.failed_parts.push_back(i);
    }
  double sum_disp = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (!placed[i]) continue;
    const double disp = std::hypot(static_cast<double>(pl.pos[i].x - target[i].x), static_cast<double>(pl.pos[i].y - target[i].y)) / 1e6;
    st.max_disp_mm = std::max(st.max_disp_mm, disp);
    sum_disp += disp;
  }
  st.mean_disp_mm = st.placed ? sum_disp / st.placed : 0.0;
  return st;
}

}  // namespace tmk::place

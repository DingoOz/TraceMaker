// SPDX-License-Identifier: GPL-3.0-or-later
#include "geom/clip.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <clipper2/clipper.h>

namespace tmk::geom {

namespace {

namespace c2 = Clipper2Lib;

c2::Paths64 to_c2(const Rings& rs) {
  c2::Paths64 out;
  out.reserve(rs.size());
  for (const auto& r : rs) {
    c2::Path64 p;
    p.reserve(r.size());
    for (const auto& q : r) p.emplace_back(q.x, q.y);
    out.push_back(std::move(p));
  }
  return out;
}

Ring from_c2(const c2::Path64& p) {
  Ring r;
  r.reserve(p.size());
  for (const auto& q : p) r.push_back({q.x, q.y});
  return r;
}

Rings from_c2(const c2::Paths64& ps) {
  Rings out;
  out.reserve(ps.size());
  for (const auto& p : ps)
    if (p.size() >= 3) out.push_back(from_c2(p));
  return out;
}

Rings boolean(c2::ClipType op, c2::FillRule rule, const Rings& a, const Rings& b) {
  return from_c2(c2::BooleanOp(op, rule, to_c2(a), to_c2(b)));
}

// Clipper2 rounds joins with chords on the true arc and offsets straight edges exactly. Growing by a tolerance
// puts every chord outside the arc; a tolerance of at most 1 µm keeps straight edges within 1 µm of exact
// (KiCad's ERROR_OUTSIDE moves only the arcs).
c2::Paths64 grow_round(const c2::Paths64& ps, c2::EndType end, Coord delta, Coord max_error) {
  const Coord tol = std::min<Coord>(max_error, 1'000);
  return c2::InflatePaths(ps, static_cast<double>(delta + tol), c2::JoinType::Round, end, 2.0, static_cast<double>(tol));
}

}  // namespace

Rings unite(const Rings& a) { return boolean(c2::ClipType::Union, c2::FillRule::NonZero, a, {}); }
Rings subtract(const Rings& a, const Rings& b) { return boolean(c2::ClipType::Difference, c2::FillRule::NonZero, a, b); }
Rings intersect(const Rings& a, const Rings& b) { return boolean(c2::ClipType::Intersection, c2::FillRule::NonZero, a, b); }
Rings even_odd(const Rings& loops) { return boolean(c2::ClipType::Union, c2::FillRule::EvenOdd, loops, {}); }

Rings offset(const Rings& a, Coord delta, Corners corners, Coord max_error) {
  if (delta == 0) return a;
  const auto jt = corners == Corners::Round ? c2::JoinType::Round : c2::JoinType::Square;
  return from_c2(c2::InflatePaths(to_c2(a), static_cast<double>(delta), jt, c2::EndType::Polygon, 2.0, static_cast<double>(max_error)));
}

Ring circle_ring(Point centre, Coord radius, Coord max_error) {
  // n segments whose chords stay within max_error of the circle; vertices on radius / cos(pi / n) keep every
  // chord outside it.
  const double r = static_cast<double>(radius);
  const double err = std::max<double>(1.0, static_cast<double>(max_error));
  int n = 8;
  if (r > err) n = std::max(8, static_cast<int>(std::ceil(std::numbers::pi / std::acos(1.0 - err / r))));
  n = (n + 3) / 4 * 4;  // symmetric about both axes
  const double rv = r / std::cos(std::numbers::pi / n);
  Ring out;
  out.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    const double a = 2 * std::numbers::pi * i / n;
    out.push_back({centre.x + std::llround(rv * std::cos(a)), centre.y + std::llround(rv * std::sin(a))});
  }
  return out;
}

Rings shape_rings(const Shape& s, Coord grow, Coord max_error) {
  const Coord r = s.r + grow;
  if (s.pts.empty() || r < 0) return {};
  if (s.pts.size() == 1) return r > 0 ? Rings{circle_ring(s.pts.front(), r, max_error)} : Rings{};
  if (s.closed) {
    if (r == 0) return unite({s.pts});
    return from_c2(grow_round(to_c2({s.pts}), c2::EndType::Polygon, r, max_error));
  }
  if (r == 0) return {};
  return from_c2(grow_round(to_c2({s.pts}), c2::EndType::Round, r, max_error));
}

double area(const Ring& r) {
  c2::Path64 p;
  p.reserve(r.size());
  for (const auto& q : r) p.emplace_back(q.x, q.y);
  return c2::Area(p);
}

double area(const Rings& rs) {
  double a = 0;
  for (const auto& r : rs) a += area(r);
  return a;
}

bool contains(const Rings& rs, Point p) {
  int winding = 0;
  const c2::Point64 q(p.x, p.y);
  for (const auto& r : rs) {
    c2::Path64 path;
    path.reserve(r.size());
    for (const auto& v : r) path.emplace_back(v.x, v.y);
    const auto res = c2::PointInPolygon(q, path);
    if (res == c2::PointInPolygonResult::IsOn) return true;
    if (res == c2::PointInPolygonResult::IsInside) winding += c2::Area(path) > 0 ? 1 : -1;
  }
  return winding > 0;
}

std::vector<Polygon> polygons(const Rings& rs) {
  c2::PolyTree64 tree;
  c2::BooleanOp(c2::ClipType::Union, c2::FillRule::NonZero, to_c2(rs), {}, tree);
  std::vector<Polygon> out;
  // Outer rings at even depths, their holes as children; islands inside holes are outers again.
  std::vector<const c2::PolyPath64*> stack{&tree};
  while (!stack.empty()) {
    const auto* node = stack.back();
    stack.pop_back();
    for (std::size_t i = 0; i < node->Count(); ++i) {
      const auto* outer = node->Child(i);
      Polygon poly;
      poly.outer = from_c2(outer->Polygon());
      for (std::size_t k = 0; k < outer->Count(); ++k) {
        const auto* hole = outer->Child(k);
        poly.holes.push_back(from_c2(hole->Polygon()));
        stack.push_back(hole);
      }
      out.push_back(std::move(poly));
    }
  }
  return out;
}

Ring fracture(const Polygon& p) {
  Ring ring = p.outer;
  std::vector<std::pair<std::size_t, std::size_t>> order;  // (hole, index of its leftmost vertex)
  for (std::size_t h = 0; h < p.holes.size(); ++h) {
    const auto& hole = p.holes[h];
    if (hole.size() < 3) continue;
    std::size_t lm = 0;
    for (std::size_t i = 1; i < hole.size(); ++i)
      if (hole[i].x < hole[lm].x || (hole[i].x == hole[lm].x && hole[i].y < hole[lm].y)) lm = i;
    order.emplace_back(h, lm);
  }
  std::stable_sort(order.begin(), order.end(), [&](const auto& a, const auto& b) {
    const Point pa = p.holes[a.first][a.second], pb = p.holes[b.first][b.second];
    return pa.x != pb.x ? pa.x < pb.x : pa.y < pb.y;
  });
  for (const auto& [h, lm] : order) {
    const auto& hole = p.holes[h];
    const Point s = hole[lm];
    // Nearest crossing of the ray from s towards -x with the current ring.
    std::size_t best = ring.size();
    double best_x = 0;
    for (std::size_t i = 0; i < ring.size(); ++i) {
      const Point a = ring[i], b = ring[(i + 1) % ring.size()];
      if ((a.y > s.y) == (b.y > s.y) && a.y != s.y && b.y != s.y) continue;
      double x;
      if (a.y == b.y) {
        if (a.y != s.y) continue;
        x = static_cast<double>(std::min(std::max(a.x, b.x), s.x));
        if (x < static_cast<double>(std::min(a.x, b.x))) continue;
      } else {
        if ((s.y < std::min(a.y, b.y)) || (s.y > std::max(a.y, b.y))) continue;
        x = static_cast<double>(a.x) + static_cast<double>(s.y - a.y) * static_cast<double>(b.x - a.x) / static_cast<double>(b.y - a.y);
      }
      if (x > static_cast<double>(s.x)) continue;
      if (best == ring.size() || x > best_x) best = i, best_x = x;
    }
    if (best == ring.size()) continue;  // not inside the outline (cannot happen for a valid polygon)
    const Point q{std::llround(best_x), s.y};
    // ring[0..best], q, the hole from s round to s, q, ring[best+1..]: both sides of the bridge are one segment.
    Ring next;
    next.reserve(ring.size() + hole.size() + 3);
    next.insert(next.end(), ring.begin(), ring.begin() + static_cast<std::ptrdiff_t>(best + 1));
    next.push_back(q);
    for (std::size_t k = 0; k <= hole.size(); ++k) next.push_back(hole[(lm + k) % hole.size()]);
    next.push_back(q);
    next.insert(next.end(), ring.begin() + static_cast<std::ptrdiff_t>(best + 1), ring.end());
    ring = std::move(next);
  }
  ring.erase(std::unique(ring.begin(), ring.end()), ring.end());
  while (ring.size() > 1 && ring.front() == ring.back()) ring.pop_back();
  return ring;
}

}  // namespace tmk::geom

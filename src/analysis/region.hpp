// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The reference copper of one layer as a queryable region (issue #7, docs/05-routing.md §42): the geometry of the return-path audit.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

#include "geom/clip.hpp"
#include "geom/point.hpp"

namespace tmk::analysis::detail {

using geom::Point;
using geom::Ring;
using geom::Rings;

struct Pt {
  double x = 0, y = 0;
};
inline Pt to_pt(Point p) { return {static_cast<double>(p.x), static_cast<double>(p.y)}; }
inline Point to_point(Pt p) { return {static_cast<Coord>(std::llround(p.x)), static_cast<Coord>(std::llround(p.y))}; }
inline double dist(Pt a, Pt b) { return std::hypot(a.x - b.x, a.y - b.y); }

// The reference copper of one layer: the region under the non-zero rule (islands with their holes). Its edges and vertices are bucketed in
// grids so that "is this point on copper?", "can a segment cross it?" and "which vertices are near?" cost the neighbourhood and not the whole
// pour (the plane of a large board has hundreds of thousands of edges, most of them antipads), and the vertices a shortest path can bend at.
class Region {
 public:
  Region() = default;
  explicit Region(const Rings& rings) {
    int island = 0;
    for (const auto& poly : geom::polygons(rings)) {
      auto add = [&](const Ring& r) {
        for (std::size_t i = 0; i < r.size(); ++i) {
          edges_.push_back({to_pt(r[i]), to_pt(r[(i + 1) % r.size()]), island});
          verts_.push_back(to_pt(r[i]));
        }
      };
      add(poly.outer);
      for (const auto& h : poly.holes) add(h);
      ++island;
    }
    for (const auto& e : edges_) {
      box_.add(to_point(e.a));
      box_.add(to_point(e.b));
    }
    if (box_.empty()) return;
    nx_ = static_cast<int>((box_.x1 - box_.x0) / kCell) + 1;
    ny_ = static_cast<int>((box_.y1 - box_.y0) / kCell) + 1;
    nrows_ = static_cast<int>((box_.y1 - box_.y0) / kRow) + 1;
    cells_.assign(static_cast<std::size_t>(nx_) * static_cast<std::size_t>(ny_), {});
    vcells_.assign(cells_.size(), {});
    rows_.assign(static_cast<std::size_t>(nrows_), {});
    for (std::size_t i = 0; i < edges_.size(); ++i) {
      const Edge& e = edges_[i];
      const int x0 = cell_x(std::min(e.a.x, e.b.x)), x1 = cell_x(std::max(e.a.x, e.b.x));
      const int y0 = cell_y(std::min(e.a.y, e.b.y)), y1 = cell_y(std::max(e.a.y, e.b.y));
      for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx) cells_[idx(cx, cy)].push_back(static_cast<int>(i));
      if (e.a.y == e.b.y) continue;  // a horizontal edge never crosses a horizontal ray
      for (int r = row_of(std::min(e.a.y, e.b.y)); r <= row_of(std::max(e.a.y, e.b.y)); ++r) rows_[static_cast<std::size_t>(r)].push_back(static_cast<int>(i));
    }
    for (std::size_t i = 0; i < verts_.size(); ++i) vcells_[idx(cell_x(verts_[i].x), cell_y(verts_[i].y))].push_back(static_cast<int>(i));
    stamp_.assign(edges_.size(), 0);
  }

  bool empty() const { return edges_.empty(); }

  // Inside the region (on its boundary counts as inside: a return may run along it).
  bool contains(Pt p) const { return locate(p) >= 0; }
  // Which island of copper (an outer ring with its holes) the point is on, -1 if none: two points on different islands have no path between them.
  int island_of(Pt p) const { return locate(p); }

  // The vertices a path no longer than `bound` between a and b could bend at: inside the ellipse with foci a and b.
  void vertices_near(Pt a, Pt b, double bound, std::vector<Pt>& out) const {
    if (box_.empty()) return;
    const double cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2, r = bound / 2;
    const int x0 = cell_x(cx - r), x1 = cell_x(cx + r), y0 = cell_y(cy - r), y1 = cell_y(cy + r);
    for (int yy = y0; yy <= y1; ++yy)
      for (int xx = x0; xx <= x1; ++xx)
        for (int vi : vcells_[idx(xx, yy)]) {
          const Pt& v = verts_[static_cast<std::size_t>(vi)];
          if (dist(a, v) + dist(v, b) <= bound) out.push_back(v);
        }
  }

  // Is the open segment a-b free of properly crossing edges? Touching an edge at a vertex or running along it is allowed.
  bool crosses(Pt a, Pt b) const {
    if (box_.empty()) return false;
    ++clock_;
    const int x0 = cell_x(std::min(a.x, b.x)), x1 = cell_x(std::max(a.x, b.x)), y0 = cell_y(std::min(a.y, b.y)), y1 = cell_y(std::max(a.y, b.y));
    for (int cy = y0; cy <= y1; ++cy)
      for (int cx = x0; cx <= x1; ++cx)
        for (int ei : cells_[idx(cx, cy)]) {
          if (stamp_[static_cast<std::size_t>(ei)] == clock_) continue;
          stamp_[static_cast<std::size_t>(ei)] = clock_;
          if (proper_cross(a, b, edges_[static_cast<std::size_t>(ei)].a, edges_[static_cast<std::size_t>(ei)].b)) return true;
        }
    return false;
  }

  // The segment lies in the region: no edge crosses it, and every piece of it between two touches of the boundary is inside (a segment
  // that runs along the boundary and over a gap in it would pass a midpoint test).
  bool sees(Pt a, Pt b) const {
    if (crosses(a, b)) return false;
    std::vector<double> ts{0.0, 1.0};
    const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
    if (len2 < 1.0) return contains(a);
    auto along = [&](Pt p, double& t) {  // p on the segment (within 3 nm)? its parameter
      t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
      if (t <= 0 || t >= 1) return false;
      return dist(p, {a.x + t * dx, a.y + t * dy}) < 3.0;
    };
    if (!box_.empty()) {
      ++clock_;
      const int x0 = cell_x(std::min(a.x, b.x)), x1 = cell_x(std::max(a.x, b.x)), y0 = cell_y(std::min(a.y, b.y)), y1 = cell_y(std::max(a.y, b.y));
      for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx)
          for (int ei : cells_[idx(cx, cy)]) {
            if (stamp_[static_cast<std::size_t>(ei)] == clock_) continue;
            stamp_[static_cast<std::size_t>(ei)] = clock_;
            const Edge& e = edges_[static_cast<std::size_t>(ei)];
            double t = 0;
            if (along(e.a, t)) ts.push_back(t);
            if (along(e.b, t)) ts.push_back(t);
            const double ex = e.b.x - e.a.x, ey = e.b.y - e.a.y, den = dx * ey - dy * ex;
            if (std::fabs(den) > 1e-9 * std::sqrt(len2) * std::hypot(ex, ey)) {
              const double tt = ((e.a.x - a.x) * ey - (e.a.y - a.y) * ex) / den, uu = ((e.a.x - a.x) * dy - (e.a.y - a.y) * dx) / den;
              if (tt > 0 && tt < 1 && uu >= 0 && uu <= 1) ts.push_back(tt);
            }
          }
    }
    std::sort(ts.begin(), ts.end());
    for (std::size_t i = 1; i < ts.size(); ++i) {
      if ((ts[i] - ts[i - 1]) * std::sqrt(len2) < 2.0) continue;  // a piece shorter than 2 nm
      const double m = (ts[i] + ts[i - 1]) / 2;
      if (!contains({a.x + m * dx, a.y + m * dy})) return false;
    }
    return true;
  }

  // The point of the region's boundary nearest to p, within `reach`; false if none. Cells are searched in growing squares. Several edges can be
  // exactly as near (a pad at the centre of a round antipad): the one that comes first in the region's edge order wins, so the answer does not
  // depend on the order the grid is walked in.
  bool nearest_boundary(Pt p, double reach, Pt& out) const {
    if (box_.empty()) return false;
    double best = reach;
    int best_edge = -1;
    const int c0x = cell_x(p.x), c0y = cell_y(p.y), maxr = static_cast<int>(reach / static_cast<double>(kCell)) + 1;
    for (int r = 0; r <= maxr; ++r) {
      for (int cy = std::max(0, c0y - r); cy <= std::min(ny_ - 1, c0y + r); ++cy)
        for (int cx = std::max(0, c0x - r); cx <= std::min(nx_ - 1, c0x + r); ++cx) {
          if (std::max(std::abs(cx - c0x), std::abs(cy - c0y)) != r) continue;  // the new ring of cells only
          for (int ei : cells_[idx(cx, cy)]) {
            Pt q;
            const double d = distance_to_edge(edges_[static_cast<std::size_t>(ei)], p, q);
            if (d < best || (best_edge >= 0 && d == best && ei < best_edge)) best = d, best_edge = ei, out = q;
          }
        }
      if (best_edge >= 0 && best <= static_cast<double>(r) * static_cast<double>(kCell)) break;  // nothing in a farther ring can be nearer
    }
    return best_edge >= 0;
  }

  // Reference paths (project rule: every accelerated path has one): the same answers by scanning every edge and vertex. Only the tests call
  // these.
  int island_of_linear(Pt p) const {
    for (const auto& e : edges_)
      if (on_edge(e, p)) return e.island;
    bool in = false;
    double nearest = std::numeric_limits<double>::infinity();
    int island = -1;
    for (const auto& e : edges_) {
      if ((e.a.y > p.y) == (e.b.y > p.y)) continue;
      const double x = (e.b.x - e.a.x) * (p.y - e.a.y) / (e.b.y - e.a.y) + e.a.x;
      if (x <= p.x) continue;
      in = !in;
      if (x < nearest) nearest = x, island = e.island;
    }
    return in ? island : -1;
  }
  bool crosses_linear(Pt a, Pt b) const {
    for (const auto& e : edges_)
      if (proper_cross(a, b, e.a, e.b)) return true;
    return false;
  }
  bool nearest_boundary_linear(Pt p, double reach, Pt& out) const {
    double best = reach;
    bool found = false;
    for (const auto& e : edges_) {
      Pt q;
      if (const double d = distance_to_edge(e, p, q); d < best) best = d, out = q, found = true;
    }
    return found;
  }
  std::size_t vertices_near_linear(Pt a, Pt b, double bound) const {
    std::size_t n = 0;
    for (const auto& v : verts_)
      if (dist(a, v) + dist(v, b) <= bound) ++n;
    return n;
  }

 private:
  static constexpr Coord kCell = 1'000'000;  // grid of edges and vertices
  static constexpr Coord kRow = 250'000;     // rows for the ray of the inside test: finer, a ray meets every edge of its row
  struct Edge {
    Pt a, b;
    int island = 0;
  };
  // The distance from p to the edge, and the nearest point of it.
  static double distance_to_edge(const Edge& e, Pt p, Pt& q) {
    const double dx = e.b.x - e.a.x, dy = e.b.y - e.a.y, l2 = dx * dx + dy * dy;
    const double t = l2 > 0 ? std::clamp(((p.x - e.a.x) * dx + (p.y - e.a.y) * dy) / l2, 0.0, 1.0) : 0.0;
    q = {e.a.x + t * dx, e.a.y + t * dy};
    return dist(p, q);
  }
  static bool on_edge(const Edge& e, Pt p) {  // within 3 nm of the edge: the boundary
    const double dx = e.b.x - e.a.x, dy = e.b.y - e.a.y, l2 = dx * dx + dy * dy;
    const double t = l2 > 0 ? std::clamp(((p.x - e.a.x) * dx + (p.y - e.a.y) * dy) / l2, 0.0, 1.0) : 0.0;
    return dist(p, {e.a.x + t * dx, e.a.y + t * dy}) < 3.0;
  }
  static double orient(Pt a, Pt b, Pt c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }
  // Strictly inside both segments (a touch at an end, or running along one, is not a crossing).
  static bool proper_cross(Pt a, Pt b, Pt c, Pt d) {
    const double eps = 1e-3 * std::max(1.0, std::max(dist(a, b), dist(c, d)));  // nm^2 scale: 1e-3 of the longer length
    const double o1 = orient(a, b, c), o2 = orient(a, b, d), o3 = orient(c, d, a), o4 = orient(c, d, b);
    return ((o1 > eps && o2 < -eps) || (o1 < -eps && o2 > eps)) && ((o3 > eps && o4 < -eps) || (o3 < -eps && o4 > eps));
  }
  std::size_t idx(int cx, int cy) const { return static_cast<std::size_t>(cy) * static_cast<std::size_t>(nx_) + static_cast<std::size_t>(cx); }
  int cell_x(double x) const { return std::clamp(static_cast<int>((x - static_cast<double>(box_.x0)) / static_cast<double>(kCell)), 0, nx_ - 1); }
  int cell_y(double y) const { return std::clamp(static_cast<int>((y - static_cast<double>(box_.y0)) / static_cast<double>(kCell)), 0, ny_ - 1); }
  int row_of(double y) const { return std::clamp(static_cast<int>((y - static_cast<double>(box_.y0)) / static_cast<double>(kRow)), 0, nrows_ - 1); }

  // The island of copper the point is on, or -1. A point within 3 nm of an edge is on the boundary (that edge's island). Otherwise a ray to
  // +x counts the edges it crosses (odd = inside; rings nest properly, so the parity of all rings is "in an outer ring and in no hole"),
  // and the first edge it meets belongs to the island the point is in.
  int locate(Pt p) const {
    if (box_.empty() || p.x < static_cast<double>(box_.x0) - 3 || p.x > static_cast<double>(box_.x1) + 3 || p.y < static_cast<double>(box_.y0) - 3 ||
        p.y > static_cast<double>(box_.y1) + 3)
      return -1;
    const int cx = cell_x(p.x), cy = cell_y(p.y);
    for (int yy = std::max(0, cy - 1); yy <= std::min(ny_ - 1, cy + 1); ++yy)
      for (int xx = std::max(0, cx - 1); xx <= std::min(nx_ - 1, cx + 1); ++xx)
        for (int ei : cells_[idx(xx, yy)])
          if (on_edge(edges_[static_cast<std::size_t>(ei)], p)) return edges_[static_cast<std::size_t>(ei)].island;
    bool in = false;
    double nearest = std::numeric_limits<double>::infinity();
    int island = -1;
    for (int ei : rows_[static_cast<std::size_t>(row_of(p.y))]) {
      const Edge& e = edges_[static_cast<std::size_t>(ei)];
      if ((e.a.y > p.y) == (e.b.y > p.y)) continue;
      const double x = (e.b.x - e.a.x) * (p.y - e.a.y) / (e.b.y - e.a.y) + e.a.x;
      if (x <= p.x) continue;
      in = !in;
      if (x < nearest) nearest = x, island = e.island;
    }
    return in ? island : -1;
  }

  std::vector<Edge> edges_;
  std::vector<Pt> verts_;
  geom::Box box_;
  int nx_ = 0, ny_ = 0, nrows_ = 0;
  std::vector<std::vector<int>> cells_, vcells_, rows_;
  mutable std::vector<std::uint32_t> stamp_;
  mutable std::uint32_t clock_ = 0;
};


}  // namespace tmk::analysis::detail

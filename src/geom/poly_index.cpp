// SPDX-License-Identifier: GPL-3.0-or-later
#include "geom/poly_index.hpp"

#include <algorithm>
#include <cmath>

namespace tmk::geom {

PolygonIndex::PolygonIndex(const std::vector<Point>& pts) : pts_(pts) {
  for (const auto& p : pts_) box_.add(p);
  if (pts_.size() < 3 || box_.empty()) return;
  // About 8 edges per occupied cell on average; at least 0.1 mm so long edges do not span absurdly many cells.
  const double w = static_cast<double>(box_.x1 - box_.x0) + 1, h = static_cast<double>(box_.y1 - box_.y0) + 1;
  const double target = std::max(1.0, static_cast<double>(pts_.size()) / 8.0);
  cell_ = std::max<Coord>(100'000, static_cast<Coord>(std::sqrt(w * h / target)));
  nx_ = static_cast<int>((box_.x1 - box_.x0) / cell_) + 1;
  ny_ = static_cast<int>((box_.y1 - box_.y0) / cell_) + 1;
  cells_.assign(static_cast<std::size_t>(nx_) * static_cast<std::size_t>(ny_), {});
  rows_.assign(static_cast<std::size_t>(ny_), {});
  const std::size_t n = pts_.size();
  for (std::size_t i = 0; i < n; ++i) {
    const Point a = pts_[i], b = pts_[(i + 1) % n];
    const int y0 = cell_y(std::min(a.y, b.y)), y1 = cell_y(std::max(a.y, b.y));
    const int x0 = cell_x(std::min(a.x, b.x)), x1 = cell_x(std::max(a.x, b.x));
    for (int y = y0; y <= y1; ++y) {
      rows_[static_cast<std::size_t>(y)].push_back(static_cast<int>(i));
      for (int x = x0; x <= x1; ++x) cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(nx_) + static_cast<std::size_t>(x)].push_back(static_cast<int>(i));
    }
  }
}

int PolygonIndex::cell_x(Coord x) const { return std::clamp(static_cast<int>((x - box_.x0) / cell_), 0, nx_ - 1); }
int PolygonIndex::cell_y(Coord y) const { return std::clamp(static_cast<int>((y - box_.y0) / cell_), 0, ny_ - 1); }

// Crossing parity of a ray to +x, over the edges of the point's row (half-open in y). A point on the boundary may
// count either way; it is then at distance 0 and the callers' edge tests decide.
bool PolygonIndex::inside(Point c) const {
  if (pts_.size() < 3 || c.y < box_.y0 || c.y > box_.y1) return false;
  const std::size_t n = pts_.size();
  bool in = false;
  for (const int i : rows_[static_cast<std::size_t>(cell_y(c.y))]) {
    const Point a = pts_[static_cast<std::size_t>(i)], b = pts_[(static_cast<std::size_t>(i) + 1) % n];
    if ((a.y > c.y) == (b.y > c.y)) continue;
    const i128 o = orient(a, b, c);
    if (b.y > a.y ? o > 0 : o < 0) in = !in;
  }
  return in;
}

template <class F>
bool PolygonIndex::near_edges(const Shape& s, Coord reach, F&& f) const {
  const std::size_t n = pts_.size(), m = s.pts.size();
  auto edge = [&](Point p, Point q) {
    const int x0 = cell_x(std::min(p.x, q.x) - reach), x1 = cell_x(std::max(p.x, q.x) + reach);
    const int y0 = cell_y(std::min(p.y, q.y) - reach), y1 = cell_y(std::max(p.y, q.y) + reach);
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
        for (const int i : cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(nx_) + static_cast<std::size_t>(x)])
          if (f(p, q, pts_[static_cast<std::size_t>(i)], pts_[(static_cast<std::size_t>(i) + 1) % n])) return true;
    return false;
  };
  if (m == 1) return edge(s.pts[0], s.pts[0]);
  for (std::size_t i = 0; i + 1 < m; ++i)
    if (edge(s.pts[i], s.pts[i + 1])) return true;
  return s.closed && m >= 3 && edge(s.pts[m - 1], s.pts[0]);
}

bool PolygonIndex::shape_closer(const Shape& s, Coord clearance) const {
  if (pts_.size() < 3 || s.pts.empty()) return false;
  const Coord t = clearance + s.r;
  if (t <= 0 || !s.box.inflated(clearance).intersects(box_)) return false;
  if (s.closed && point_in_polygon(pts_[0], s.pts)) return true;
  if (inside(s.pts[0])) return true;
  return near_edges(s, t, [&](Point p, Point q, Point a, Point b) { return seg_seg_closer(p, q, a, b, t); });
}

double PolygonIndex::shape_gap(const Shape& s, Coord bound) const {
  if ((s.closed && point_in_polygon(pts_[0], s.pts)) || inside(s.pts[0])) return -static_cast<double>(s.r);
  long double best = 1e30L;
  near_edges(s, bound, [&](Point p, Point q, Point a, Point b) {
    best = std::min(best, seg_seg_dist(p, q, a, b));
    return false;
  });
  return static_cast<double>(best) - static_cast<double>(s.r);
}

bool PolygonIndex::disk_closer(Point c, Coord r, Coord clearance) const {
  if (pts_.size() < 3) return false;
  const Coord t = clearance + r;
  const Box disk{c.x - r, c.y - r, c.x + r, c.y + r};
  if (t <= 0 || !disk.inflated(clearance).intersects(box_)) return false;
  const std::size_t n = pts_.size();
  if (inside(c)) return true;
  // Edges within reach of the disk.
  const int x0 = cell_x(c.x - t), x1 = cell_x(c.x + t), y0 = cell_y(c.y - t), y1 = cell_y(c.y + t);
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x)
      for (const int i : cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(nx_) + static_cast<std::size_t>(x)])
        if (seg_seg_closer(c, c, pts_[static_cast<std::size_t>(i)], pts_[(static_cast<std::size_t>(i) + 1) % n], t)) return true;
  return false;
}

}  // namespace tmk::geom

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Bucketed edges of one large closed polygon (a zone fill), so "does this disk touch the polygon?" costs the
// edges near the disk instead of every edge. Exact: the answer equals
// closer_than(Shape::point(c, r), Shape::polygon(pts, 0), clearance) (reference path; tests/test_geom.cpp).
#include <vector>

#include "geom/shape.hpp"

namespace tmk::geom {

class PolygonIndex {
 public:
  PolygonIndex() = default;
  explicit PolygonIndex(const std::vector<Point>& pts);

  // True if the gap between the disk (centre c, radius r) and the polygon is strictly less than `clearance`
  // (clearance + r > 0).
  bool disk_closer(Point c, Coord r, Coord clearance) const;

  // Any shape against the polygon. shape_closer(s, clearance) equals closer_than(s, Shape::polygon(pts, 0),
  // clearance); shape_gap(s, bound) equals gap(s, Shape::polygon(pts, 0)) when the two cores are closer than
  // `bound` (the caller knows that from shape_closer). Both visit only the polygon edges near the shape; the
  // linear functions are the reference path (tests/test_geom.cpp).
  bool shape_closer(const Shape& s, Coord clearance) const;
  double shape_gap(const Shape& s, Coord bound) const;

 private:
  bool inside(Point c) const;  // crossing parity; a point on the boundary may count either way
  // Calls f(p, q, a, b) for every core edge pq of s and every polygon edge ab in a cell within `reach` of pq
  // (an edge may come up more than once); stops and returns true when f does.
  template <class F>
  bool near_edges(const Shape& s, Coord reach, F&& f) const;
  int cell_x(Coord x) const;
  int cell_y(Coord y) const;

  std::vector<Point> pts_;
  Box box_;
  Coord cell_ = 1;
  int nx_ = 0, ny_ = 0;
  std::vector<std::vector<int>> cells_;  // edges (index of the start point) overlapping each cell, row-major
  std::vector<std::vector<int>> rows_;   // edges whose y-range overlaps each row of cells (crossing counts)
};

}  // namespace tmk::geom

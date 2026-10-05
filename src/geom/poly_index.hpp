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

 private:
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

#pragma once
// Exact integer geometry for clearance checks (design doc 03 §1).
//
// Every copper shape is a "rounded core": a point, an open polyline or a closed polygon, inflated by a
// radius r. Tracks are 2-point polylines with r = width/2, round pads are points, ovals are segments,
// round-rect pads are rectangles shrunk by the corner radius and inflated back. Gap tests compare squared
// distances in 128-bit integers, so "closer than" decisions are exact.
#include <cstdint>
#include <vector>

#include "geom/point.hpp"

namespace tmk::geom {

__extension__ using i128 = __int128;  // GCC extension; exact products of nm coordinates

struct Shape {
  std::vector<Point> pts;  // 1 point, open polyline (>= 2), or closed polygon (>= 3)
  Coord r = 0;             // inflation radius
  bool closed = false;     // polygon interior is part of the shape
  Box box;                 // bounding box including r

  static Shape point(Point p, Coord r);
  static Shape segment(Point a, Point b, Coord r);
  static Shape polyline(std::vector<Point> pts, Coord r);
  static Shape polygon(std::vector<Point> pts, Coord r = 0);
  void update_box();
};

// Orientation of c relative to a→b: >0 left (counter-clockwise in y-up math terms), <0 right, 0 collinear.
inline i128 orient(Point a, Point b, Point c) {
  return static_cast<i128>(b.x - a.x) * (c.y - a.y) - static_cast<i128>(b.y - a.y) * (c.x - a.x);
}

// True if closed segments ab and cd share at least one point.
bool segments_intersect(Point a, Point b, Point c, Point d);

// True if the distance from p to segment ab is strictly less than t (t >= 0).
bool point_seg_closer(Point p, Point a, Point b, Coord t);

// True if the distance between segments ab and cd is strictly less than t (t >= 0).
bool seg_seg_closer(Point a, Point b, Point c, Point d, Coord t);

// Squared distances, for reporting (long double).
long double point_seg_dist(Point p, Point a, Point b);
long double seg_seg_dist(Point a, Point b, Point c, Point d);

// Point in closed polygon (boundary counts as inside).
bool point_in_polygon(Point p, const std::vector<Point>& poly);

// True if the gap between the two shapes (distance between their surfaces, negative when they overlap) is
// strictly less than `clearance`. clearance may be 0 (then: do they overlap or touch? no — strictly overlap).
bool closer_than(const Shape& a, const Shape& b, Coord clearance);

// Gap between two shapes in nm (negative = overlap depth bound; 0 when cores touch with r = 0).
double gap(const Shape& a, const Shape& b);

// Flattens a circular arc through start/mid/end into points with sagitta error <= max_error.
std::vector<Point> arc_points(Point start, Point mid, Point end, Coord max_error = 5'000);
// Circle outline as a closed polyline (first point repeated at the end).
std::vector<Point> circle_points(Point centre, Coord radius, Coord max_error = 5'000);

}  // namespace tmk::geom

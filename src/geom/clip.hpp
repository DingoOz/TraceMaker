// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Polygon booleans and offsets on integer nanometres, a thin wrapper over Clipper2 (Angus Johnson, 2022; the
// Vatti scan-beam clipper, 1992). Used for zone fills (doc 05 §36). Results depend only on the inputs and
// their order, so callers keep inputs in board order.
#include <vector>

#include "geom/point.hpp"
#include "geom/shape.hpp"

namespace tmk::geom {

using Ring = std::vector<Point>;  // closed, first point not repeated
using Rings = std::vector<Ring>;  // a region under the non-zero fill rule: outer rings and holes

enum class Corners { Round, Chamfer };

Rings unite(const Rings& a);
Rings subtract(const Rings& a, const Rings& b);
Rings intersect(const Rings& a, const Rings& b);
// Region of closed loops under the even-odd rule (a board outline with cut-outs).
Rings even_odd(const Rings& loops);
// Grows (delta > 0) or shrinks a region; round corners stay within max_error of the true arc.
Rings offset(const Rings& a, Coord delta, Corners corners, Coord max_error);
// `s` grown by `grow`, every approximated arc outside the true one (KiCad's ERROR_OUTSIDE), so a knockout built
// from it is never smaller than the clearance asks.
Rings shape_rings(const Shape& s, Coord grow, Coord max_error);
// Regular polygon around a circle with every edge outside it, chord error <= max_error.
Ring circle_ring(Point centre, Coord radius, Coord max_error);

double area(const Ring& r);  // signed: outer rings positive, holes negative (as returned by these functions)
double area(const Rings& rs);
// Inside the region or on its boundary.
bool contains(const Rings& rs, Point p);

struct Polygon {
  Ring outer;
  Rings holes;
};
// The region as separate polygons (a fill's islands), each with its holes.
std::vector<Polygon> polygons(const Rings& rs);
// One ring with zero-width bridges from every hole to the outline, as KiCad writes a zone's filled_polygon
// (SHAPE_POLY_SET::Fracture). Holes are joined in order of their leftmost point, each by a horizontal bridge to
// the nearest edge on its left (the hole-elimination step of ear clipping; Eberly, 2002).
Ring fracture(const Polygon& p);

}  // namespace tmk::geom

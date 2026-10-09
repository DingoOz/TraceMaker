// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <utility>

#include "drc/rule_geometry.hpp"

using namespace tmk;
namespace {
using geom::Point;
using geom::Shape;
constexpr Coord M = 1'000'000;

std::vector<Point> rectangle(Coord x0, Coord y0, Coord x1, Coord y1) {
  return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}
model::Zone square_area() {
  model::Zone z;
  z.copper = model::layer_bit(0);
  z.outline.push_back(rectangle(0, 0, 10 * M, 10 * M));
  return z;
}
drc::CopperItem copper(Shape shape, model::LayerMask layers = model::layer_bit(0)) {
  drc::CopperItem item;
  item.layers = layers;
  item.box = shape.box;
  item.shapes.push_back(std::move(shape));
  return item;
}
void add_graphic(model::Board& b, model::Graphic g, std::size_t fp = 0) {
  g.footprint = static_cast<int>(fp);
  b.footprints[fp].graphics.push_back(static_cast<int>(b.graphics.size()));
  b.graphics.push_back(std::move(g));
}
model::Board court_board() {
  model::Board b;
  model::Footprint fp;
  fp.reference = "R12";
  b.footprints.push_back(std::move(fp));
  return b;
}
model::Graphic line(Point a, Point b, const std::string& layer = "F.CrtYd") {
  model::Graphic g;
  g.kind = model::Graphic::Kind::Line;
  g.layer = layer;
  g.a = a;
  g.b = b;
  return g;
}
}  // namespace

TEST_CASE("Rule area intersection and enclosure use common copper layers", "[rules][drc][rule_geometry]") {
  const auto area = square_area();
  // KiCad 10.0.3 area_insideArea/area_intersectsArea both report crossing and
  // inside; area_enclosedByArea reports inside only. All exclude B.Cu-only.
  const auto inside = copper(Shape::segment({2 * M, 5 * M}, {8 * M, 5 * M}, M / 10));
  const auto crossing = copper(Shape::segment({-2 * M, 5 * M}, {2 * M, 5 * M}, M / 10));
  const auto outside = copper(Shape::point({12 * M, 5 * M}, M / 10));
  CHECK(drc::area_matches(inside, area, false));
  CHECK(drc::area_matches(inside, area, true));
  CHECK(drc::area_matches(crossing, area, false));
  CHECK_FALSE(drc::area_matches(crossing, area, true));
  CHECK_FALSE(drc::area_matches(outside, area, false));
  auto back = inside;
  back.layers = model::layer_bit(1);
  CHECK_FALSE(drc::area_matches(back, area, false));
  CHECK_FALSE(drc::area_matches(back, area, true));
  back.layers |= model::layer_bit(0);
  CHECK(drc::area_matches(back, area, true));
}

TEST_CASE("Rule area enclosure checks full rounded copper, not its box", "[rules][drc][rule_geometry]") {
  auto area = square_area();
  // A disk fits this diamond, although its bounding-box corners do not.
  area.outline = {{{0, 5 * M}, {5 * M, 0}, {10 * M, 5 * M}, {5 * M, 10 * M}}};
  CHECK(drc::area_matches(copper(Shape::point({5 * M, 5 * M}, 3 * M)), area, true));
  CHECK_FALSE(drc::area_matches(copper(Shape::point({5 * M, 5 * M}, 4 * M)), area, true));
  area = square_area();
  // The centreline fits, but the track's copper protrudes across the boundary.
  const auto thick = copper(Shape::segment({M, M / 10}, {9 * M, M / 10}, M / 5));
  CHECK(drc::area_matches(thick, area, false));
  CHECK_FALSE(drc::area_matches(thick, area, true));
  // Radius-only overlap: no core point or centreline is in the area.
  CHECK(drc::area_matches(copper(Shape::segment({-M / 10, M}, {-M / 10, 9 * M}, M / 5)), area, false));
  // Bbox overlap alone is insufficient: the disk misses the square corner.
  CHECK_FALSE(drc::area_matches(copper(Shape::point({-M, -M}, 6 * M / 5)), area, false));
  auto compound = copper(Shape::point({2 * M, 2 * M}, M / 10));
  compound.shapes.push_back(Shape::point({12 * M, 2 * M}, M / 10));
  CHECK(drc::area_matches(compound, area, false));
  CHECK_FALSE(drc::area_matches(compound, area, true));
  CHECK_FALSE(drc::area_matches(drc::CopperItem{}, area, true));
}

TEST_CASE("Rule areas respect concavity and holes including vertex crossings", "[rules][drc][rule_geometry]") {
  auto area = square_area();
  // KiCad 10.0.3 area_concave_* and area_hole_*: solid/crossing intersect,
  // only solid encloses; notch/hole and off-layer controls do not match.
  area.outline = {{{0, 0}, {10 * M, 0}, {10 * M, 3 * M}, {3 * M, 3 * M}, {3 * M, 10 * M}, {0, 10 * M}}};
  CHECK(drc::area_matches(copper(Shape::point({M, 8 * M}, M / 10)), area, true));
  CHECK_FALSE(drc::area_matches(copper(Shape::point({8 * M, 8 * M}, M / 10)), area, false));
  // Both ends are inside, but the middle traverses the concave void.
  const auto bridge = copper(Shape::segment({2 * M, 8 * M}, {8 * M, 2 * M}, 0));
  CHECK(drc::area_matches(bridge, area, false));
  CHECK_FALSE(drc::area_matches(bridge, area, true));
  area = square_area();
  area.outline.push_back(rectangle(4 * M, 4 * M, 6 * M, 6 * M));
  CHECK_FALSE(drc::area_matches(copper(Shape::point({5 * M, 5 * M}, M / 4)), area, false));
  const auto across_hole = copper(Shape::segment({2 * M, 5 * M}, {8 * M, 5 * M}, M / 10));
  CHECK(drc::area_matches(across_hole, area, false));
  CHECK_FALSE(drc::area_matches(across_hole, area, true));
  // No copper edge crosses the hole: its entire interior is covered by the pad.
  CHECK_FALSE(drc::area_matches(copper(Shape::polygon(rectangle(2 * M, 2 * M, 8 * M, 8 * M))), area, true));
  // Crossing through hole vertices (not proper edge intersections).
  CHECK_FALSE(drc::area_matches(copper(Shape::segment({2 * M, 2 * M}, {8 * M, 8 * M}, 0)), area, true));
  // Tangency at one vertex must not reject an otherwise enclosed zero-width core.
  CHECK(drc::area_matches(copper(Shape::segment({2 * M, 6 * M}, {6 * M, 2 * M}, 0)), area, true));
  // Boundary endpoints with the whole intervening segment in the hole.
  CHECK_FALSE(drc::area_matches(copper(Shape::segment({4 * M, 5 * M}, {6 * M, 5 * M}, 0)), area, true));
  std::reverse(area.outline[1].begin(), area.outline[1].end());
  CHECK_FALSE(drc::area_matches(across_hole, area, true));
  // A disjoint contour is also usable; filling is not hardcoded to ring zero.
  area.outline.push_back(rectangle(20 * M, 0, 30 * M, 10 * M));
  CHECK(drc::area_matches(copper(Shape::point({25 * M, 5 * M}, M)), area, true));
}

TEST_CASE("Courtyard side selection is independent of copper side and matches wildcard refs", "[rules][drc][rule_geometry]") {
  auto b = court_board();
  model::Graphic front;
  front.kind = model::Graphic::Kind::Rect;
  front.layer = "F.CrtYd";
  front.a = {0, 0}; front.b = {10 * M, 10 * M};
  add_graphic(b, front);
  auto back = front;
  back.layer = "B.CrtYd";
  back.a = {20 * M, 0}; back.b = {30 * M, 10 * M};
  add_graphic(b, back);
  const auto cache = drc::build_courtyards(b);
  const auto in_front = copper(Shape::point({5 * M, 5 * M}, M / 10), model::layer_bit(1));
  const auto in_back = copper(Shape::point({25 * M, 5 * M}, M / 10));
  // KiCad 10.0.3 court_intersectsBackCourtyard also flags a F.Cu track
  // inside the B.CrtYd outline. Neither function filters item copper layers.
  CHECK(drc::courtyard_matches(in_front, cache, "R?2", "intersectsFrontCourtyard"));
  CHECK_FALSE(drc::courtyard_matches(in_front, cache, "R*", "intersectsBackCourtyard"));
  CHECK(drc::courtyard_matches(in_back, cache, "R*", "intersectsBackCourtyard"));
  CHECK(drc::courtyard_matches(in_back, cache, "R12", "intersectsCourtyard"));
  CHECK_FALSE(drc::courtyard_matches(in_back, cache, "C*", "intersectsCourtyard"));
  CHECK_FALSE(drc::courtyard_matches(in_back, cache, "r*", "intersectsCourtyard"));
  CHECK_FALSE(drc::courtyard_matches(in_back, cache, "R*", "unknownCourtyard"));
}

TEST_CASE("Rule area holes serialized as slit bridges retain material topology", "[rules][drc][rule_geometry]") {
  auto area = square_area();
  // KiCad 10.0.3 area_hole_* uses this single weakly-simple bridged contour,
  // not separate hole rings: solid/crossing intersect; only solid encloses.
  area.outline = {{{10 * M, 5 * M}, {25 * M, 5 * M}, {25 * M, 20 * M}, {10 * M, 20 * M},
                   {10 * M, 5 * M}, {14 * M, 9 * M}, {14 * M, 16 * M}, {21 * M, 16 * M},
                   {21 * M, 9 * M}, {14 * M, 9 * M}, {10 * M, 5 * M}}};
  const auto solid = copper(Shape::segment({11 * M, 7 * M}, {13 * M, 7 * M}, M / 10));
  const auto crossing = copper(Shape::segment({12 * M, 13 * M}, {18 * M, 13 * M}, M / 10));
  const auto hole = copper(Shape::segment({17 * M, 12 * M}, {19 * M, 12 * M}, M / 10));
  CHECK(drc::area_matches(solid, area, false));
  CHECK(drc::area_matches(solid, area, true));
  CHECK(drc::area_matches(crossing, area, false));
  CHECK_FALSE(drc::area_matches(crossing, area, true));
  CHECK_FALSE(drc::area_matches(hole, area, false));
  // Additional topology regression: the serialized out-and-back bridge is not
  // a physical void and must not reject a segment or pad crossing it.
  CHECK(drc::area_matches(copper(Shape::segment({11 * M, 7 * M}, {13 * M, 7 * M}, M / 4)), area, true));
  CHECK(drc::area_matches(copper(Shape::polygon(rectangle(11 * M, 6 * M, 13 * M, 8 * M))), area, true));
}

TEST_CASE("Courtyards chain unordered reversed lines but reject open outlines", "[rules][drc][rule_geometry]") {
  auto b = court_board();
  add_graphic(b, line({0, 0}, {10 * M, 0}));
  add_graphic(b, line({10 * M, 10 * M}, {10 * M, 0}));
  add_graphic(b, line({0, 10 * M}, {10 * M, 10 * M}));
  const auto inside = copper(Shape::point({5 * M, 5 * M}, M / 10));
  // KiCad 10.0.3 court_unclosed_* returns no hits for a three-line square.
  CHECK_FALSE(drc::courtyard_matches(inside, drc::build_courtyards(b), "R*", "intersectsCourtyard"));
  add_graphic(b, line({0, 0}, {0, 10 * M}));
  const auto cache = drc::build_courtyards(b);
  // KiCad 10.0.3 court_line_* reports inside and boundary-crossing copper.
  CHECK(drc::courtyard_matches(inside, cache, "R*", "intersectsFrontCourtyard"));
  CHECK(drc::courtyard_matches(copper(Shape::segment({-M, 5 * M}, {M, 5 * M}, M / 10)), cache, "R*", "intersectsCourtyard"));
  CHECK_FALSE(drc::courtyard_matches(copper(Shape::point({12 * M, 5 * M}, M / 10)), cache, "R*", "intersectsCourtyard"));
}

TEST_CASE("Arc circle and polygon courtyards use contours rather than bounding boxes", "[rules][drc][rule_geometry]") {
  auto b = court_board();
  model::Graphic arc;
  arc.kind = model::Graphic::Kind::Arc;
  arc.layer = "F.CrtYd";
  arc.a = {0, 5 * M}; arc.c = {5 * M, 0}; arc.b = {10 * M, 5 * M};
  add_graphic(b, arc);
  std::swap(arc.a, arc.b);
  arc.c = {5 * M, 10 * M};
  add_graphic(b, arc);
  auto cache = drc::build_courtyards(b);
  const auto centre = copper(Shape::point({5 * M, 5 * M}, M / 10));
  const auto corner = copper(Shape::point({M / 2, M / 2}, M / 10));
  // KiCad 10.0.3 court_arc_*: the two-semicircle courtyard hits inside and
  // crossing, not the bounding-square corner control.
  CHECK(drc::courtyard_matches(centre, cache, "R*", "intersectsCourtyard"));
  CHECK_FALSE(drc::courtyard_matches(corner, cache, "R*", "intersectsCourtyard"));
  CHECK(drc::courtyard_matches(copper(Shape::segment({-M, 5 * M}, {M, 5 * M}, M / 10)), cache, "R*", "intersectsCourtyard"));
  b.graphics.clear(); b.footprints[0].graphics.clear();
  model::Graphic circle;
  circle.kind = model::Graphic::Kind::Circle;
  circle.layer = "F.CrtYd";
  circle.a = {5 * M, 5 * M}; circle.b = {10 * M, 5 * M};
  add_graphic(b, circle);
  cache = drc::build_courtyards(b);
  CHECK(drc::courtyard_matches(centre, cache, "R*", "intersectsCourtyard"));
  CHECK_FALSE(drc::courtyard_matches(corner, cache, "R*", "intersectsCourtyard"));
  b.graphics.clear(); b.footprints[0].graphics.clear();
  model::Graphic poly;
  poly.kind = model::Graphic::Kind::Poly;
  poly.layer = "F.CrtYd";
  poly.pts = {{0, 0}, {10 * M, 0}, {10 * M, 3 * M}, {3 * M, 3 * M}, {3 * M, 10 * M}, {0, 10 * M}};
  add_graphic(b, poly);
  cache = drc::build_courtyards(b);
  CHECK(drc::courtyard_matches(copper(Shape::point({M, 8 * M}, M / 10)), cache, "R*", "intersectsCourtyard"));
  CHECK_FALSE(drc::courtyard_matches(copper(Shape::point({8 * M, 8 * M}, M / 10)), cache, "R*", "intersectsCourtyard"));
}

TEST_CASE("Overlapping and touching courtyard outlines are merged; unreadable ones are marked", "[rules][drc][rule_geometry]") {
  auto b = court_board();
  model::Graphic rect;
  rect.kind = model::Graphic::Kind::Rect;
  rect.layer = "F.CrtYd";
  rect.a = {0, 0}; rect.b = {10 * M, 10 * M};
  add_graphic(b, rect);
  rect.a = {5 * M, 0}; rect.b = {15 * M, 10 * M};
  add_graphic(b, rect);
  auto cache = drc::build_courtyards(b);
  // KiCad 10.0.6 court_overlap_*: the courtyard is the union. Even/odd filling would exclude x 5..10.
  CHECK_FALSE(cache.entries[0].sides[0].unreadable);
  CHECK(drc::courtyard_matches(copper(Shape::point({2 * M, 5 * M}, M / 10)), cache, "R*", "intersectsCourtyard"));
  CHECK(drc::courtyard_matches(copper(Shape::point({7 * M, 5 * M}, M / 10)), cache, "R*", "intersectsCourtyard"));
  CHECK(drc::courtyard_matches(copper(Shape::point({13 * M, 5 * M}, M / 10)), cache, "R*", "intersectsCourtyard"));
  CHECK_FALSE(drc::courtyard_matches(copper(Shape::point({17 * M, 5 * M}, M / 10)), cache, "R*", "intersectsCourtyard"));
  // court_touch_*: a shared edge.
  b.graphics.back().a = {10 * M, 0};
  cache = drc::build_courtyards(b);
  CHECK(drc::courtyard_matches(copper(Shape::point({2 * M, 5 * M}, M / 10)), cache, "R*", "intersectsCourtyard"));
  CHECK(drc::courtyard_matches(copper(Shape::point({13 * M, 5 * M}, M / 10)), cache, "R*", "intersectsCourtyard"));
  // Outlines clear of each other keep even/odd filling: the inner one is a hole.
  b.graphics.back().a = {4 * M, 4 * M};
  b.graphics.back().b = {6 * M, 6 * M};
  cache = drc::build_courtyards(b);
  CHECK(drc::courtyard_matches(copper(Shape::point({2 * M, 5 * M}, M / 10)), cache, "R*", "intersectsCourtyard"));
  CHECK_FALSE(drc::courtyard_matches(copper(Shape::point({5 * M, 5 * M}, M / 10)), cache, "R*", "intersectsCourtyard"));

  // No courtyard graphics: empty, but nothing was unreadable. An open outline or a Bezier curve is unreadable.
  auto open = court_board();
  CHECK_FALSE(drc::build_courtyards(open).entries[0].sides[0].unreadable);
  add_graphic(open, line({0, 0}, {10 * M, 0}));
  add_graphic(open, line({10 * M, 0}, {10 * M, 10 * M}));
  cache = drc::build_courtyards(open);
  CHECK(cache.entries[0].sides[0].unreadable);
  CHECK_FALSE(cache.entries[0].sides[1].unreadable);
  CHECK(cache.entries[0].sides[0].parts.empty());
  auto curve = court_board();
  model::Graphic bezier;
  bezier.kind = model::Graphic::Kind::Curve;
  bezier.layer = "B.CrtYd";
  bezier.pts = {{0, 0}, {5 * M, 5 * M}, {10 * M, 5 * M}, {10 * M, 0}};
  add_graphic(curve, bezier);
  CHECK(drc::build_courtyards(curve).entries[0].sides[1].unreadable);
}

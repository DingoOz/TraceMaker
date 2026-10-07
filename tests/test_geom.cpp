// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "core/rng.hpp"
#include "geom/poly_index.hpp"
#include "geom/shape.hpp"

using namespace tmk::geom;

TEST_CASE("rotation follows KiCad's convention and is exact for right angles", "[geom]") {
  CHECK(rotate({1000, 0}, 90) == Point{0, -1000});  // counter-clockwise on screen (y down)
  CHECK(rotate({1000, 0}, 180) == Point{-1000, 0});
  CHECK(rotate({1000, 0}, -90) == Point{0, 1000});
  CHECK(rotate({1000, 2000}, 360) == Point{1000, 2000});
}

TEST_CASE("segment intersection handles crossing, touching and collinear cases", "[geom]") {
  CHECK(segments_intersect({0, 0}, {10, 10}, {0, 10}, {10, 0}));
  CHECK(segments_intersect({0, 0}, {10, 0}, {10, 0}, {20, 5}));   // shared endpoint
  CHECK(segments_intersect({0, 0}, {10, 0}, {5, 0}, {15, 0}));    // collinear overlap
  CHECK_FALSE(segments_intersect({0, 0}, {10, 0}, {11, 0}, {15, 0}));
  CHECK_FALSE(segments_intersect({0, 0}, {10, 0}, {0, 1}, {10, 1}));
}

TEST_CASE("exact closer-than agrees with floating-point distance away from the boundary", "[geom]") {
  const tmk::RngStream rng(3, 1, 0);
  int checked = 0;
  for (std::uint64_t i = 0; i < 20000; ++i) {
    auto c = [&](std::uint64_t k) { return static_cast<tmk::Coord>(rng.u64(i * 16 + k) % 2'000'000) - 1'000'000; };
    const Point a{c(0), c(1)}, b{c(2), c(3)}, p{c(4), c(5)}, q{c(6), c(7)};
    const tmk::Coord t = static_cast<tmk::Coord>(rng.u64(i * 16 + 8) % 600'000);
    const long double d = seg_seg_dist(a, b, p, q);
    if (std::fabs(static_cast<double>(d) - static_cast<double>(t)) < 2.0) continue;  // too close to call in floating point
    REQUIRE(seg_seg_closer(a, b, p, q, t) == (d < static_cast<long double>(t)));
    ++checked;
  }
  CHECK(checked > 19000);
}

TEST_CASE("shape gaps: round pads, tracks and polygons", "[geom]") {
  const Shape pad = Shape::point({0, 0}, 500'000);                       // 1 mm round pad
  const Shape track = Shape::segment({800'000, -1'000'000}, {800'000, 1'000'000}, 100'000);  // 0.2 mm track
  // Gap = 800 µm - 500 - 100 = 200 µm.
  CHECK(closer_than(pad, track, 200'001));
  CHECK_FALSE(closer_than(pad, track, 200'000));
  CHECK(std::fabs(gap(pad, track) - 200'000.0) < 1.0);
  const Shape rect = Shape::polygon({{-1000, -1000}, {1000, -1000}, {1000, 1000}, {-1000, 1000}});
  const Shape inside = Shape::point({0, 0}, 1);
  CHECK(closer_than(rect, inside, 1));  // contained: overlap
  CHECK(point_in_polygon({1000, 0}, rect.pts));
  CHECK_FALSE(point_in_polygon({1001, 0}, rect.pts));
}

TEST_CASE("arcs flatten within the error bound", "[geom]") {
  const auto pts = arc_points({1'000'000, 0}, {0, 1'000'000}, {-1'000'000, 0}, 5'000);
  REQUIRE(pts.size() > 4);
  for (const auto& p : pts) CHECK(std::fabs(std::hypot(double(p.x), double(p.y)) - 1e6) < 2.0);
  CHECK(pts.front() == Point{1'000'000, 0});
  CHECK(pts.back() == Point{-1'000'000, 0});
  CHECK(pts[pts.size() / 2].y > 900'000);  // went through the mid point side
}

TEST_CASE("polygon edge index answers disk tests exactly like closer_than", "[geom]") {
  // A jagged star polygon with many edges (like a zone fill), tested against disks inside, outside, on the
  // boundary and on vertices: the bucketed path must agree with the linear reference everywhere.
  const tmk::RngStream rng(11, 2, 0);
  std::vector<Point> poly;
  const int n = 400;
  for (int i = 0; i < n; ++i) {
    const double a = 2.0 * M_PI * i / n;
    const double rad = (i % 2 ? 20e6 : 35e6) + static_cast<double>(rng.u64(static_cast<std::uint64_t>(i)) % 5'000'000);
    poly.push_back({static_cast<tmk::Coord>(rad * std::cos(a)), static_cast<tmk::Coord>(rad * std::sin(a))});
  }
  const Shape ref = Shape::polygon(poly, 0);
  const PolygonIndex idx(poly);
  int hits = 0;
  for (std::uint64_t k = 0; k < 4000; ++k) {
    const Point p = poly[k % poly.size()], q = poly[(k + 1) % poly.size()];
    Point c{static_cast<tmk::Coord>(rng.u64(1000 + k) % 90'000'000) - 45'000'000, static_cast<tmk::Coord>(rng.u64(9000 + k) % 90'000'000) - 45'000'000};
    if (k % 10 == 0) c = p;                                   // on a vertex
    if (k % 10 == 1) c = Point{(p.x + q.x) / 2, (p.y + q.y) / 2};  // on (or next to) an edge
    const tmk::Coord r = static_cast<tmk::Coord>(rng.u64(20000 + k) % 400'000);
    const bool want = closer_than(Shape::point(c, r), ref, 1);
    REQUIRE(idx.disk_closer(c, r, 1) == want);
    REQUIRE(idx.disk_closer(c, r + 1, 0) == closer_than(Shape::point(c, r + 1), ref, 0));  // overlap only
    hits += want;
  }
  CHECK(hits > 500);
  CHECK(hits < 3800);
}

TEST_CASE("polygon edge index answers shape tests and gaps exactly like the linear reference", "[geom]") {
  // Tracks, pads (rectangles with a corner radius) and disks against a jagged polygon: shape_closer must equal
  // closer_than, and shape_gap must equal gap bit for bit whenever the shapes are within the clearance.
  const tmk::RngStream rng(12, 3, 0);
  std::vector<Point> poly;
  const int n = 600;
  for (int i = 0; i < n; ++i) {
    const double a = 2.0 * M_PI * i / n;
    const double rad = (i % 2 ? 20e6 : 35e6) + static_cast<double>(rng.u64(static_cast<std::uint64_t>(i)) % 5'000'000);
    poly.push_back({static_cast<tmk::Coord>(rad * std::cos(a)), static_cast<tmk::Coord>(rad * std::sin(a))});
  }
  const Shape ref = Shape::polygon(poly, 0);
  const PolygonIndex idx(poly);
  auto coord = [&](std::uint64_t k) { return static_cast<tmk::Coord>(rng.u64(k) % 90'000'000) - 45'000'000; };
  int near = 0;
  for (std::uint64_t k = 0; k < 6000; ++k) {
    Point c{coord(7 * k), coord(7 * k + 1)};
    if (k % 12 == 0) c = poly[k % poly.size()];  // on a vertex
    const tmk::Coord len = static_cast<tmk::Coord>(rng.u64(7 * k + 2) % 6'000'000), r = static_cast<tmk::Coord>(rng.u64(7 * k + 3) % 300'000);
    const Point d{c.x + len, c.y + static_cast<tmk::Coord>(rng.u64(7 * k + 4) % 3'000'000) - 1'500'000};
    Shape s;
    switch (k % 3) {
      case 0: s = Shape::segment(c, d, r); break;
      case 1: s = Shape::point(c, r + 1); break;
      default: s = Shape::polygon({c, {d.x, c.y}, {d.x, c.y + len / 2 + 1}, {c.x, c.y + len / 2 + 1}}, r); break;
    }
    const tmk::Coord clearance = static_cast<tmk::Coord>(rng.u64(7 * k + 5) % 500'000);
    const bool want = closer_than(s, ref, clearance);
    REQUIRE(idx.shape_closer(s, clearance) == want);
    REQUIRE(idx.shape_closer(s, 0) == closer_than(s, ref, 0));
    if (want) {
      REQUIRE(idx.shape_gap(s, clearance + s.r) == gap(s, ref));
      ++near;
    }
  }
  CHECK(near > 800);
  CHECK(near < 5500);
}


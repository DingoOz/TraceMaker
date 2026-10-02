#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "core/rng.hpp"
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
    REQUIRE(seg_seg_closer(a, b, p, q, t) == (d < t));
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

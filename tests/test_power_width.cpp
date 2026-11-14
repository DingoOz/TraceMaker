// SPDX-License-Identifier: GPL-3.0-or-later
// Preferred widths (doc 05 §79, D184): a net with a preferred width is routed at it where it fits and at its class width where it does not.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "io/kicad/board_reader.hpp"
#include "route/router.hpp"

using namespace tmk;

namespace {

// 30 x 20 mm, two layers. Net P joins two 1 x 1 mm F.Cu pads at (5, 10) and (25, 10). A wall of net W at x = 15 on both layers, from the
// top and bottom edges to `gap_mm` / 2 either side of y = 10, leaves one gap for P to pass.
model::Board board(double gap_mm) {
  const double y0 = 10 - gap_mm / 2, y1 = 10 + gap_mm / 2;
  std::string s = R"((kicad_pcb (version 20240108) (generator "pcbnew")
  (general (thickness 1.6))
  (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (44 "Edge.Cuts" user))
  (setup (pad_to_mask_clearance 0))
  (net 0 "") (net 1 "P") (net 2 "W")
  (footprint "T:P" (layer "F.Cu") (at 5 10) (property "Reference" "R1")
    (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "P")))
  (footprint "T:P" (layer "F.Cu") (at 25 10) (property "Reference" "R2")
    (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "P")))
  (gr_rect (start 0 0) (end 30 20) (stroke (width 0.1) (type default)) (fill none) (layer "Edge.Cuts"))
)";
  for (const char* l : {"F.Cu", "B.Cu"}) {
    s += "  (segment (start 15 0.3) (end 15 " + std::to_string(y0) + ") (width 0.2) (locked yes) (layer \"" + l + "\") (net 2))\n";
    s += "  (segment (start 15 " + std::to_string(y1) + ") (end 15 19.7) (width 0.2) (locked yes) (layer \"" + l + "\") (net 2))\n";
  }
  s += ")\n";
  return io::read_board(sexpr::Document::parse(s));
}

model::DesignRules rules() {
  model::DesignRules r;
  r.minimums.clearance = 150'000;
  r.minimums.track_width = 150'000;
  r.minimums.via_diameter = 600'000;
  r.minimums.through_hole_diameter = 300'000;
  r.minimums.hole_clearance = 200'000;
  model::NetClass d;
  d.name = "Default";
  d.clearance = 200'000;
  d.track_width = 250'000;
  d.via_diameter = 800'000;
  d.via_drill = 400'000;
  r.classes = {d};
  return r;
}

route::RouteResult run(const model::Board& b, Coord preferred) {
  route::RouterOptions o;
  o.work_budget = 20'000'000;
  o.time_limit_s = 600;
  o.gpu_device = -1;
  if (preferred > 0) o.preferred_widths = {{"P", preferred}};
  return route::Router(b, rules(), o).run();
}

Coord narrowest(const route::RouteResult& r) {
  Coord w = INT64_MAX;
  for (const auto& t : r.tracks) w = std::min(w, t.width);
  return w;
}

}  // namespace

TEST_CASE("preferred width: routed at it where it fits", "[route][power_width]") {
  const auto b = board(3.0);  // 2.8 mm free between the wall ends
  const auto r = run(b, 800'000);
  REQUIRE(r.routed == 1);
  CHECK(r.wide == 1);
  CHECK(narrowest(r) == 800'000);
  const auto plain = run(b, 0);
  REQUIRE(plain.routed == 1);
  CHECK(plain.wide == 0);
  CHECK(narrowest(plain) == 250'000);
}

TEST_CASE("preferred width: falls back to the class width where it does not fit", "[route][power_width]") {
  const auto b = board(1.2);  // 1.0 mm free: 0.6 mm of track at 0.2 mm clearance, so 0.8 mm cannot pass, 0.25 mm can
  const auto r = run(b, 800'000);
  REQUIRE(r.routed == 1);
  CHECK(r.wide == 0);
  CHECK(narrowest(r) == 250'000);
  // Narrower than the class width: ignored.
  const auto n = run(b, 200'000);
  CHECK(n.routed == 1);
  CHECK(n.wide == 0);
}

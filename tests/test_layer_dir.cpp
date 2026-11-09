// SPDX-License-Identifier: GPL-3.0-or-later
// Per-layer preferred directions and the 45-degree penalty (issue #66, D174; doc 05 §29): --layer-dir and --layer-diag-pen.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <string>

#include "io/kicad/board_reader.hpp"
#include "route/layer_limits.hpp"
#include "route/router.hpp"

using namespace tmk;

namespace {

// 30 x 25 mm, two copper layers. Net A joins two F.Cu pads 15 mm apart in x and 10 mm in y, so the shortest octilinear
// track is 10 mm of 45-degree run plus 5 mm of horizontal run; nothing else is on the board.
model::Board board() {
  const std::string s = R"((kicad_pcb (version 20240108) (generator "pcbnew")
  (general (thickness 1.6))
  (layers (0 "F.Cu" signal "Top") (31 "B.Cu" signal) (44 "Edge.Cuts" user))
  (setup (pad_to_mask_clearance 0))
  (net 0 "") (net 1 "A")
  (footprint "T:P" (layer "F.Cu") (at 7 7) (property "Reference" "R1")
    (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "A")))
  (footprint "T:P" (layer "F.Cu") (at 22 17) (property "Reference" "R2")
    (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "A")))
  (gr_rect (start 0 0) (end 30 25) (stroke (width 0.1) (type default)) (fill none) (layer "Edge.Cuts"))
))";
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

route::RouterOptions base() {
  route::RouterOptions o;
  o.work_budget = 20'000'000;
  o.time_limit_s = 600;
  o.gpu_device = -1;
  o.allow_vias = false;  // the track stays on F.Cu, where the pads are
  return o;
}

// Track length (nm) on F.Cu by class: 0 horizontal, 1 vertical, 2 at 45 degrees, 3 other.
struct Shares {
  double len[4] = {0, 0, 0, 0};
};
Shares shares(const route::RouteResult& r) {
  Shares s;
  for (const auto& t : r.tracks) {
    if (t.layer != 0) continue;
    const double dx = std::abs(static_cast<double>(t.b.x - t.a.x)), dy = std::abs(static_cast<double>(t.b.y - t.a.y));
    const double l = std::hypot(dx, dy);
    s.len[dy == 0 ? 0 : dx == 0 ? 1 : std::abs(dx - dy) <= 2 ? 2 : 3] += l;
  }
  return s;
}

bool same_copper(const route::RouteResult& a, const route::RouteResult& b) {
  if (a.tracks.size() != b.tracks.size() || a.vias.size() != b.vias.size() || a.routed != b.routed) return false;
  for (std::size_t i = 0; i < a.tracks.size(); ++i)
    if (a.tracks[i].a != b.tracks[i].a || a.tracks[i].b != b.tracks[i].b || a.tracks[i].layer != b.tracks[i].layer) return false;
  return true;
}

}  // namespace

TEST_CASE("layer directions: names resolve against the board; unknown layers warn, malformed entries throw", "[route][layers]") {
  const auto b = board();
  REQUIRE(b.copper_count() == 2);
  auto d = route::layer_directions(b, {"F.Cu=v", "B.Cu=h"}, {});
  CHECK(d.dir == std::vector<std::int8_t>{route::kLayerDirVertical, route::kLayerDirHorizontal});
  CHECK(d.diag_pen.empty());
  CHECK(d.warnings.empty());
  // Layers not named have no preferred direction; the user's layer name and the long words work.
  d = route::layer_directions(b, {"Top=horizontal"}, {});
  CHECK(d.dir == std::vector<std::int8_t>{route::kLayerDirHorizontal, route::kLayerDirNone});
  // An unknown layer (an inner layer of a four-layer convention) is a warning, the rest applies.
  d = route::layer_directions(b, {"F.Cu=h", "In1.Cu=v"}, {"In2.Cu=1"});
  CHECK(d.dir == std::vector<std::int8_t>{route::kLayerDirHorizontal, route::kLayerDirNone});
  CHECK(d.diag_pen == std::vector<double>{0, 0});
  CHECK(d.warnings.size() == 2);
  // A bare value is every layer's; named layers override it in either order.
  d = route::layer_directions(b, {}, {"B.Cu=0.25", "0.5"});
  CHECK(d.dir.empty());
  CHECK(d.diag_pen == std::vector<double>{0.5, 0.25});
  CHECK_NOTHROW(route::check_layer_dir_specs({"F.Cu=none", "B.Cu=-"}, {"0"}));
  CHECK_THROWS_AS(route::check_layer_dir_specs({"F.Cu"}, {}), std::invalid_argument);
  CHECK_THROWS_AS(route::check_layer_dir_specs({"F.Cu=x"}, {}), std::invalid_argument);
  CHECK_THROWS_AS(route::check_layer_dir_specs({"F.Cu=h", "F.Cu=v"}, {}), std::invalid_argument);
  CHECK_THROWS_AS(route::check_layer_dir_specs({}, {"F.Cu=-1"}), std::invalid_argument);
  CHECK_THROWS_AS(route::check_layer_dir_specs({}, {"F.Cu=1x"}), std::invalid_argument);
  CHECK_THROWS_AS(route::check_layer_dir_specs({}, {"F.Cu=nan"}), std::invalid_argument);
  CHECK_THROWS_AS(route::check_layer_dir_specs({}, {"F.Cu=101"}), std::invalid_argument);
  CHECK_THROWS_AS(route::check_layer_dir_specs({}, {"1", "2"}), std::invalid_argument);
  CHECK_THROWS_AS(route::layer_directions(b, {"F.Cu=up"}, {}), std::invalid_argument);
}

TEST_CASE("layer directions: naming the parity's directions gives the parity's routes exactly", "[route][layers]") {
  const auto b = board();
  auto o = base();
  o.layer_dir_pen = 1.0;
  const auto parity = route::Router(b, rules(), o).run();
  auto named = o;
  named.layer_dir = {route::kLayerDirHorizontal, route::kLayerDirVertical};
  CHECK(same_copper(parity, route::Router(b, rules(), named).run()));
  auto swapped = o;
  swapped.layer_dir_swap = true;
  named.layer_dir = {route::kLayerDirVertical, route::kLayerDirHorizontal};
  const auto sw = route::Router(b, rules(), swapped).run();
  CHECK(same_copper(sw, route::Router(b, rules(), named).run()));
  CHECK_FALSE(same_copper(parity, sw));  // the board does tell the two directions apart
  // A zero diagonal penalty changes nothing.
  named.layer_diag_pen = {0, 0};
  CHECK(same_copper(sw, route::Router(b, rules(), named).run()));
}

TEST_CASE("layer directions: the preferred direction and the diagonal penalty shape the track", "[route][layers]") {
  const auto b = board();
  auto o = base();
  o.layer_dir_pen = 0;
  const auto plain = route::Router(b, rules(), o).run();
  REQUIRE(plain.routed == 1);
  const Shares p = shares(plain);
  CHECK(p.len[2] > 0);  // the shortest track runs at 45 degrees for most of its length

  // F.Cu vertical: horizontal steps cost a pitch more, and a 45-degree zigzag is cheaper than paying them.
  auto v = o;
  v.layer_dir = {route::kLayerDirVertical, route::kLayerDirNone};
  v.layer_dir_pen = 1.0;  // at 0.3 (the strength when none is given) paying 5 mm of horizontal run is still the cheaper way
  const auto rv = route::Router(b, rules(), v).run();
  CHECK(rv.routed == 1);
  CHECK(shares(rv).len[0] < p.len[0]);

  // The diagonal penalty takes 45-degree length away (doc 05 §29: the issue's observation, diagonals were free).
  auto dg = o;
  dg.layer_diag_pen = {1.0, 0};
  const auto rd = route::Router(b, rules(), dg).run();
  CHECK(rd.routed == 1);
  CHECK(shares(rd).len[2] < p.len[2]);
  // On the other layer it changes nothing.
  dg.layer_diag_pen = {0, 1.0};
  CHECK(same_copper(plain, route::Router(b, rules(), dg).run()));
}

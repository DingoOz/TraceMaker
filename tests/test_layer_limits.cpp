// SPDX-License-Identifier: GPL-3.0-or-later
// Layer limits (doc 05 §29): --no-tracks-on keeps new tracks off a copper layer, --layer-cost makes one dearer.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#include "io/kicad/board_reader.hpp"
#include "route/layer_limits.hpp"
#include "route/router.hpp"

using namespace tmk;

namespace {

// 30 x 20 mm, four copper layers. Net A joins two F.Cu pads left and right of a wall: locked tracks of net W from
// edge to edge on the layers listed in `walls`, so A has to change to a layer without one. Net G has a fill on
// In1.Cu in the lower left corner, away from A's way, with a through-hole pad in it and an F.Cu pad above it: the
// cheapest connection of that pad is a via into the fill.
model::Board board(const std::vector<std::string>& walls) {
  std::string s = R"((kicad_pcb (version 20240108) (generator "pcbnew")
  (general (thickness 1.6))
  (layers (0 "F.Cu" signal) (1 "In1.Cu" power "Planes") (2 "In2.Cu" signal) (31 "B.Cu" signal) (44 "Edge.Cuts" user))
  (setup (pad_to_mask_clearance 0))
  (net 0 "") (net 1 "A") (net 2 "W") (net 3 "G")
  (footprint "T:P" (layer "F.Cu") (at 5 8) (property "Reference" "R1")
    (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "A")))
  (footprint "T:P" (layer "F.Cu") (at 25 8) (property "Reference" "R2")
    (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "A")))
  (footprint "T:P" (layer "F.Cu") (at 10 14) (property "Reference" "R3")
    (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 3 "G")))
  (footprint "T:H" (layer "F.Cu") (at 3 18) (property "Reference" "J1")
    (pad "1" thru_hole circle (at 0 0) (size 1.6 1.6) (drill 0.8) (layers "*.Cu") (net 3 "G")))
  (zone (net 3) (net_name "G") (layer "In1.Cu") (hatch edge 0.5) (connect_pads (clearance 0.2)) (min_thickness 0.25)
    (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon (pts (xy 1 13) (xy 11 13) (xy 11 19) (xy 1 19)))
    (filled_polygon (layer "In1.Cu") (pts (xy 1 13) (xy 11 13) (xy 11 19) (xy 1 19))))
  (gr_rect (start 0 0) (end 30 20) (stroke (width 0.1) (type default)) (fill none) (layer "Edge.Cuts"))
)";
  for (const auto& l : walls) s += "  (segment (start 15 0.5) (end 15 19.5) (width 0.5) (locked yes) (layer \"" + l + "\") (net 2))\n";
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

route::RouteResult run(const model::Board& b, model::LayerMask no_tracks, std::vector<std::int32_t> cost_pm = {}) {
  route::RouterOptions o;
  o.work_budget = 40'000'000;
  o.time_limit_s = 600;
  o.gpu_device = -1;
  o.no_track_layers = no_tracks;
  o.layer_cost_pm = std::move(cost_pm);
  return route::Router(b, rules(), o).run();
}

// Tracks of `net` on copper layer `l`.
long tracks_on(const route::RouteResult& r, int l, model::NetId net) {
  return std::count_if(r.tracks.begin(), r.tracks.end(), [&](const model::Track& t) { return t.layer == l && t.net == net; });
}
long tracks_on(const route::RouteResult& r, int l) {
  return std::count_if(r.tracks.begin(), r.tracks.end(), [&](const model::Track& t) { return t.layer == l; });
}

constexpr int kF = 0, kIn1 = 1, kIn2 = 2, kB = 3;

}  // namespace

TEST_CASE("layer limits: names resolve to copper layers; what cannot be applied is an error", "[route][layers]") {
  const auto b = board({});
  REQUIRE(b.copper_count() == 4);
  const auto lim = route::layer_limits(b, {"In1.Cu", "B.Cu"}, {"In2.Cu=2.5", "F.Cu=1"});
  CHECK(lim.no_track_layers == (model::layer_bit(kIn1) | model::layer_bit(kB)));
  CHECK(lim.layer_cost_pm == std::vector<std::int32_t>{1000, 1000, 2500, 1000});
  CHECK(route::layer_limits(b, {"Planes"}, {}).no_track_layers == model::layer_bit(kIn1));  // the user's layer name
  const auto none = route::layer_limits(b, {}, {});
  CHECK(none.no_track_layers == 0);
  CHECK(none.layer_cost_pm.empty());
  CHECK_THROWS_AS(route::layer_limits(b, {"In3.Cu"}, {}), std::invalid_argument);
  CHECK_THROWS_AS(route::layer_limits(b, {"Edge.Cuts"}, {}), std::invalid_argument);
  CHECK_THROWS_AS(route::layer_limits(b, {"F.Cu", "In1.Cu", "In2.Cu", "B.Cu"}, {}), std::invalid_argument);
  CHECK_THROWS_AS(route::layer_limits(b, {}, {"In1.Cu"}), std::invalid_argument);
  CHECK_THROWS_AS(route::layer_limits(b, {}, {"In1.Cu=0.5"}), std::invalid_argument);
  CHECK_THROWS_AS(route::layer_limits(b, {}, {"In1.Cu=4x"}), std::invalid_argument);
  CHECK_THROWS_AS(route::layer_limits(b, {}, {"In1.Cu=nan"}), std::invalid_argument);
  CHECK_THROWS_AS(route::layer_limits(b, {}, {"Nope=2"}), std::invalid_argument);
}

TEST_CASE("layer limits: a layer without tracks gets none, and vias still reach its zone fill", "[route][layers]") {
  const auto b = board({"F.Cu", "B.Cu"});  // A can cross the wall only on an inner layer
  const model::NetId a = b.net_by_name("A"), g = b.net_by_name("G");

  const auto plain = run(b, 0);
  REQUIRE(plain.connections == 2);
  CHECK(plain.routed == 2);
  CHECK(tracks_on(plain, kIn1, a) + tracks_on(plain, kIn2, a) > 0);

  const auto no1 = run(b, model::layer_bit(kIn1));
  CHECK(no1.routed == 2);
  CHECK(tracks_on(no1, kIn1) == 0);
  CHECK(tracks_on(no1, kIn2, a) > 0);
  // G's pad still drops a via into its fill on the layer that takes no tracks.
  CHECK(std::count_if(no1.vias.begin(), no1.vias.end(), [&](const model::Via& v) { return v.net == g; }) == 1);
  CHECK(tracks_on(no1, kF, g) <= 1);  // at most the stub from the pad centre to the via

  const auto no12 = run(b, model::layer_bit(kIn1) | model::layer_bit(kIn2));
  CHECK(no12.routed == 1);  // only G: nothing is left for A to cross the wall on
  CHECK(tracks_on(no12, kIn1) + tracks_on(no12, kIn2) == 0);
  REQUIRE(no12.unrouted.size() == 1);
  CHECK(no12.unrouted[0].net == "A");
}

TEST_CASE("layer limits: pads only on a layer without tracks stay unrouted", "[route][layers]") {
  const auto b = board({});
  const auto r = run(b, model::layer_bit(kF));
  CHECK(r.connections == 2);
  CHECK(r.routed == 0);
  CHECK(r.tracks.empty());
  CHECK(r.vias.empty());
}

TEST_CASE("layer limits: a dearer layer is avoided while a plain one will do, and used when none will", "[route][layers]") {
  const auto b = board({"F.Cu"});  // In1, In2 and B.Cu all cross the wall at the same plain cost
  const model::NetId a = b.net_by_name("A");
  for (const int keep : {kIn1, kIn2, kB}) {
    std::vector<std::int32_t> pm(4, 4000);
    pm[kF] = 1000;
    pm[static_cast<std::size_t>(keep)] = 1000;
    const auto r = run(b, 0, pm);
    CAPTURE(keep);
    CHECK(r.routed == 2);
    for (const int l : {kIn1, kIn2, kB}) CHECK((tracks_on(r, l, a) > 0) == (l == keep));
  }
  // A cost is not a ban: with every way across dear, A is still routed.
  const auto all = run(b, 0, {1000, 4000, 4000, 4000});
  CHECK(all.routed == 2);
  // Factors of 1 change nothing at all.
  const auto plain = run(b, 0), ones = run(b, 0, {1000, 1000, 1000, 1000});
  REQUIRE(plain.tracks.size() == ones.tracks.size());
  for (std::size_t i = 0; i < plain.tracks.size(); ++i) {
    CHECK(plain.tracks[i].a == ones.tracks[i].a);
    CHECK(plain.tracks[i].b == ones.tracks[i].b);
    CHECK(plain.tracks[i].layer == ones.tracks[i].layer);
  }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Zone refill in the engine (doc 05 §36): fills follow the current copper, islands without a pad go, pads join
// by their zone connection, teardrops lose their fill; the fill-index connectivity equals the linear one.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "drc/connectivity.hpp"
#include "drc/copper.hpp"
#include "drc/drc.hpp"
#include "drc/refill.hpp"
#include "geom/clip.hpp"
#include "index/uniform_grid.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"

using namespace tmk;
namespace fs = std::filesystem;

namespace {

// 30 x 20 mm, two layers. GND pads at (5, 10) and (25, 10) on F.Cu under a GND plane on F.Cu; `pad1` and `zone`
// add attributes to the first pad and the zone; `extra` adds items.
std::string board_text(const std::string& extra, const std::string& pad1 = "", const std::string& zone = "") {
  return R"((kicad_pcb (version 20240108) (generator "pcbnew")
  (general (thickness 1.6))
  (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (44 "Edge.Cuts" user))
  (setup (pad_to_mask_clearance 0))
  (net 0 "") (net 1 "GND") (net 2 "S")
  (footprint "T:P" (layer "F.Cu") (at 5 10) (property "Reference" "R1")
    (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "GND") )" + pad1 + R"())
  (footprint "T:P" (layer "F.Cu") (at 25 10) (property "Reference" "R2")
    (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "GND")))
  (gr_rect (start 0 0) (end 30 20) (stroke (width 0.1) (type default)) (fill none) (layer "Edge.Cuts"))
  (zone (net 1) (net_name "GND") (layer "F.Cu") (uuid "a") (hatch edge 0.5) (connect_pads )" + zone + R"( (clearance 0.2))
    (min_thickness 0.25) (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5))
    (polygon (pts (xy 1 1) (xy 29 1) (xy 29 19) (xy 1 19))))
)" + extra + ")\n";
}

std::string segment(double x0, double y0, double x1, double y1) {
  return "  (segment (start " + std::to_string(x0) + " " + std::to_string(y0) + ") (end " + std::to_string(x1) + " " + std::to_string(y1) +
         ") (width 0.3) (layer \"F.Cu\") (net 2))\n";
}
std::string track(double x, double y0, double y1) { return segment(x, y0, x, y1); }

struct Loaded {
  model::Board board;
  model::DesignRules rules;
};

Loaded load(const std::string& name, const std::string& text) {
  const fs::path dir = fs::temp_directory_path() / ("tmk_refill_" + name + "_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  const fs::path pcb = dir / "b.kicad_pcb";
  std::ofstream(pcb) << text;
  Loaded l{io::read_board_file(pcb.string()).board, io::read_design_rules(pcb.string())};
  fs::remove_all(dir);
  return l;
}

// Missing connections of GND (the S tracks are obstacles only, their own net may be in pieces).
int unconnected(const model::Board& b, const model::DesignRules& r) {
  drc::DrcOptions o;
  o.connectivity_only = true;
  o.dangling = false;
  int n = 0;
  for (const auto& v : drc::run_drc(b, r, o).unconnected) n += v.description == "Missing connection in net GND";
  return n;
}

int errors(const model::Board& b, const model::DesignRules& r) {
  int n = 0;
  for (const auto& v : drc::run_drc(b, r).violations) n += v.severity == "error";
  return n;
}

int fills_on(const model::Board& b, int zone) { return static_cast<int>(b.zones[static_cast<std::size_t>(zone)].fills.size()); }

}  // namespace

TEST_CASE("refill: a track across the plane splits it, the stored fill does not", "[refill]") {
  // The stored fill is the whole outline: judged on it, the two pads are joined through the track's net's copper.
  auto l = load("split", board_text(track(15, 0.8, 19.2)));
  REQUIRE(l.board.zones.size() == 1);
  l.board.zones[0].fills = {{0, {{1'000'000, 1'000'000}, {29'000'000, 1'000'000}, {29'000'000, 19'000'000}, {1'000'000, 19'000'000}}}};
  CHECK(unconnected(l.board, l.rules) == 0);
  const auto rf = drc::refill_zones(l.board, l.rules);
  CHECK(rf.zones == 1);
  CHECK(fills_on(rf.board, 0) == 2);  // one island each side of the track
  CHECK(unconnected(rf.board, l.rules) == 1);
  // Every refilled island keeps its clearance from the track: no DRC error.
  CHECK(errors(rf.board, l.rules) == 0);
}

TEST_CASE("refill: islands without a pad follow island_removal_mode", "[refill]") {
  const std::string two = track(15, 0.8, 19.2) + track(20, 0.8, 19.2);  // the strip between holds no pad
  for (const auto& [mode, kept] : {std::pair{std::string{}, 2}, {"(island_removal_mode 1)", 3}}) {
    std::string text = board_text(two);
    text.replace(text.find("(thermal_bridge_width 0.5)"), 26, "(thermal_bridge_width 0.5) " + mode);
    const auto l = load("islands", text);
    const auto rf = drc::refill_zones(l.board, l.rules);
    CHECK(fills_on(rf.board, 0) == kept);
    CHECK(rf.islands_removed == 3 - kept);
  }
}

TEST_CASE("refill: pads join the plane by their zone connection", "[refill]") {
  // Thermal (zone default) and solid connections join R1; "none" leaves it isolated in its clearance hole.
  for (const auto& [pad, joined] : {std::pair{std::string{}, true}, {"(zone_connect 2)", true}, {"(zone_connect 0)", false}}) {
    const auto l = load("connect", board_text("", pad));
    const auto rf = drc::refill_zones(l.board, l.rules);
    CHECK((unconnected(rf.board, l.rules) == 0) == joined);
  }
  // The zone's own setting applies to pads without one: connect_pads no isolates both pads.
  const auto l = load("connect_no", board_text("", "", "no"));
  CHECK(l.board.zones[0].connect == model::ZoneConnect::None);
  CHECK(unconnected(drc::refill_zones(l.board, l.rules).board, l.rules) == 1);
}

TEST_CASE("refill: a thermal pad joins the plane by the spokes that reach it", "[refill]") {
  // S tracks above and below R1 cut its upper and lower spokes off; the right spoke still reaches the plane.
  const std::string walls = segment(0.5, 8.7, 9, 8.7) + segment(0.5, 11.3, 9, 11.3);
  const auto open = load("spokes", board_text(walls));
  CHECK(unconnected(drc::refill_zones(open.board, open.rules).board, open.rules) == 0);
  // A third track closes the band on the right: only the left spoke is left, into copper no other pad reaches.
  const auto closed = load("spokes_closed", board_text(walls + track(7, 8.7, 11.3)));
  CHECK(unconnected(drc::refill_zones(closed.board, closed.rules).board, closed.rules) == 1);
}

TEST_CASE("refill: teardrops lose their fill, hatched zones keep theirs", "[refill]") {
  const std::string teardrop =
      "  (zone (net 1) (net_name \"GND\") (layer \"B.Cu\") (uuid \"t\") (attr (teardrop (type padvia))) (connect_pads (clearance 0))"
      " (min_thickness 0.1) (fill yes) (polygon (pts (xy 10 10) (xy 11 10) (xy 11 11)))"
      " (filled_polygon (layer \"B.Cu\") (pts (xy 10 10) (xy 11 10) (xy 11 11))))\n";
  const auto l = load("teardrop", board_text(teardrop));
  REQUIRE(l.board.zones.size() == 2);
  CHECK(l.board.zones[1].teardrop);
  const auto rf = drc::refill_zones(l.board, l.rules);
  CHECK(rf.board.zones[1].fills.empty());
  CHECK(rf.zones == 1);
}

TEST_CASE("refill: connectivity through the fill edge index equals the linear test", "[refill]") {
  const auto l = load("index", board_text(track(15, 0.8, 19.2) + track(20, 0.8, 19.2)));
  const auto rf = drc::refill_zones(l.board, l.rules);
  const auto cm = drc::build_copper(rf.board);
  geom::Box all;
  for (const auto& it : cm.items) all.add(it.box);
  index::UniformGrid grid(all.inflated(1), 1'000'000, cm.items.size());
  for (std::size_t i = 0; i < cm.items.size(); ++i) grid.insert(static_cast<int>(i), cm.items[i].box);
  const drc::ZoneFills fills(cm);
  const auto linear = drc::compute_connectivity(rf.board, cm, grid, true);
  const auto indexed = drc::compute_connectivity(rf.board, cm, grid, true, &fills);
  CHECK(linear.root == indexed.root);
  CHECK(linear.end_hit == indexed.end_hit);
  CHECK(linear.via_layers == indexed.via_layers);
}

TEST_CASE("clip: a polygon with holes fractures into one ring of the same area", "[refill]") {
  const geom::Rings region = geom::subtract({{{0, 0}, {10'000, 0}, {10'000, 10'000}, {0, 10'000}}},
                                            {{{2'000, 2'000}, {4'000, 2'000}, {4'000, 4'000}, {2'000, 4'000}},
                                             {{6'000, 5'000}, {8'000, 5'000}, {8'000, 8'000}, {6'000, 8'000}}});
  const auto polys = geom::polygons(region);
  REQUIRE(polys.size() == 1);
  CHECK(polys[0].holes.size() == 2);
  const auto ring = geom::fracture(polys[0]);
  CHECK(std::abs(geom::area(ring)) == 100'000'000.0 - 4'000'000.0 - 6'000'000.0);
  CHECK(geom::point_in_polygon({1'000, 1'000}, ring));
  CHECK(!geom::point_in_polygon({3'000, 3'000}, ring));
  CHECK(!geom::point_in_polygon({7'000, 6'000}, ring));
}

TEST_CASE("reader: custom-named copper layers and zone connection settings", "[refill]") {
  const auto doc = sexpr::Document::parse(R"((kicad_pcb (version 20171130) (host pcbnew 5)
  (layers (0 F.Cu signal) (1 Gnd.Cu signal) (31 B.Cu signal) (44 Edge.Cuts user))
  (net 0 "") (net 1 GND)
  (module T:P (layer F.Cu) (at 5 5) (zone_connect 2)
    (pad 1 thru_hole circle (at 0 0) (size 1.6 1.6) (drill 0.8) (layers *.Cu) (net 1 GND) (zone_connect 0) (thermal_width 0.3) (thermal_gap 0.4)))
  (zone (net 1) (net_name GND) (layer Gnd.Cu) (tstamp 0) (hatch edge 0.508) (connect_pads thru_hole_only (clearance 0.5))
    (min_thickness 0.2) (fill yes (thermal_gap 0.6) (thermal_bridge_width 0.7) (island_removal_mode 2) (island_area_min 10))
    (polygon (pts (xy 0 0) (xy 10 0) (xy 10 10)))))
)");
  const auto b = io::read_board(doc);
  REQUIRE(b.copper_count() == 3);
  CHECK(b.copper_name(1) == "In1.Cu");
  CHECK(b.copper_index("Gnd.Cu") == 1);
  REQUIRE(b.zones.size() == 1);
  const auto& z = b.zones[0];
  CHECK(z.copper == model::layer_bit(1));
  CHECK(z.connect == model::ZoneConnect::ThtThermal);
  CHECK(z.min_thickness == 200'000);
  CHECK(z.thermal_gap == 600'000);
  CHECK(z.thermal_bridge_width == 700'000);
  CHECK(z.island_removal == 2);
  CHECK(z.island_area_min == 10e12);
}

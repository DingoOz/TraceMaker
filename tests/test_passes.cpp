// SPDX-License-Identifier: GPL-3.0-or-later
// After-route passes (doc 05 §39): the style metric on hand-made polylines, power widening in channels and at
// junctions, stitching vias on small boards with planes.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>

#include "drc/drc.hpp"
#include "drc/refill.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "passes/stitch.hpp"
#include "passes/style.hpp"
#include "passes/widen.hpp"
#include "route/obstacles.hpp"

using namespace tmk;
using geom::Point;
namespace fs = std::filesystem;

namespace {

constexpr Coord mm = 1'000'000;

struct Loaded {
  model::Board board;
  model::DesignRules rules;
};

Loaded load(const std::string& name, const std::string& text) {
  const fs::path dir = fs::temp_directory_path() / ("tmk_passes_" + name + "_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  const fs::path pcb = dir / "b.kicad_pcb";
  std::ofstream(pcb) << text;
  Loaded l{io::read_board_file(pcb.string()).board, io::read_design_rules(pcb.string())};
  fs::remove_all(dir);
  // Fixed rules, whatever KiCad's defaults are: 0.25 mm tracks, 0.2 mm clearance, 0.6 / 0.3 mm vias.
  auto& d = l.rules.classes.front();
  d.track_width = 250'000;
  d.clearance = 200'000;
  d.via_diameter = 600'000;
  d.via_drill = 300'000;
  l.rules.minimums = {};
  l.rules.minimums.clearance = 200'000;
  l.rules.minimums.track_width = 100'000;
  return l;
}

// 30 x 20 mm. Nets: 1 GND, 2 P (2 x 2 mm pads at (5, 10) and (25, 10) on F.Cu), 3 S. `layers` = 2 or 4.
std::string board_text(const std::string& extra, int layers = 2) {
  return std::string(R"((kicad_pcb (version 20240108) (generator "pcbnew")
  (general (thickness 1.6))
  (layers (0 "F.Cu" signal) )") +
         (layers == 4 ? R"((1 "In1.Cu" signal) (2 "In2.Cu" signal) )" : "") + R"((31 "B.Cu" signal) (44 "Edge.Cuts" user))
  (setup (pad_to_mask_clearance 0))
  (net 0 "") (net 1 "GND") (net 2 "P") (net 3 "S")
  (footprint "T:P" (layer "F.Cu") (at 5 10) (property "Reference" "R1")
    (pad "1" smd rect (at 0 0) (size 2 2) (layers "F.Cu") (net 2 "P")))
  (footprint "T:P" (layer "F.Cu") (at 25 10) (property "Reference" "R2")
    (pad "1" smd rect (at 0 0) (size 2 2) (layers "F.Cu") (net 2 "P")))
  (gr_rect (start 0 0) (end 30 20) (stroke (width 0.1) (type default)) (fill none) (layer "Edge.Cuts"))
)" + extra + ")\n";
}

std::string seg(double x0, double y0, double x1, double y1, int net, double w = 0.2, const std::string& layer = "F.Cu") {
  return "  (segment (start " + std::to_string(x0) + " " + std::to_string(y0) + ") (end " + std::to_string(x1) + " " + std::to_string(y1) + ") (width " +
         std::to_string(w) + ") (layer \"" + layer + "\") (net " + std::to_string(net) + "))\n";
}

// GND plane over the whole board on `layer`, and one plated GND pin at (3, 17) that keeps the fills from being islands.
std::string gnd_zone(const std::string& layer) {
  return "  (zone (net 1) (net_name \"GND\") (layer \"" + layer + "\") (uuid \"z" + layer +
         "\") (hatch edge 0.5) (connect_pads yes (clearance 0.2))\n"
         "    (min_thickness 0.25) (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5))\n"
         "    (polygon (pts (xy 1 1) (xy 29 1) (xy 29 19) (xy 1 19))))\n";
}
const char* kGndPin = R"(  (footprint "T:H" (layer "F.Cu") (at 3 17) (property "Reference" "J1")
    (pad "1" thru_hole circle (at 0 0) (size 1.6 1.6) (drill 0.8) (layers "*.Cu") (net 1 "GND")))
)";

model::Track track(Point a, Point b, model::NetId net, Coord w = 250'000, int layer = 0) { return {a, b, w, layer, net, false, sexpr::kNoNode}; }
Point pt(double x, double y) { return {static_cast<Coord>(x * 1e6), static_cast<Coord>(y * 1e6)}; }

passes::StyleReport style_of(const std::vector<std::pair<double, double>>& poly, Coord w = 250'000, const model::Board& b = {}) {
  model::Board empty;
  empty.nets.resize(4);
  std::vector<model::Track> t;
  for (std::size_t i = 0; i + 1 < poly.size(); ++i) t.push_back(track(pt(poly[i].first, poly[i].second), pt(poly[i + 1].first, poly[i + 1].second), 2, w));
  return passes::measure_style(b.nets.empty() ? empty : b, t, {}, {});
}

int errors(const model::Board& b, const model::DesignRules& r) {
  int n = 0;
  for (const auto& v : drc::run_drc(b, r).violations) n += v.severity == "error";
  return n;
}

int unconnected(const model::Board& b, const model::DesignRules& r) {
  drc::DrcOptions o;
  o.connectivity_only = true;
  o.dangling = false;
  return static_cast<int>(drc::run_drc(b, r, o).unconnected.size());
}

}  // namespace

// ---------------------------------------------------------------------------------------------- style metric

TEST_CASE("style: a straight run in pieces is one segment without bends", "[passes][style]") {
  const auto r = style_of({{0, 0}, {2, 0}, {5, 0}, {9, 0}});
  REQUIRE(r.connections.size() == 1);
  CHECK(r.segments == 1);
  CHECK(r.bends == 0);
  CHECK(r.length == 9 * mm);
  CHECK(r.detour_ratio() == 1.0);
  CHECK(r.long_detours == 0);
}

TEST_CASE("style: bends, and which of them are acute", "[passes][style]") {
  const auto ok = style_of({{0, 0}, {5, 0}, {8, 3}, {8, 9}});  // two 45 degree bends
  CHECK(ok.bends == 2);
  CHECK(ok.acute == 0);
  CHECK(ok.off_angle == 0);
  const auto right = style_of({{0, 0}, {5, 0}, {5, 5}});
  CHECK(right.bends == 1);
  CHECK(right.acute == 0);  // 90 degrees is not sharper than 90
  const auto sharp = style_of({{0, 0}, {5, 0}, {2, 3}});  // turns back by 135 degrees: 45 degrees between the segments
  CHECK(sharp.bends == 1);
  CHECK(sharp.acute == 1);
  CHECK(style_of({{0, 0}, {5, 0}}).bends_per_connection() == 0.0);
  CHECK(ok.bends_per_connection() == 2.0);
}

TEST_CASE("style: stair-steps are runs of short alternating jogs", "[passes][style]") {
  // 0.25 mm track: a segment is short below 1 mm. Three jogs of 0.5 to 0.7 mm turning right, left, right, left
  // between two long runs.
  const auto stair = style_of({{0, 0}, {5, 0}, {5.5, 0.5}, {6, 0.5}, {6.5, 1}, {7, 1}, {12, 1}});
  CHECK(stair.stair_steps == 1);
  CHECK(stair.bends == 4);
  // The same shape drawn large is a deliberate offset, not a stair.
  const auto big = style_of({{0, 0}, {5, 0}, {8, 3}, {11, 3}, {14, 6}, {17, 6}});
  CHECK(big.stair_steps == 0);
  // Two short jogs only: one Z, below the three-segment minimum.
  CHECK(style_of({{0, 0}, {5, 0}, {5.5, 0.5}, {6, 0.5}, {12, 0.5}}).stair_steps == 0);
  // Short segments that keep turning the same way are a curve, not a stair.
  CHECK(style_of({{0, 0}, {5, 0}, {5.5, 0.5}, {5.5, 1}, {5, 1.5}, {0, 1.5}}).stair_steps == 0);
}

TEST_CASE("style: short and off-angle segments", "[passes][style]") {
  const auto s = style_of({{0, 0}, {5, 0}, {5.1, 0.1}, {5.1, 5}});  // 0.14 mm diagonal on a 0.25 mm track
  CHECK(s.short_segments == 1);
  CHECK(s.off_angle == 0);
  const auto o = style_of({{0, 0}, {10, 3}, {12, 3}});
  CHECK(o.off_angle == 1);
  CHECK(o.short_segments == 0);
  CHECK(style_of({{0, 0}, {10, 0.01}}).off_angle == 0);  // 0.06 degrees: within the 0.1 degree tolerance
  CHECK(style_of({{0, 0}, {10, 0.03}}).off_angle == 1);
}

TEST_CASE("style: detour ratio against the shortest octilinear path", "[passes][style]") {
  const auto u = style_of({{0, 0}, {0, 10}, {10, 10}, {10, 0}});  // 30 mm of copper between ends 10 mm apart
  REQUIRE(u.connections.size() == 1);
  CHECK(u.detour_ratio() == 3.0);
  CHECK(u.long_detours == 1);
  const auto d = style_of({{0, 0}, {5, 5}, {10, 5}});  // exactly the octilinear distance (diagonal rounded to the nm)
  CHECK(d.long_detours == 0);
  CHECK(std::abs(d.detour_ratio() - 1.0) < 1e-6);
}

TEST_CASE("style: connections end at pads and junctions and pass through vias", "[passes][style]") {
  const auto l = load("style", board_text(""));
  // P pad to pad with a layer change in the middle: one connection, one via, no bend at the via.
  std::vector<model::Track> t = {track(pt(5, 10), pt(15, 10), 2), track(pt(15, 10), pt(25, 10), 2, 250'000, 1)};
  model::Via v;
  v.pos = pt(15, 10);
  v.size = 600'000;
  v.drill = 300'000;
  v.layer_top = 0;
  v.layer_bottom = 1;
  v.net = 2;
  auto r = passes::measure_style(l.board, t, {}, {v});
  REQUIRE(r.connections.size() == 1);
  CHECK(r.connections[0].vias == 1);
  CHECK(r.bends == 0);
  CHECK(r.vias == 1);
  // A track running on through the second pad: the pad ends the first connection.
  t = {track(pt(5, 10), pt(25, 10), 2), track(pt(25, 10), pt(25, 15), 2)};
  r = passes::measure_style(l.board, t, {}, {});
  CHECK(r.connections.size() == 2);
  CHECK(r.bends == 0);
  // Three tracks meeting off any pad: three connections, no bend counted across the junction.
  t = {track(pt(10, 5), pt(15, 5), 3), track(pt(15, 5), pt(20, 5), 3), track(pt(15, 5), pt(15, 2), 3)};
  r = passes::measure_style(l.board, t, {}, {});
  CHECK(r.connections.size() == 3);
  CHECK(r.bends == 0);
}

TEST_CASE("style: the result does not depend on the order or direction of the tracks", "[passes][style]") {
  model::Board b;
  b.nets.resize(4);
  std::vector<model::Track> t;
  const std::vector<std::pair<double, double>> poly = {{0, 0}, {5, 0}, {5.5, 0.5}, {6, 0.5}, {6.5, 1}, {7, 1}, {12, 1}, {9, 4}, {9, 9}};
  for (std::size_t i = 0; i + 1 < poly.size(); ++i) t.push_back(track(pt(poly[i].first, poly[i].second), pt(poly[i + 1].first, poly[i + 1].second), 2));
  const auto a = passes::style_json(b, passes::measure_style(b, t, {}, {}), true);
  std::reverse(t.begin(), t.end());
  for (std::size_t i = 0; i < t.size(); i += 2) std::swap(t[i].a, t[i].b);
  const auto c = passes::style_json(b, passes::measure_style(b, t, {}, {}), true);
  CHECK(a == c);
  CHECK(a["stair_steps"] == 1);
  CHECK(a["acute_angles"] == 1);
}

// ---------------------------------------------------------------------------------------------- power widening

TEST_CASE("widen: a track in a channel takes the widest candidate that fits", "[passes][widen]") {
  // S walls (0.2 mm) at y = 10 +- 0.75: copper may reach 0.45 mm from the centre line, a track up to 0.9 mm wide.
  const auto l = load("channel", board_text(seg(8, 9.25, 22, 9.25, 3) + seg(8, 10.75, 22, 10.75, 3)));
  std::vector<model::Track> t = {track(pt(5, 10), pt(25, 10), 2)};
  passes::WidenOptions o;
  o.targets = {{2, 800'000, "option"}};
  auto r = passes::widen_tracks(l.board, l.rules, t, {}, o);
  CHECK(t[0].width == 800'000);  // the target fits
  REQUIRE(r.nets.size() == 1);
  CHECK(r.nets[0].widened == 1);
  CHECK(r.nets[0].at_target == 1);
  CHECK(r.nets[0].before.at(250'000) == 20 * mm);
  CHECK(r.nets[0].after.at(800'000) == 20 * mm);
  CHECK(r.nets[0].widened_length == 20 * mm);

  t = {track(pt(5, 10), pt(25, 10), 2)};
  o.targets = {{2, 1'200'000, "option"}};
  r = passes::widen_tracks(l.board, l.rules, t, {}, o);
  // 1.2 does not fit; the steps below it are 1.081, 0.962, 0.843: the first that clears the walls is kept.
  CHECK(t[0].width == 843'000);
  CHECK(r.nets[0].at_target == 0);
  model::Board b = l.board;
  b.tracks.push_back(t[0]);
  CHECK(errors(b, l.rules) == 0);
  b.tracks.back().width = 962'000;  // the next candidate up is a real violation
  CHECK(errors(b, l.rules) > 0);
}

TEST_CASE("widen: a track with no room keeps its width", "[passes][widen]") {
  // Walls exactly at the clearance of the 0.25 mm track.
  const auto l = load("tight", board_text(seg(8, 9.575, 22, 9.575, 3) + seg(8, 10.425, 22, 10.425, 3)));
  std::vector<model::Track> t = {track(pt(5, 10), pt(25, 10), 2)};
  passes::WidenOptions o;
  o.targets = {{2, 800'000, "option"}};
  const auto r = passes::widen_tracks(l.board, l.rules, t, {}, o);
  CHECK(t[0].width == 250'000);
  CHECK(r.widened == 0);
  REQUIRE(r.nets.size() == 1);
  CHECK(r.nets[0].tracks == 1);
  CHECK(r.nets[0].before == r.nets[0].after);
  CHECK(r.checks == 8);
}

TEST_CASE("widen: a wide track meets a narrow one, and a via limits the width", "[passes][widen]") {
  // The second half of the P connection runs in a tight channel; the first half is free.
  const auto l = load("junction", board_text(seg(16, 9.575, 22, 9.575, 3) + seg(16, 10.425, 22, 10.425, 3)));
  std::vector<model::Track> t = {track(pt(5, 10), pt(14, 10), 2), track(pt(14, 10), pt(25, 10), 2)};
  passes::WidenOptions o;
  o.targets = {{2, 800'000, "option"}};
  passes::widen_tracks(l.board, l.rules, t, {}, o);
  CHECK(t[0].width == 800'000);
  CHECK(t[1].width == 250'000);
  model::Board b = l.board;
  b.tracks.insert(b.tracks.end(), t.begin(), t.end());
  CHECK(errors(b, l.rules) == 0);
  CHECK(unconnected(b, l.rules) == unconnected(l.board, l.rules) - 1);  // P is joined; the two S walls are not one net

  // A track that ends on a via is not made wider than the via.
  const auto f = load("via", board_text(""));
  model::Via v;
  v.pos = pt(15, 10);
  v.size = 600'000;
  v.drill = 300'000;
  v.layer_top = 0;
  v.layer_bottom = 1;
  v.net = 2;
  t = {track(pt(5, 10), pt(15, 10), 2), track(pt(15, 10), pt(15, 15), 2, 250'000, 1)};
  o.targets = {{2, 1'000'000, "option"}};
  const auto r = passes::widen_tracks(f.board, f.rules, t, {v}, o);
  CHECK(t[0].width == 600'000);
  CHECK(t[1].width == 600'000);
  CHECK(r.nets[0].at_target == 2);
}

TEST_CASE("widen: locked tracks, pairs and nets without a target are left alone", "[passes][widen]") {
  const auto l = load("locked", board_text(""));
  std::vector<model::Track> t = {track(pt(5, 10), pt(25, 10), 2), track(pt(5, 5), pt(25, 5), 3)};
  t[0].locked = true;
  passes::WidenOptions o;
  o.targets = {{2, 800'000, "option"}};
  auto r = passes::widen_tracks(l.board, l.rules, t, {}, o);
  CHECK(t[0].width == 250'000);  // locked
  CHECK(t[1].width == 250'000);  // at its class width, no target: never invent one
  CHECK(r.widened == 0);
  CHECK(r.checks == 0);
  t[0].locked = false;
  o.skip_nets = {2};
  r = passes::widen_tracks(l.board, l.rules, t, {}, o);
  CHECK(t[0].width == 250'000);
}

TEST_CASE("widen: a neck-down goes back to the class width, never beyond", "[passes][widen]") {
  const auto l = load("neck", board_text(""));
  std::vector<model::Track> t = {track(pt(5, 5), pt(25, 5), 3, 150'000), track(pt(5, 15), pt(25, 15), 3, 250'000)};
  passes::WidenOptions o;
  auto r = passes::widen_tracks(l.board, l.rules, t, {}, o);
  CHECK(t[0].width == 250'000);
  CHECK(t[1].width == 250'000);
  REQUIRE(r.nets.size() == 1);
  CHECK(r.nets[0].source == "net class");
  CHECK(r.nets[0].before.at(150'000) == 20 * mm);
  CHECK(r.nets[0].after.at(250'000) == 40 * mm);
  t[0].width = 150'000;
  o.restore_necked = false;
  r = passes::widen_tracks(l.board, l.rules, t, {}, o);
  CHECK(t[0].width == 150'000);
  // The work budget stops the pass.
  o.restore_necked = true;
  o.work_limit = 0;
  r = passes::widen_tracks(l.board, l.rules, t, {}, o);
  CHECK(t[0].width == 150'000);
  CHECK(r.out_of_work);
}

TEST_CASE("widen: a custom rule's maximum track width caps the widening", "[passes][widen]") {
  // (rule back_max (layer "B.Cu") (constraint track_width (max 0.45mm))): the F.Cu track takes the target, the
  // B.Cu track stops at the rule's maximum, and the DRC agrees with both.
  auto l = load("maxw", board_text(""));
  model::CustomRule rule;
  rule.name = "back_max";
  rule.layer = "B.Cu";
  model::Constraint c;
  c.type = "track_width";
  c.max = 450'000;
  rule.constraints.push_back(c);
  l.rules.custom.push_back(rule);
  std::vector<model::Track> t = {track(pt(5, 5), pt(25, 5), 3, 250'000, 0), track(pt(5, 15), pt(25, 15), 3, 250'000, 1)};
  passes::WidenOptions o;
  o.targets = {{3, 800'000, "option"}};
  const auto r = passes::widen_tracks(l.board, l.rules, t, {}, o);
  CHECK(t[0].width == 800'000);
  CHECK(t[1].width == 450'000);
  CHECK(r.widened == 2);
  model::Board b = l.board;
  b.tracks.insert(b.tracks.end(), t.begin(), t.end());
  CHECK(errors(b, l.rules) == 0);
  b.tracks.back().width = 451'000;  // the DRC reports a track above the maximum
  CHECK(errors(b, l.rules) == 1);
}

// ---------------------------------------------------------------------------------------------- stitching vias

namespace {
std::set<std::pair<Coord, Coord>> positions(const passes::StitchResult& r) {
  std::set<std::pair<Coord, Coord>> s;
  for (const auto& v : r.vias) s.emplace(v.pos.x, v.pos.y);
  return s;
}
}  // namespace

TEST_CASE("stitch: two overlapping fills get the grid", "[passes][stitch]") {
  const auto l = load("grid", board_text(kGndPin + gnd_zone("F.Cu") + gnd_zone("B.Cu")));
  passes::StitchOptions o;
  o.planes = true;
  o.pitch = 5 * mm;
  const auto r = passes::stitch_vias(l.board, l.rules, o);
  // Sites at 2.5 + 5 i, 2.5 + 5 j. The F.Cu fill is cut away around the two P pads at (5, 10) and (25, 10), which
  // leaves every site inside; the site at (2.5, 17.5) is within half a pitch of the plated GND pin.
  std::set<std::pair<Coord, Coord>> want;
  for (int i = 0; i < 6; ++i)
    for (int j = 0; j < 4; ++j)
      if (!(i == 0 && j == 3)) want.emplace(2'500'000 + i * 5 * mm, 2'500'000 + j * 5 * mm);
  CHECK(positions(r) == want);
  CHECK(r.sites == 24);
  CHECK(r.site_served == 1);
  CHECK(r.site_blocked == 0);
  CHECK(r.dropped == 0);
  for (const auto& v : r.vias) {
    CHECK(v.net == 1);
    CHECK(v.free);
    CHECK(v.size == 600'000);
    CHECK(v.layer_top == 0);
    CHECK(v.layer_bottom == 1);
  }
  // The result is clean and no less connected after a refill.
  model::Board b = l.board;
  b.vias.insert(b.vias.end(), r.vias.begin(), r.vias.end());
  const auto rf = drc::refill_zones(b, l.rules);
  CHECK(errors(rf.board, l.rules) == 0);
  CHECK(unconnected(rf.board, l.rules) == unconnected(drc::refill_zones(l.board, l.rules).board, l.rules));
  // Deterministic.
  const auto again = passes::stitch_vias(l.board, l.rules, o);
  REQUIRE(again.vias.size() == r.vias.size());
  for (std::size_t i = 0; i < r.vias.size(); ++i) CHECK(again.vias[i].pos == r.vias[i].pos);
}

TEST_CASE("stitch: nothing is placed without overlapping fills", "[passes][stitch]") {
  const auto l = load("single", board_text(kGndPin + gnd_zone("F.Cu")));
  passes::StitchOptions o;
  o.planes = true;
  o.pitch = 5 * mm;
  const auto r = passes::stitch_vias(l.board, l.rules, o);
  CHECK(r.vias.empty());
  CHECK(r.sites == 0);
  // And nothing at all when plane stitching is not asked for.
  const auto two = load("off", board_text(kGndPin + gnd_zone("F.Cu") + gnd_zone("B.Cu")));
  CHECK(passes::stitch_vias(two.board, two.rules, {}).vias.empty());
}

TEST_CASE("stitch: an obstacle removes exactly the sites it blocks", "[passes][stitch]") {
  // Four layers, GND on the outer two. An S track on In1.Cu passes under the sites (12.5, 7.5) and (17.5, 7.5):
  // both lie inside the two fills, and the exact check refuses the via through the track. A track of S on B.Cu
  // at (22.5, 12.5) cuts the fill there, so that site is not in two fills at all.
  const auto l = load("obstacle", board_text(kGndPin + gnd_zone("F.Cu") + gnd_zone("B.Cu") + seg(11, 7.5, 19, 7.5, 3, 0.2, "In1.Cu") +
                                                 seg(22.5, 11, 22.5, 14, 3, 0.2, "B.Cu"),
                                             4));
  passes::StitchOptions o;
  o.planes = true;
  o.pitch = 5 * mm;
  const auto r = passes::stitch_vias(l.board, l.rules, o);
  std::set<std::pair<Coord, Coord>> want;
  for (int i = 0; i < 6; ++i)
    for (int j = 0; j < 4; ++j) {
      const bool pin = i == 0 && j == 3, under = (i == 2 || i == 3) && j == 1, cut = i == 4 && j == 2;
      if (!pin && !under && !cut) want.emplace(2'500'000 + i * 5 * mm, 2'500'000 + j * 5 * mm);
    }
  CHECK(positions(r) == want);
  CHECK(r.sites == 23);
  CHECK(r.site_blocked == 2);
  model::Board b = l.board;
  b.vias.insert(b.vias.end(), r.vias.begin(), r.vias.end());
  CHECK(errors(drc::refill_zones(b, l.rules).board, l.rules) == 0);
}

TEST_CASE("stitch: a site inside another net's clearance is skipped", "[passes][stitch]") {
  // An S track on In1.Cu 0.45 mm beside the site (12.5, 7.5): the via (0.3 mm radius) would be 0.05 mm from its
  // copper, inside the 0.2 mm clearance, although it does not touch it. One mm away it fits.
  for (const double dx : {0.45, 1.0}) {
    const auto l = load("clear", board_text(kGndPin + gnd_zone("F.Cu") + gnd_zone("B.Cu") + seg(12.5 + dx, 6, 12.5 + dx, 9, 3, 0.2, "In1.Cu"), 4));
    passes::StitchOptions o;
    o.planes = true;
    o.pitch = 5 * mm;
    const auto r = passes::stitch_vias(l.board, l.rules, o);
    CHECK(positions(r).count({12'500'000, 7'500'000}) == (dx > 0.5 ? 1u : 0u));
    CHECK(r.site_blocked == (dx > 0.5 ? 0 : 1));
  }
  // A rule area that forbids vias takes its sites out too.
  const auto k = load("keepout", board_text(kGndPin + gnd_zone("F.Cu") + gnd_zone("B.Cu") + R"(  (zone (net 0) (net_name "") (layers "F.Cu" "B.Cu") (uuid "k") (hatch edge 0.5)
    (connect_pads (clearance 0)) (min_thickness 0.25)
    (keepout (tracks allowed) (vias not_allowed) (pads allowed) (copperpour allowed) (footprints allowed))
    (fill (thermal_gap 0.5) (thermal_bridge_width 0.5))
    (polygon (pts (xy 11 6) (xy 14 6) (xy 14 9) (xy 11 9))))
)"));
  passes::StitchOptions o;
  o.planes = true;
  o.pitch = 5 * mm;
  const auto r = passes::stitch_vias(k.board, k.rules, o);
  CHECK(positions(r).count({12'500'000, 7'500'000}) == 0);
  CHECK(r.vias.size() == 22);
}

TEST_CASE("stitch: a via may pass through the middle of another net's plane, not through its edge", "[passes][stitch]") {
  // Four layers: GND on the outer two, a P plane on In1.Cu from x = 1 to 13 (tied to P by a plated pin). The
  // sites at x = 2.5 and 7.5 go through solid P copper; those at x = 12.5 are 0.5 mm from the plane's edge, less
  // than the hole a refill cuts plus the zone's minimum width; from x = 17.5 on there is no P copper.
  const std::string p_plane = R"(  (footprint "T:H" (layer "F.Cu") (at 5 3) (property "Reference" "J2")
    (pad "1" thru_hole circle (at 0 0) (size 1.6 1.6) (drill 0.8) (layers "*.Cu") (net 2 "P")))
  (zone (net 2) (net_name "P") (layer "In1.Cu") (uuid "zp") (hatch edge 0.5) (connect_pads yes (clearance 0.2))
    (min_thickness 0.25) (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5))
    (polygon (pts (xy 1 1) (xy 13 1) (xy 13 19) (xy 1 19))))
)";
  const auto l = load("through", board_text(kGndPin + gnd_zone("F.Cu") + gnd_zone("B.Cu") + p_plane, 4));
  passes::StitchOptions o;
  o.planes = true;
  o.pitch = 5 * mm;
  const auto r = passes::stitch_vias(l.board, l.rules, o);
  const auto got = positions(r);
  for (int j = 0; j < 4; ++j) {
    CHECK(got.count({12'500'000, 2'500'000 + j * 5 * mm}) == 0);
    CHECK(got.count({7'500'000, 2'500'000 + j * 5 * mm}) == 1);
    CHECK(got.count({17'500'000, 2'500'000 + j * 5 * mm}) == 1);
  }
  CHECK(r.site_blocked == 4);  // the edge column
  CHECK(!r.discarded);
  CHECK(r.dropped == 0);
  // After a refill with the vias the P plane is still one piece, joined to its pin, and nothing is in violation.
  model::Board b = l.board;
  b.vias.insert(b.vias.end(), r.vias.begin(), r.vias.end());
  const auto rf = drc::refill_zones(b, l.rules);
  int p_pieces = 0;
  for (const auto& z : rf.board.zones)
    if (z.net == 2) p_pieces += static_cast<int>(z.fills.size());
  CHECK(p_pieces == 1);
  CHECK(errors(rf.board, l.rules) == 0);
  CHECK(unconnected(rf.board, l.rules) == unconnected(drc::refill_zones(l.board, l.rules).board, l.rules));
}

TEST_CASE("stitch: holes through another net's plane keep that plane's minimum width between them", "[passes][stitch]") {
  // Four layers: GND on the outer two, a P plane with a 0.6 mm minimum width on In1.Cu, its two pins at the left
  // edge. At a 1.3 mm pitch neighbouring sites would leave 0.3 mm of P copper between their holes; the refill
  // prunes that, and a grid of merged holes leaves the plane in loose pieces that are dropped as islands. The
  // plane may lose the holes and nothing else.
  const std::string p_plane = R"(  (footprint "T:H" (layer "F.Cu") (at 3 6) (property "Reference" "J2")
    (pad "1" thru_hole circle (at 0 0) (size 1.6 1.6) (drill 0.8) (layers "*.Cu") (net 2 "P")))
  (footprint "T:H" (layer "F.Cu") (at 3 12) (property "Reference" "J3")
    (pad "1" thru_hole circle (at 0 0) (size 1.6 1.6) (drill 0.8) (layers "*.Cu") (net 2 "P")))
  (zone (net 2) (net_name "P") (layer "In1.Cu") (uuid "zp") (hatch edge 0.5) (connect_pads yes (clearance 0.2))
    (min_thickness 0.6) (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.7))
    (polygon (pts (xy 1 1) (xy 29 1) (xy 29 19) (xy 1 19))))
)";
  const auto l = load("web", board_text(kGndPin + gnd_zone("F.Cu") + gnd_zone("B.Cu") + p_plane, 4));
  auto p_area = [](const model::Board& b) {  // mm^2; a fractured outline has its holes inside the one ring
    double a = 0;
    for (const auto& z : b.zones)
      if (z.net == 2)
        for (const auto& [layer, pts] : z.fills) {
          double s = 0;
          for (std::size_t k = 0, j = pts.size() - 1; k < pts.size(); j = k++)
            s += static_cast<double>(pts[j].x) / 1e6 * static_cast<double>(pts[k].y) / 1e6 - static_cast<double>(pts[k].x) / 1e6 * static_cast<double>(pts[j].y) / 1e6;
          a += std::abs(s) / 2;
        }
    return a;
  };
  passes::StitchOptions o;
  o.planes = true;
  o.pitch = 1'300'000;
  const auto r = passes::stitch_vias(l.board, l.rules, o);
  REQUIRE(r.vias.size() > 20);
  CHECK(!r.discarded);
  for (std::size_t i = 0; i < r.vias.size(); ++i)
    for (std::size_t j = i + 1; j < r.vias.size(); ++j) {
      const double d = std::hypot(static_cast<double>(r.vias[i].pos.x - r.vias[j].pos.x), static_cast<double>(r.vias[i].pos.y - r.vias[j].pos.y));
      CHECK(d >= 2 * (300'000 + 200'000) + 600'000);  // two knock-outs (via radius + clearance) and the web
    }
  model::Board b = l.board;
  b.vias.insert(b.vias.end(), r.vias.begin(), r.vias.end());
  const double before = p_area(drc::refill_zones(l.board, l.rules).board), after = p_area(drc::refill_zones(b, l.rules).board);
  const double holes = static_cast<double>(r.vias.size()) * 3.14159265 * 0.52 * 0.52;  // radius 0.3 + 0.2 and the refill's margins
  CHECK(before - after > 0);
  CHECK(before - after <= holes);
}

TEST_CASE("stitch: vias keep a wide margin from copper text", "[passes][stitch]") {
  const std::string text = R"(  (gr_text "TEST_q" (at 15 10 0) (layer "F.Cu") (uuid "t1")
    (effects (font (size 1 1) (thickness 0.15))))
)";
  const auto plain = load("text0", board_text(kGndPin + gnd_zone("F.Cu") + gnd_zone("B.Cu") + gnd_zone("In1.Cu"), 4));
  const auto l = load("text1", board_text(kGndPin + gnd_zone("F.Cu") + gnd_zone("B.Cu") + gnd_zone("In1.Cu") + text, 4));
  passes::StitchOptions o;
  o.planes = true;
  o.pitch = 1 * mm;
  const auto r0 = passes::stitch_vias(plain.board, plain.rules, o), r = passes::stitch_vias(l.board, l.rules, o);
  CHECK(r.vias.size() < r0.vias.size());
  // The boxes the obstacle model holds for the text are estimates of KiCad's glyphs: every via is clear of them by
  // the clearance and a quarter millimetre more.
  model::Board w = l.board;
  const route::Obstacles obs(w, l.rules);
  REQUIRE(!obs.copper_texts().empty());
  for (const auto& v : r.vias)
    for (const auto& [layer, box] : obs.copper_texts()) CHECK(!geom::closer_than(geom::Shape::point(v.pos, v.size / 2), box, 200'000 + 250'000));
}

TEST_CASE("stitch: a signal via gets one plane via within the distance", "[passes][stitch]") {
  const std::string sig = seg(10, 5, 15, 5, 3) + seg(15, 5, 20, 5, 3, 0.2, "B.Cu") +
                          "  (via (at 15 5) (size 0.6) (drill 0.3) (layers \"F.Cu\" \"B.Cu\") (net 3))\n";
  const auto l = load("return", board_text(kGndPin + gnd_zone("F.Cu") + gnd_zone("B.Cu") + sig));
  passes::StitchOptions o;
  o.returns = {{3, 1, 2 * mm, "option"}};
  auto r = passes::stitch_vias(l.board, l.rules, o);
  REQUIRE(r.vias.size() == 1);
  CHECK(r.return_path[0] == 1);
  CHECK(r.vias[0].net == 1);
  const Coord dx = r.vias[0].pos.x - 15 * mm, dy = r.vias[0].pos.y - 5 * mm;
  CHECK(dx * dx + dy * dy <= 4 * mm * mm);
  REQUIRE(r.returns.size() == 1);
  CHECK(r.returns[0].signal_vias == 1);
  CHECK(r.returns[0].stitched_before == 0);
  CHECK(r.returns[0].stitched_after == 1);
  model::Board b = l.board;
  b.vias.push_back(r.vias[0]);
  CHECK(errors(drc::refill_zones(b, l.rules).board, l.rules) == 0);
  // With that via on the board the signal via counts as stitched and nothing more is added.
  r = passes::stitch_vias(b, l.rules, o);
  CHECK(r.vias.empty());
  CHECK(r.returns[0].stitched_before == 1);
  CHECK(r.returns[0].stitched_after == 1);
  // Without a second GND layer there is nothing to tie together: the via stays unstitched.
  const auto one = load("return1", board_text(kGndPin + gnd_zone("F.Cu") + sig));
  r = passes::stitch_vias(one.board, one.rules, o);
  CHECK(r.vias.empty());
  CHECK(r.returns[0].stitched_after == 0);
}

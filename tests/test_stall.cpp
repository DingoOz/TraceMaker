// SPDX-License-Identifier: GPL-3.0-or-later
// Stall diagnostics (issue #8, doc 09 section 8, doc 13): a failed connection's `failure` event carries the width and clearance of the
// attempt, how much room the passage that closed had, a verdict (placement, rule, contested, unknown), the path that was tried and the
// rule it came from. Measuring them never changes the routing: a run with a sink attached gives the same copper as one without.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "drc/drc.hpp"
#include "io/srj/srj.hpp"
#include "route/router.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace tmk;

namespace {

struct Capture final : events::Sink {
  std::vector<json> failures;
  void publish(std::string text) override {
    const auto j = json::parse(text, nullptr, false);
    if (!j.is_discarded() && j.value("type", "") == "failure") failures.push_back(j);
  }
  bool wants_transient() const override { return false; }
};

struct Loaded {
  model::Board board;
  model::DesignRules rules;
};

// One copper layer, 20 x 20 mm. A wall of fixed copper (two bars with a gap of `gap` mm between them) crosses the board at x = 0; net A
// runs from (-8, 0) to (8, 0), through the gap, and has no other way. The rules: net class width, clearance, board minimum width.
Loaded wall_board(double gap, double width, double clearance, double min_width, bool second_net = false) {
  json p;
  p["layerCount"] = 1;
  p["minTraceWidth"] = min_width;
  p["nominalTraceWidth"] = width;
  p["defaultObstacleMargin"] = clearance;
  p["bounds"] = {{"minX", -10.0}, {"maxX", 10.0}, {"minY", -10.0}, {"maxY", 10.0}};
  json obstacles = json::array(), connections = json::array();
  auto pad = [&](const std::string& id, const std::string& part, double x, double y) {
    obstacles.push_back({{"type", "rect"}, {"layers", {"top"}}, {"center", {{"x", x}, {"y", y}}}, {"width", 0.6}, {"height", 0.5},
                         {"connectedTo", {id}}, {"obstacleId", "pad_" + id}, {"componentId", part}});
  };
  auto bar = [&](const std::string& id, double cy, double h) {
    obstacles.push_back({{"type", "rect"}, {"layers", {"top"}}, {"center", {{"x", 0.0}, {"y", cy}}}, {"width", 0.6}, {"height", h},
                         {"obstacleId", id}, {"componentId", "WALL"}});
  };
  const double ax = second_net ? 9.7 : 8.0;  // with a second net, A spans the board too, so the two must cross
  pad("a1", "U1", -ax, 0);
  pad("a2", "U2", ax, 0);
  connections.push_back({{"name", "NET_A"},
                         {"pointsToConnect", {{{"x", -ax}, {"y", 0.0}, {"layer", "top"}, {"pointId", "a1"}}, {{"x", ax}, {"y", 0.0}, {"layer", "top"}, {"pointId", "a2"}}}},
                         {"rootConnectionName", "NET_A"}});
  if (gap > 0) {  // the wall: up from -10 to -gap/2, and from gap/2 to 10
    const double h = 10 - gap / 2;
    bar("wall_lo", -(gap / 2 + h / 2), h);
    bar("wall_hi", gap / 2 + h / 2, h);
  }
  if (second_net) {  // net B across the whole board at x = 0, no gap in it for A: B's copper is what closes A's way
    pad("b1", "U3", 0, -9.7);  // the pads end 0.05 mm short of the board edge: no way around them
    pad("b2", "U4", 0, 9.7);
    connections.push_back({{"name", "NET_B"},
                           {"pointsToConnect", {{{"x", 0.0}, {"y", -9.7}, {"layer", "top"}, {"pointId", "b1"}}, {{"x", 0.0}, {"y", 9.7}, {"layer", "top"}, {"pointId", "b2"}}}},
                           {"rootConnectionName", "NET_B"}});
  }
  p["obstacles"] = obstacles;
  p["connections"] = connections;
  const auto files = io::srj_to_kicad(io::read_srj(p.dump()));
  const fs::path dir = fs::temp_directory_path() / ("tm_stall_test_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  const fs::path pcb = dir / "b.kicad_pcb";
  std::ofstream(pcb) << files.pcb;
  std::ofstream(dir / "b.kicad_pro") << files.pro;
  if (!files.dru.empty()) std::ofstream(dir / "b.kicad_dru") << files.dru;
  Loaded l{io::read_board_file(pcb.string()).board, io::read_design_rules(pcb.string())};
  fs::remove_all(dir);
  return l;
}

route::RouterOptions options(events::Sink* sink) {
  route::RouterOptions o;
  o.work_budget = 3'000'000;
  o.gpu_device = -1;
  o.optimize = false;
  o.sink = sink;
  return o;
}

}  // namespace

TEST_CASE("stall: a gap too narrow for any legal track is a placement problem", "[stall][route]") {
  // Class width 0.25, clearance 0.2: a 0.3 mm gap leaves no room for a track of any width (0.3 - 2 x 0.2 < 0).
  const auto l = wall_board(0.3, 0.25, 0.2, 0.1);
  Capture cap;
  const auto res = route::Router(l.board, l.rules, options(&cap)).run();
  REQUIRE(res.routed == 0);
  REQUIRE_FALSE(cap.failures.empty());
  const auto& f = cap.failures.back();
  INFO(f.dump());
  CHECK(f["width_nm"] == 250'000);
  CHECK(f["clearance_nm"] == 200'000);
  CHECK(f["min_width_nm"] == 100'000);
  CHECK(f["verdict"] == "placement");
  CHECK(f["gap_nm"] == 0);
  CHECK(f["gap_layer"] == 0);
  CHECK(f["reason"] == "boxed_in");  // the wall closes the source side: the open list ran out inside the pocket
  CHECK(f["rule"].get<std::string>().find("netclass") == 0);
  // No way gets through, so the place to look at is the closest approach: in front of the wall, on the line to the target.
  REQUIRE(f.contains("at"));
  CHECK(f["at"][0].get<long long>() > -1'000'000);
  CHECK(f["at"][0].get<long long>() <= 0);
  CHECK(std::llabs(f["at"][1].get<long long>()) <= 200'000);
  // The path that was tried starts at the source pad and stops there.
  REQUIRE(f["rejected"].size() >= 2);
  CHECK(f["rejected"].front()[0].get<long long>() < -7'000'000);
  CHECK(f["rejected"].back() == json::array({f["at"][0], f["at"][1], 0}));
  // The older fields are still there (old readers ignore the new ones).
  CHECK(f["type"] == "failure");
  CHECK(f.contains("a"));
  CHECK(f.contains("region"));
}

TEST_CASE("stall: a gap a narrower legal track passes is a rule problem", "[stall][route]") {
  // Clearance 0.1: a 0.32 mm gap takes a track of 0.12 mm. The class asks for 0.25; the board minimum is 0.1 (the router's own neck-down
  // stops at 0.15), so a narrower class width, not a moved part, opens the way.
  const auto l = wall_board(0.32, 0.25, 0.1, 0.1);
  Capture cap;
  const auto res = route::Router(l.board, l.rules, options(&cap)).run();
  REQUIRE(res.routed == 0);
  REQUIRE_FALSE(cap.failures.empty());
  const auto& f = cap.failures.back();
  INFO(f.dump());
  CHECK(f["verdict"] == "rule");
  CHECK(f["gap_nm"].get<long long>() >= 100'000);
  CHECK(f["gap_nm"].get<long long>() < 250'000);
  CHECK(f["rule"].get<std::string>().find("fits") != std::string::npos);
}

TEST_CASE("stall: routed copper of another net closing the way is contested", "[stall][route]") {
  // A free board: one layer, no wall, but net B runs across the whole board at x = 0, and A has no layer to change to.
  const auto l = wall_board(0, 0.25, 0.1, 0.1, true);
  Capture cap;
  const auto res = route::Router(l.board, l.rules, options(&cap)).run();
  REQUIRE(res.routed == 1);  // one of the two nets routes; the other is closed out by it
  REQUIRE_FALSE(cap.failures.empty());
  bool contested = false;
  for (const auto& f : cap.failures) contested = contested || f["verdict"] == "contested";
  INFO(cap.failures.back().dump());
  CHECK(contested);
  const json* c = nullptr;
  for (const auto& f : cap.failures)
    if (f["verdict"] == "contested") c = &f;
  REQUIRE(c != nullptr);
  CHECK((*c)["gap_fixed_nm"].get<long long>() >= 250'000);  // fixed copper alone leaves room for the class width ...
  CHECK((*c)["gap_nm"].get<long long>() < 250'000);         // ... the routed copper of the other net does not
}

TEST_CASE("stall: a viewer changes nothing about the routing", "[stall][route]") {
  for (const auto& l : {wall_board(0.3, 0.25, 0.2, 0.1), wall_board(0, 0.25, 0.1, 0.1, true), wall_board(2.0, 0.25, 0.1, 0.1)}) {
    Capture cap;
    const auto with = route::Router(l.board, l.rules, options(&cap)).run();
    const auto without = route::Router(l.board, l.rules, options(nullptr)).run();
    REQUIRE(with.tracks.size() == without.tracks.size());
    REQUIRE(with.vias.size() == without.vias.size());
    for (std::size_t i = 0; i < with.tracks.size(); ++i) {
      CHECK(with.tracks[i].a == without.tracks[i].a);
      CHECK(with.tracks[i].b == without.tracks[i].b);
      CHECK(with.tracks[i].layer == without.tracks[i].layer);
    }
    CHECK(with.routed == without.routed);
    CHECK(with.expansions == without.expansions);  // not one expansion more: the probe is bookkeeping, not search
  }
}

TEST_CASE("stall: a connection that routes has no failure to explain", "[stall][route]") {
  const auto l = wall_board(2.0, 0.25, 0.1, 0.1);  // a 2 mm gap: plenty
  Capture cap;
  const auto res = route::Router(l.board, l.rules, options(&cap)).run();
  CHECK(res.routed == 1);
  CHECK(cap.failures.empty());
}

TEST_CASE("no_neck_nets: a listed net keeps its class width, the others are necked down through a narrow gap", "[stall][route][neck]") {
  // Clearance 0.1, class width 0.25, board minimum 0.1: a 0.35 mm gap takes a track of 0.15 mm, the router's neck-down width. Without the
  // option NET_A squeezes through at 0.15 mm; listed in no_neck_nets it keeps 0.25 mm and has no way (a relaxation is spent on signals only).
  const auto l = wall_board(0.35, 0.25, 0.1, 0.1);
  auto o = options(nullptr);
  const auto plain = route::Router(l.board, l.rules, o).run();
  REQUIRE(plain.routed == 1);
  CHECK(plain.necked >= 1);
  o.no_neck_nets = {"NET_A"};
  const auto kept = route::Router(l.board, l.rules, o).run();
  CHECK(kept.routed == 0);
  CHECK(kept.necked == 0);
  o.no_neck_nets = {"OTHER"};  // another net listed: no effect on NET_A
  const auto other = route::Router(l.board, l.rules, o).run();
  CHECK(other.routed == 1);
}

// ---- escape features of the EtherCAT review (doc 05 §47): corridor length, persistence, radial supply pins -------------------------------------------
namespace {

// A 16-pin package U1 (two rows of 8, 0.5 mm pitch, 0.3 x 0.8 mm pads) at the left, connectors J1 and J2 at the right; every pin of U1 has a net to J1, and
// pins 0-3 of one row share one more net `VCC` (with a capacitor C1 and a via-free pad J2 far away). `wall_x` > 0: a keep-out column at that x across the board.
Loaded escape_board(double wall_x, bool tunnel = false) {
  json p;
  p["layerCount"] = 2;
  p["minTraceWidth"] = 0.1;
  p["nominalTraceWidth"] = 0.15;
  p["defaultObstacleMargin"] = 0.1;
  p["bounds"] = {{"minX", -12.0}, {"maxX", 40.0}, {"minY", -12.0}, {"maxY", 12.0}};
  json obstacles = json::array(), connections = json::array();
  auto pad = [&](const std::string& id, const std::string& part, double x, double y, double w, double h) {
    obstacles.push_back({{"type", "rect"}, {"layers", {"top"}}, {"center", {{"x", x}, {"y", y}}}, {"width", w}, {"height", h},
                         {"connectedTo", {id}}, {"obstacleId", "pad_" + id}, {"componentId", part}});
  };
  auto pt = [](const std::string& id, double x, double y) { return json{{"x", x}, {"y", y}, {"layer", "top"}, {"pointId", id}}; };
  for (int i = 0; i < 8; ++i) {  // rows at x = -1.9 and x = +1.9 (pads 0.8 mm long in x)
    for (int side = 0; side < 2; ++side) {
      const double x = side ? 1.9 : -1.9, y = (i - 3.5) * 0.5;
      const std::string id = "u" + std::to_string(side * 8 + i);
      pad(id, "U1", x, y, 0.8, 0.3);
      const bool vcc = side == 0 && i < 4;
      if (tunnel && (side * 8 + i == 5 || side * 8 + i == 13)) continue;  // these two pins of U1 are one net T instead (a tunnel from the left row to the right row)
      if (!vcc) {
        const std::string jd = "j" + std::to_string(side * 8 + i);
        pad(jd, "J1", 30.0, (side * 8 + i - 7.5) * 1.2, 1.0, 0.8);
        connections.push_back({{"name", "S" + std::to_string(side * 8 + i)}, {"pointsToConnect", {pt(id, x, y), pt(jd, 30.0, (side * 8 + i - 7.5) * 1.2)}},
                               {"rootConnectionName", "S" + std::to_string(side * 8 + i)}});
      }
    }
  }
  json vcc_pts = json::array();
  for (int i = 0; i < 4; ++i) vcc_pts.push_back(pt("u" + std::to_string(i), -1.9, (i - 3.5) * 0.5));
  if (tunnel) connections.push_back({{"name", "T"}, {"pointsToConnect", {pt("u5", -1.9, (5 - 3.5) * 0.5), pt("u13", 1.9, (5 - 3.5) * 0.5)}}, {"rootConnectionName", "T"}});
  pad("c1", "C1", -8.0, -3.0, 0.8, 0.8);
  pad("j2", "J2", 20.0, 10.0, 1.0, 1.0);
  vcc_pts.push_back(pt("c1", -8.0, -3.0));
  vcc_pts.push_back(pt("j2", 20.0, 10.0));
  connections.push_back({{"name", "VCC"}, {"pointsToConnect", vcc_pts}, {"rootConnectionName", "VCC"}});
  if (wall_x > 0)
    obstacles.push_back({{"type", "rect"}, {"layers", {"top", "bottom"}}, {"center", {{"x", wall_x}, {"y", 0.0}}}, {"width", 0.6}, {"height", 24.0}, {"obstacleId", "wall"}, {"componentId", "WALL"}});
  p["obstacles"] = obstacles;
  p["connections"] = connections;
  const auto files = io::srj_to_kicad(io::read_srj(p.dump()));
  const fs::path dir = fs::temp_directory_path() / ("tm_escfeat_test_" + std::to_string(::getpid()) + "_" + std::to_string(static_cast<int>(wall_x)) + (tunnel ? "t" : ""));
  fs::create_directories(dir);
  const fs::path pcb = dir / "b.kicad_pcb";
  std::ofstream(pcb) << files.pcb;
  std::ofstream(dir / "b.kicad_pro") << files.pro;
  if (!files.dru.empty()) std::ofstream(dir / "b.kicad_dru") << files.dru;
  Loaded l{io::read_board_file(pcb.string()).board, io::read_design_rules(pcb.string())};
  fs::remove_all(dir);
  return l;
}

route::RouterOptions escape_options() {
  route::RouterOptions o;
  o.work_budget = 3'000'000;
  o.escape_plan = true;
  return o;
}

}  // namespace

TEST_CASE("escape length: corridors run far past the package edge, and stop where fixed copper is in the way", "[stall][route][escape]") {
  const auto open = escape_board(0);
  auto o = escape_options();
  const auto base = route::Router(open.board, open.rules, o).run();
  o.escape_length_mm = 12;
  const auto longer = route::Router(open.board, open.rules, o).run();
  CHECK(base.escape_corridors == longer.escape_corridors);
  CHECK(base.escape_corridors > 0);
  CHECK(longer.escape_corridor_mm > base.escape_corridor_mm * 3);  // 2 mm stubs against up to 12 mm runs
  // A wall 6 mm to the right of the package: the runs stop in front of it.
  const auto walled = escape_board(6);
  const auto stopped = route::Router(walled.board, walled.rules, o).run();
  CHECK(stopped.escape_corridor_mm < longer.escape_corridor_mm);
  CHECK(stopped.escape_corridor_mm > base.escape_corridor_mm);
  // Same input, same options: the same plan.
  const auto again = route::Router(open.board, open.rules, o).run();
  CHECK(again.escape_corridor_mm == longer.escape_corridor_mm);
  CHECK(again.routed == longer.routed);
}

TEST_CASE("radial supply: a net with several pins on a dense package gets no pin-to-pin edges", "[stall][route][escape]") {
  const auto l = escape_board(0);
  auto o = escape_options();
  const auto plain = route::Router(l.board, l.rules, o).run();
  o.radial_supply = true;
  const auto radial = route::Router(l.board, l.rules, o).run();
  CHECK(plain.package_pin_edges >= 2);  // VCC's four pins are chained pin to pin
  CHECK(radial.package_pin_edges == 0);  // each goes to the capacitor instead
  INFO("plain routed " << plain.routed << "/" << plain.connections << ", radial " << radial.routed << "/" << radial.connections);
  CHECK(radial.routed > 0);
}

TEST_CASE("escape persist: legal, deterministic, and corridors come back for ripped-up connections", "[stall][route][escape]") {
  const auto l = escape_board(0);
  auto o = escape_options();
  o.escape_persist = true;
  o.escape_length_mm = 8;
  const auto one = route::Router(l.board, l.rules, o).run();
  const auto two = route::Router(l.board, l.rules, o).run();
  CHECK(one.routed == two.routed);
  CHECK(one.tracks.size() == two.tracks.size());
  CHECK(one.escape_reclaimed == two.escape_reclaimed);
  CHECK(one.routed > 0);
  CHECK(one.unrouted.size() == static_cast<std::size_t>(one.connections - one.routed));
}

TEST_CASE("via drops: routing with them is legal, deterministic and reaches the same connections on an open board", "[stall][route][escape]") {
  const auto l = escape_board(0);
  auto o = escape_options();
  const auto plain = route::Router(l.board, l.rules, o).run();
  o.via_drops = true;
  const auto one = route::Router(l.board, l.rules, o).run();
  const auto two = route::Router(l.board, l.rules, o).run();
  CHECK(one.routed == two.routed);
  CHECK(one.tracks.size() == two.tracks.size());
  CHECK(one.vias.size() == two.vias.size());
  CHECK(one.routed >= plain.routed - 1);  // offered, never forced: an open board routes as before (within the order effects of one extra endpoint)
  CHECK(one.routed > 0);
}

// A footprint that draws a mask opening over the pads of two nets (a solder jumper, a microphone's pad group) is a net-less opening that keeps every track out:
// the pin inside cannot start. --soft-mask-graphics takes the opening off (KiCad then flags what lies in it).
TEST_CASE("soft mask graphics: a pin inside a net-less mask opening routes only with the option", "[stall][route][mask]") {
  const std::string pcb =
      "(kicad_pcb (version 20171130) (host pcbnew \"5\")\n"
      "  (general (thickness 1.6))\n  (page A4)\n"
      "  (layers (0 F.Cu signal) (31 B.Cu signal) (38 B.Mask user) (39 F.Mask user) (44 Edge.Cuts user))\n"
      "  (setup (last_trace_width 0.2) (trace_clearance 0.2) (zone_clearance 0.508) (zone_45_only no) (trace_min 0.2) (via_size 0.6) (via_drill 0.3) (via_min_size 0.4) (via_min_drill 0.3)\n"
      "    (pad_to_mask_clearance 0))\n"
      "  (net 0 \"\")\n  (net 1 A)\n  (net 2 B)\n"
      "  (net_class Default \"\" (clearance 0.2) (trace_width 0.2) (via_dia 0.6) (via_drill 0.3) (uvia_dia 0.3) (uvia_drill 0.1) (add_net A) (add_net B))\n"
      "  (module J1 (layer F.Cu) (tedit 0) (tstamp 1) (at 5 5)\n"
      "    (fp_text reference J1 (at 0 -2) (layer F.SilkS) (effects (font (size 1 1) (thickness 0.15))))\n"
      "    (fp_line (start -1.8 -0.9) (end 1.8 -0.9) (layer F.Mask) (width 0.1))\n"
      "    (fp_line (start 1.8 -0.9) (end 1.8 0.9) (layer F.Mask) (width 0.1))\n"
      "    (fp_line (start 1.8 0.9) (end -1.8 0.9) (layer F.Mask) (width 0.1))\n"
      "    (fp_line (start -1.8 0.9) (end -1.8 -0.9) (layer F.Mask) (width 0.1))\n"
      "    (pad 1 smd rect (at -1 0) (size 0.8 0.8) (layers F.Cu F.Paste F.Mask) (net 1 A))\n"
      "    (pad 2 smd rect (at 1 0) (size 0.8 0.8) (layers F.Cu F.Paste F.Mask) (net 2 B)))\n"
      "  (module J2 (layer F.Cu) (tedit 0) (tstamp 2) (at 15 5)\n"
      "    (fp_text reference J2 (at 0 -2) (layer F.SilkS) (effects (font (size 1 1) (thickness 0.15))))\n"
      "    (pad 1 smd rect (at 0 0) (size 0.8 0.8) (layers F.Cu F.Paste F.Mask) (net 1 A))\n"
      "    (pad 2 smd rect (at 0 3) (size 0.8 0.8) (layers F.Cu F.Paste F.Mask) (net 2 B)))\n"
      "  (gr_line (start 0 0) (end 20 0) (layer Edge.Cuts) (width 0.1))\n  (gr_line (start 20 0) (end 20 10) (layer Edge.Cuts) (width 0.1))\n"
      "  (gr_line (start 20 10) (end 0 10) (layer Edge.Cuts) (width 0.1))\n  (gr_line (start 0 10) (end 0 0) (layer Edge.Cuts) (width 0.1))\n)\n";
  const fs::path dir = fs::temp_directory_path() / ("tm_mask_test_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  const fs::path file = dir / "b.kicad_pcb";
  std::ofstream(file) << pcb;
  const auto lb = io::read_board_file(file.string());
  const auto rules = io::read_design_rules(file.string());
  fs::remove_all(dir);
  route::RouterOptions o;
  o.work_budget = 1'000'000;
  const auto closed = route::Router(lb.board, rules, o).run();
  o.soft_mask_graphics = true;
  const auto open = route::Router(lb.board, rules, o).run();
  const auto again = route::Router(lb.board, rules, o).run();
  CHECK(closed.routed < closed.connections);  // the pin inside the opening cannot start
  CHECK(open.routed == open.connections);
  CHECK(open.routed > closed.routed);
  CHECK(again.routed == open.routed);
  CHECK(again.tracks.size() == open.tracks.size());
}

// Net-less copper drawn as a board graphic (a polygon on F.Cu) around a pad is fixed copper that no track may touch: the pad is enclosed. --soft-copper-graphics ignores it.
TEST_CASE("soft copper graphics: a pin inside a net-less copper polygon routes only with the option", "[stall][route][mask]") {
  const std::string pcb =
      "(kicad_pcb (version 20171130) (host pcbnew \"5\")\n"
      "  (general (thickness 1.6))\n  (page A4)\n"
      "  (layers (0 F.Cu signal) (31 B.Cu signal) (38 B.Mask user) (39 F.Mask user) (44 Edge.Cuts user))\n"
      "  (setup (last_trace_width 0.2) (trace_clearance 0.2) (zone_clearance 0.508) (zone_45_only no) (trace_min 0.2) (via_size 0.6) (via_drill 0.3) (via_min_size 0.4) (via_min_drill 0.3)\n"
      "    (pad_to_mask_clearance 0))\n"
      "  (net 0 \"\")\n  (net 1 A)\n"
      "  (net_class Default \"\" (clearance 0.2) (trace_width 0.2) (via_dia 0.6) (via_drill 0.3) (uvia_dia 0.3) (uvia_drill 0.1) (add_net A))\n"
      "  (module J1 (layer F.Cu) (tedit 0) (tstamp 1) (at 5 5)\n"
      "    (fp_text reference J1 (at 0 -2) (layer F.SilkS) (effects (font (size 1 1) (thickness 0.15))))\n"
      "    (pad 1 smd rect (at 0 0) (size 0.8 0.8) (layers F.Cu F.Paste F.Mask) (net 1 A)))\n"
      "  (module J2 (layer F.Cu) (tedit 0) (tstamp 2) (at 15 5)\n"
      "    (fp_text reference J2 (at 0 -2) (layer F.SilkS) (effects (font (size 1 1) (thickness 0.15))))\n"
      "    (pad 1 smd rect (at 0 0) (size 0.8 0.8) (layers F.Cu F.Paste F.Mask) (net 1 A)))\n"
      "  (gr_poly (pts (xy 3 2) (xy 8 2) (xy 8 8) (xy 3 8)) (layer F.Cu) (width 0))\n"
      "  (gr_poly (pts (xy 3 2) (xy 8 2) (xy 8 8) (xy 3 8)) (layer B.Cu) (width 0))\n"
      "  (gr_line (start 0 0) (end 20 0) (layer Edge.Cuts) (width 0.1))\n  (gr_line (start 20 0) (end 20 10) (layer Edge.Cuts) (width 0.1))\n"
      "  (gr_line (start 20 10) (end 0 10) (layer Edge.Cuts) (width 0.1))\n  (gr_line (start 0 10) (end 0 0) (layer Edge.Cuts) (width 0.1))\n)\n";
  const fs::path dir = fs::temp_directory_path() / ("tm_cugfx_test_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  const fs::path file = dir / "b.kicad_pcb";
  std::ofstream(file) << pcb;
  const auto lb = io::read_board_file(file.string());
  const auto rules = io::read_design_rules(file.string());
  fs::remove_all(dir);
  route::RouterOptions o;
  o.work_budget = 1'000'000;
  const auto closed = route::Router(lb.board, rules, o).run();
  o.soft_copper_graphics = true;
  const auto open = route::Router(lb.board, rules, o).run();
  const auto again = route::Router(lb.board, rules, o).run();
  CHECK(closed.routed < closed.connections);  // the pin sits inside fixed copper on both layers
  CHECK(open.routed == open.connections);
  CHECK(again.routed == open.routed);
  CHECK(again.tracks.size() == open.tracks.size());
}

// ---- bundle escapes (doc 05 §50): the connections that leave one side of a dense package for one side of another part are placed as a block of parallel lanes -----
TEST_CASE("bundle escape: the pins of one side leave as straight parallel lanes, deterministic and legal", "[stall][route][bundle]") {
  const auto l = escape_board(0);
  auto o = escape_options();
  o.work_budget = 4'000'000;
  const auto plain = route::Router(l.board, l.rules, o).run();
  CHECK(plain.bundle_lanes == 0);  // off by default
  o.bundle_escape = true;
  const auto one = route::Router(l.board, l.rules, o).run();
  const auto two = route::Router(l.board, l.rules, o).run();
  CHECK(one.bundle_groups >= 1);
  CHECK(one.bundle_spines >= 1);
  CHECK(one.bundle_lanes >= 6);  // the right-hand row of U1: eight pins, each to J1; the two at the ends are corner pins and go the ordinary way
  CHECK(one.routed >= plain.routed);
  CHECK(one.routed == two.routed);
  CHECK(one.bundle_lanes == two.bundle_lanes);
  REQUIRE(one.tracks.size() == two.tracks.size());
  for (std::size_t i = 0; i < one.tracks.size(); ++i) {
    CHECK(one.tracks[i].a == two.tracks[i].a);
    CHECK(one.tracks[i].b == two.tracks[i].b);
  }
  // A lane leaves its pad straight out of the package, as far as the block is wide: every pad of the right row starts a track going right, at the pad's own y.
  int straight = 0;
  for (const auto& pd : l.board.pads) {
    if (pd.footprint < 0 || l.board.footprints[static_cast<std::size_t>(pd.footprint)].reference != "U1" || pd.pos.x < 0) continue;
    for (const auto& t : one.tracks)
      if (t.a == pd.pos && t.b.y == pd.pos.y && t.b.x - t.a.x >= 1'500'000) {
        ++straight;
        break;
      }
  }
  CHECK(straight >= 6);
  // Legal: no copper rule is broken by the lanes (clearance, shorts), as KiCad's own checks count them.
  model::Board routed = l.board;
  routed.tracks.insert(routed.tracks.end(), one.tracks.begin(), one.tracks.end());
  routed.vias.insert(routed.vias.end(), one.vias.begin(), one.vias.end());
  const auto counts = drc::run_drc(routed, l.rules).counts();
  CHECK_FALSE(counts.contains("clearance"));
  CHECK_FALSE(counts.contains("shorting_items"));
}

TEST_CASE("bundle escape: lanes that cannot all be planar are left to the router", "[stall][route][bundle]") {
  // Same board, but the nets of two neighbouring right-row pins swap their pads on J1: those two lanes would cross, so at most one of them is a lane.
  auto l = escape_board(0);
  auto o = escape_options();
  o.bundle_escape = true;
  const auto base = route::Router(l.board, l.rules, o).run();
  REQUIRE(base.bundle_lanes >= 6);
  std::vector<std::size_t> j1;
  for (std::size_t i = 0; i < l.board.pads.size(); ++i)
    if (l.board.pads[i].footprint >= 0 && l.board.footprints[static_cast<std::size_t>(l.board.pads[i].footprint)].reference == "J1") j1.push_back(i);
  REQUIRE(j1.size() >= 12);
  std::sort(j1.begin(), j1.end(), [&](std::size_t a, std::size_t c) { return l.board.pads[a].pos.y < l.board.pads[c].pos.y; });
  std::swap(l.board.pads[j1[3]].net, l.board.pads[j1[4]].net);  // two pads of the right row's lanes trade nets
  const auto crossed = route::Router(l.board, l.rules, o).run();
  CHECK(crossed.bundle_lanes < base.bundle_lanes);
  CHECK(crossed.routed > 0);
}

// ---- supply first (doc 05 §51): the nets with two or more perimeter pins on a dense package and no pour go first and are not ripped ---------------------------
TEST_CASE("supply first: a net with several pins on a package is routed first, legal and deterministic", "[stall][route][bundle]") {
  const auto l = escape_board(0);
  auto o = escape_options();
  const auto plain = route::Router(l.board, l.rules, o).run();
  o.supply_first = true;
  const auto one = route::Router(l.board, l.rules, o).run();
  const auto two = route::Router(l.board, l.rules, o).run();
  CHECK(one.routed == two.routed);
  REQUIRE(one.tracks.size() == two.tracks.size());
  for (std::size_t i = 0; i < one.tracks.size(); ++i) {
    CHECK(one.tracks[i].a == two.tracks[i].a);
    CHECK(one.tracks[i].b == two.tracks[i].b);
  }
  CHECK(one.routed >= plain.routed - 1);  // VCC (four pins of U1) is the only such net: moving it to the front costs at most the order effects of one connection
  const auto vcc = [&](const model::Track& t) { return l.board.nets[static_cast<std::size_t>(t.net)].name == "VCC"; };
  CHECK(std::any_of(one.tracks.begin(), one.tracks.end(), vcc));
}

// ---- package tunnels (doc 05 §52): two pins of one net on opposite sides of a package are joined by a via at each pin and a run on the other layer -------------
TEST_CASE("package tunnels: two pins of one net on opposite sides get a stub and a via each and a run on the back layer", "[stall][route][bundle]") {
  const auto l = escape_board(0, true);
  auto o = escape_options();
  o.work_budget = 4'000'000;
  const auto plain = route::Router(l.board, l.rules, o).run();
  CHECK(plain.tunnels == 0);  // off by default
  o.package_tunnels = true;
  const auto one = route::Router(l.board, l.rules, o).run();
  const auto two = route::Router(l.board, l.rules, o).run();
  CHECK(one.tunnels == 1);
  CHECK(one.routed == two.routed);
  CHECK(one.tracks.size() == two.tracks.size());
  CHECK(one.vias.size() == two.vias.size());
  // Net T: two vias, and tracks on both layers, between its two pins.
  int t_vias = 0, t_front = 0, t_back = 0;
  for (const auto& v : one.vias)
    if (l.board.nets[static_cast<std::size_t>(v.net)].name == "T") ++t_vias;
  for (const auto& t : one.tracks)
    if (l.board.nets[static_cast<std::size_t>(t.net)].name == "T") ++(t.layer == 0 ? t_front : t_back);
  CHECK(t_vias == 2);
  CHECK(t_front >= 2);
  CHECK(t_back >= 1);
  model::Board routed = l.board;
  routed.tracks.insert(routed.tracks.end(), one.tracks.begin(), one.tracks.end());
  routed.vias.insert(routed.vias.end(), one.vias.begin(), one.vias.end());
  const auto counts = drc::run_drc(routed, l.rules).counts();
  CHECK_FALSE(counts.contains("clearance"));
  CHECK_FALSE(counts.contains("shorting_items"));
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Escape planning (M9): corridor geometry on a synthetic BGA and the feasibility analysis on a fixture board.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>

#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "route/escape.hpp"
#include "route/escape_flow.hpp"
#include "route/obstacles.hpp"
#include "route/router.hpp"

using namespace tmk;
using geom::Point;

namespace {

// n x n ball grid at `pitch`, every ball on its own net, on F.Cu of a 2-layer board.
model::Board bga(int n, Coord pitch, Coord ball) {
  model::Board b;
  b.nets.push_back({});
  b.footprints.push_back({});
  b.footprints[0].reference = "U1";
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      model::Pad p;
      p.footprint = 0;
      p.number = std::to_string(y * n + x);
      p.type = model::PadType::Smd;
      p.shape = model::PadShape::Circle;
      p.pos = {x * pitch, y * pitch};
      p.size_x = p.size_y = ball;
      p.copper = model::layer_bit(0);
      model::Net net;
      net.name = "N" + p.number;
      p.net = static_cast<model::NetId>(b.nets.size());
      b.nets.push_back(net);
      b.footprints[0].pads.push_back(static_cast<int>(b.pads.size()));
      b.pads.push_back(p);
    }
  return b;
}

double dist_point_segment(Point p, Point a, Point b) {
  const double ux = static_cast<double>(b.x - a.x), uy = static_cast<double>(b.y - a.y);
  const double len2 = ux * ux + uy * uy;
  double t = len2 > 0 ? ((static_cast<double>(p.x - a.x)) * ux + (static_cast<double>(p.y - a.y)) * uy) / len2 : 0;
  t = std::clamp(t, 0.0, 1.0);
  return std::hypot(static_cast<double>(p.x - a.x) - t * ux, static_cast<double>(p.y - a.y) - t * uy);
}

}  // namespace

TEST_CASE("escape plan: perimeter pins fan out, inner balls get one dog-bone site each", "[escape]") {
  const Coord pitch = 800'000;
  const auto b = bga(6, pitch, 400'000);
  std::vector<char> needs(b.pads.size(), 1);
  route::EscapeStats st;
  const auto plan = route::plan_escapes(b, needs, [](model::NetId) { return Coord{300'000}; }, {}, &st);
  REQUIRE(plan.size() == b.pads.size());
  CHECK(st.parts == 1);
  CHECK(st.perimeter == 20);  // the outer ring of a 6 x 6 array
  CHECK(st.dogbones == 16);
  std::set<std::pair<Coord, Coord>> sites;
  const Point centre{5 * pitch / 2, 5 * pitch / 2};
  for (const auto& c : plan) {
    CHECK(c.band <= pitch / 2);
    if (c.via) {
      CHECK(sites.insert({c.b.x, c.b.y}).second);  // every interstitial site serves one ball
      // pointing away from the package centre
      CHECK(std::llabs(c.b.x - centre.x) >= std::llabs(c.a.x - centre.x));
      CHECK(std::llabs(c.b.y - centre.y) >= std::llabs(c.a.y - centre.y));
    } else {
      // leaves the package
      const bool out = c.b.x < 0 || c.b.y < 0 || c.b.x > 5 * pitch || c.b.y > 5 * pitch;
      CHECK(out);
    }
  }
  // No corridor runs over another ball's centre (corridors never reserve a neighbour's pad).
  for (const auto& c : plan)
    for (const auto& p : b.pads)
      if (b.pads[static_cast<std::size_t>(c.pad)].pos != p.pos) CHECK(dist_point_segment(p.pos, c.a, c.b) >= static_cast<double>(pitch) / 2 - 1);
  // With a channel callback the second ring escapes between two outer balls instead of taking a via.
  route::EscapeStats st2;
  const auto plan2 = route::plan_escapes(b, needs, [](model::NetId) { return Coord{300'000}; }, {}, &st2,
                                         [](model::NetId) { return Coord{350'000}; });  // 0.8 pitch - 0.4 ball = 0.4 >= 0.35
  CHECK(st2.perimeter == 20);
  CHECK(st2.second_ring == 12);
  CHECK(st2.dogbones == 4);
  std::set<std::pair<Coord, Coord>> gaps;
  for (const auto& c : plan2)
    if (c.has_mid) {
      CHECK(gaps.insert({c.mid.x, c.mid.y}).second);  // one ball per gap
      for (const auto& p : b.pads)
        if (b.pads[static_cast<std::size_t>(c.pad)].pos != p.pos) {
          CHECK(dist_point_segment(p.pos, c.a, c.mid) >= static_cast<double>(pitch) / 2 - 1);
          CHECK(dist_point_segment(p.pos, c.mid, c.b) >= static_cast<double>(pitch) / 2 - 1);
        }
    }
  // Too narrow a gap: dog-bones as before.
  route::EscapeStats st3;
  route::plan_escapes(b, needs, [](model::NetId) { return Coord{300'000}; }, {}, &st3, [](model::NetId) { return Coord{450'000}; });
  CHECK(st3.second_ring == 0);
  // Deterministic.
  const auto again = route::plan_escapes(b, needs, [](model::NetId) { return Coord{300'000}; });
  REQUIRE(again.size() == plan.size());
  for (std::size_t i = 0; i < plan.size(); ++i) CHECK((again[i].b == plan[i].b && again[i].pad == plan[i].pad));
  // Pads that need no routing get no corridor; coarse parts are left alone.
  std::vector<char> none(b.pads.size(), 0);
  CHECK(route::plan_escapes(b, none, [](model::NetId) { return Coord{300'000}; }).empty());
  route::EscapeOptions coarse;
  coarse.max_pitch = 500'000;
  CHECK(route::plan_escapes(b, needs, [](model::NetId) { return Coord{300'000}; }, coarse).empty());
}

TEST_CASE("escape analysis: sbc's DRAM balls are blocked only by the solder-mask rule", "[escape][fixture]") {
  const std::string path = std::string(TM_SOURCE_DIR) + "/bench/data/freerouting/scripts/benchmark/fixtures/PCBench/sbc_sbc/unrouted.kicad_pcb";
  if (!std::filesystem::exists(path)) SKIP("fixture missing: " + path);
  const auto lb = io::read_board_file(path);
  const auto rules = io::read_design_rules(path);
  model::Board b = lb.board;
  route::Obstacles obs(b, rules);
  const auto parts = route::analyse_escapes(b, rules, obs);
  const route::PartEscape* dram = nullptr;
  for (const auto& pe : parts)
    if (pe.ref == "DRAM1") dram = &pe;
  REQUIRE(dram != nullptr);
  CHECK(dram->pins > 60);
  CHECK(!dram->dead.empty());
  int mask_only = 0;
  for (const auto& d : dram->dead) mask_only += d.reason.find("solder-mask") != std::string::npos;
  CHECK(mask_only > 0);
  CHECK(!dram->hint.empty());
  // With tented vias the same balls escape: the analysis restores the setting it probes with.
  CHECK(obs.via_mask() > 0);
  obs.set_via_mask(0);
  const auto tented = route::analyse_escapes(b, rules, obs);
  for (const auto& pe : tented)
    if (pe.ref == "DRAM1") CHECK(pe.dead.size() < dram->dead.size());
}

// ---- Escape planning v2: min-cost-flow channel and layer assignment (route/escape_flow.hpp) ----

namespace {

route::FlowEscapeInput flow_input(Coord w, Coord s, Coord via, int layers) {
  route::FlowEscapeInput in;
  in.width = [w](model::NetId) { return w; };
  in.clearance = [s](model::NetId) { return s; };
  in.via = [via](model::NetId) { return via; };
  in.keep = [w, s](model::NetId) { return w + s; };
  in.layers = layers;
  return in;
}

// The corridor's polyline on the layer it ends on (the tail, preceded by b), or on the pad layer when it has none.
std::vector<Point> tail_of(const route::EscapeCorridor& c) {
  std::vector<Point> pts{c.b};
  pts.insert(pts.end(), c.tail.begin(), c.tail.end());
  if (!c.via) pts.insert(pts.begin(), c.a);
  return pts;
}

// Does the polyline cross the open middle of segment p-q (between 20 % and 80 % of its length)?
bool crosses_middle(const std::vector<Point>& pl, Point p, Point q) {
  for (std::size_t k = 0; k + 1 < pl.size(); ++k) {
    const double ax = static_cast<double>(pl[k].x), ay = static_cast<double>(pl[k].y);
    const double bx = static_cast<double>(pl[k + 1].x), by = static_cast<double>(pl[k + 1].y);
    const double px = static_cast<double>(p.x), py = static_cast<double>(p.y), qx = static_cast<double>(q.x), qy = static_cast<double>(q.y);
    const double d = (bx - ax) * (qy - py) - (by - ay) * (qx - px);
    if (std::fabs(d) < 1e-9) continue;
    const double t = ((px - ax) * (qy - py) - (py - ay) * (qx - px)) / d;   // along the polyline segment
    const double u = ((px - ax) * (by - ay) - (py - ay) * (bx - ax)) / d;   // along p-q
    if (t >= -1e-9 && t <= 1 + 1e-9 && u >= 0.2 && u <= 0.8) return true;
  }
  return false;
}

}  // namespace

TEST_CASE("escape flow: a 6 x 6 array escapes on its pad layer within every channel's capacity", "[escape]") {
  const Coord pitch = 1'000'000, ball = 400'000, w = 150'000, s = 150'000;
  const auto b = bga(6, pitch, ball);
  std::vector<char> needs(b.pads.size(), 1);
  route::FlowEscapeStats st;
  const auto plan = route::plan_escapes_flow(b, needs, flow_input(w, s, 600'000, 1), {}, &st);
  CHECK(st.arrays == 1);
  REQUIRE(st.rings.size() == 3);
  CHECK(st.rings[0].pins == 20);
  CHECK(st.rings[1].pins == 12);
  CHECK(st.rings[2].pins == 4);
  // One track fits between two balls (0.6 mm gap, 0.15 / 0.15 mm rules): 20 boundary channels for 16 inner balls.
  for (const auto& r : st.rings) CHECK(r.pad_layer == r.pins);
  REQUIRE(plan.size() == 36);
  // Independent check of the plan's geometry: no more corridors between two neighbouring balls than fit there
  // (one), none through a gap's centre more often than its diagonal allows (two), and every corridor keeps the
  // track clear of the other balls.
  auto pos = [&](int i, int j) { return Point{i * pitch, j * pitch}; };
  for (int j = 0; j < 6; ++j)
    for (int i = 0; i < 6; ++i)
      for (const auto [di, dj] : {std::pair{1, 0}, std::pair{0, 1}}) {
        if (i + di >= 6 || j + dj >= 6) continue;
        int n = 0;
        for (const auto& c : plan) n += crosses_middle(tail_of(c), pos(i, j), pos(i + di, j + dj));
        CHECK(n <= 1);
      }
  for (int j = 0; j < 5; ++j)
    for (int i = 0; i < 5; ++i) {
      const Point centre{i * pitch + pitch / 2, j * pitch + pitch / 2};
      int n = 0;
      for (const auto& c : plan) n += static_cast<int>(std::count(c.tail.begin(), c.tail.end(), centre)) + (c.b == centre);
      CHECK(n <= 2);
    }
  for (const auto& c : plan) {
    const auto pl = tail_of(c);
    for (const auto& p : b.pads) {
      if (p.pos == b.pads[static_cast<std::size_t>(c.pad)].pos) continue;
      for (std::size_t k = 0; k + 1 < pl.size(); ++k) CHECK(dist_point_segment(p.pos, pl[k], pl[k + 1]) >= static_cast<double>(ball / 2 + s + w / 2) - 1);
    }
    // ends outside the array
    const Point e = pl.back();
    CHECK((e.x < 0 || e.y < 0 || e.x > 5 * pitch || e.y > 5 * pitch));
  }
  // Deterministic.
  const auto again = route::plan_escapes_flow(b, needs, flow_input(w, s, 600'000, 1));
  REQUIRE(again.size() == plan.size());
  for (std::size_t i = 0; i < plan.size(); ++i) CHECK((again[i].pad == plan[i].pad && again[i].b == plan[i].b && again[i].tail == plan[i].tail));
}

TEST_CASE("escape flow: without channels between balls, inner rings take dog-bone vias and leave on the next layer", "[escape]") {
  // 0.8 mm pitch, 0.45 mm balls, 0.1 / 0.15 mm rules: 0.35 mm between balls is too narrow for one track.
  const Coord pitch = 800'000, ball = 450'000, w = 100'000, s = 150'000, via = 300'000;
  const auto b = bga(8, pitch, ball);
  std::vector<char> needs(b.pads.size(), 1);
  route::FlowEscapeStats st;
  const auto plan = route::plan_escapes_flow(b, needs, flow_input(w, s, via, 4), {}, &st);
  REQUIRE(st.rings.size() == 4);
  CHECK(st.rings[0].pad_layer == 28);  // the perimeter leaves on its own layer
  CHECK(st.rings[1].pad_layer == 0);
  int via_pins = 0, other = 0;
  for (const auto& r : st.rings) via_pins += r.other_layer + r.via_only, other += r.other_layer;
  CHECK(via_pins == 36);  // every inner ball gets a via site
  CHECK(other > 0);
  CHECK(st.per_layer[0] == 28);
  CHECK(st.per_layer[1] > 0);  // the nearest layer first
  std::set<std::pair<Coord, Coord>> sites;
  for (const auto& c : plan) {
    if (!c.via) continue;
    CHECK(sites.insert({c.b.x, c.b.y}).second);  // one via per interstitial site
    CHECK(std::llabs(std::llabs(c.b.x - c.a.x) - pitch / 2) <= 1);
    CHECK(std::llabs(std::llabs(c.b.y - c.a.y) - pitch / 2) <= 1);
    if (c.tail.empty()) continue;
    CHECK(c.tail_layer > 0);
    // On the via layers the vias are the obstacles: the tail keeps clear of every other planned via.
    for (const auto& o : plan)
      if (o.via && o.pad != c.pad)
        for (std::size_t k = 0; k + 1 < c.tail.size(); ++k)
          CHECK(dist_point_segment(o.b, c.tail[k], c.tail[k + 1]) >= static_cast<double>(via / 2 + s + w / 2) - 1);
  }
}

TEST_CASE("escape flow: shallow packages are planned exactly as version 1", "[escape]") {
  const auto b = bga(4, 800'000, 400'000);  // two rings only
  std::vector<char> needs(b.pads.size(), 1);
  const auto v1 = route::plan_escapes(b, needs, [](model::NetId) { return Coord{300'000}; });
  route::FlowEscapeStats st;
  const auto v2 = route::plan_escapes_flow(b, needs, flow_input(100'000, 100'000, 400'000, 2), {}, &st);
  CHECK(st.arrays == 0);
  REQUIRE(v1.size() == v2.size());
  for (std::size_t i = 0; i < v1.size(); ++i) CHECK((v1[i].pad == v2[i].pad && v1[i].b == v2[i].b && v1[i].via == v2[i].via && v2[i].tail.empty()));
  CHECK(route::array_rings(b, needs) == std::vector<int>(b.pads.size(), 0));
  const auto deep = bga(7, 800'000, 400'000);
  std::vector<char> all(deep.pads.size(), 1);
  const auto rings = route::array_rings(deep, all);
  CHECK(rings[0] == 1);
  CHECK(rings[static_cast<std::size_t>(3 * 7 + 3)] == 4);
}

// ---- Escape analysis: outstanding obligations and the board's custom rules ----

namespace {

// Two-layer board: U1 and J1, ten 0.6 x 1.5 mm SMD pads each at 1 mm pitch, pad k of both on net Nk; U1 has a
// courtyard. `extra` adds copper inside the board, `dru` custom rules.
struct RuleBoard {
  std::filesystem::path dir;
  model::Board board;
  model::DesignRules rules;
  RuleBoard(const std::string& name, const std::string& extra, const std::string& dru) {
    dir = std::filesystem::temp_directory_path() / ("tmk_escape_" + name + "_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::string nets = "(net 0 \"\")";
    for (int k = 0; k < 10; ++k) nets += " (net " + std::to_string(k + 1) + " \"N" + std::to_string(k) + "\")";
    auto fp = [](const char* ref, double y, bool courtyard) {
      std::string s = std::string("  (footprint \"P\" (layer \"F.Cu\") (at 10 ") + std::to_string(y) + ")\n    (property \"Reference\" \"" + ref +
                      "\" (at 0 0) (layer \"F.SilkS\"))\n";
      if (courtyard) s += "    (fp_rect (start -5 -1.2) (end 5 1.2) (layer \"F.CrtYd\") (stroke (width 0.05) (type solid)))\n";
      for (int k = 0; k < 10; ++k)
        s += "    (pad \"" + std::to_string(k + 1) + "\" smd rect (at " + std::to_string(k - 4.5) + " 0) (size 0.6 1.5) (layers \"F.Cu\") (net " +
             std::to_string(k + 1) + " \"N" + std::to_string(k) + "\"))\n";
      return s + "  )\n";
    };
    const std::string pcb = "(kicad_pcb (version 20240108) (generator \"pcbnew\")\n"
                            "  (layers (0 \"F.Cu\" signal) (31 \"B.Cu\" signal) (37 \"F.SilkS\" user) (39 \"F.CrtYd\" user) (44 \"Edge.Cuts\" user))\n  " +
                            nets + "\n" + fp("U1", 5, true) + fp("J1", 15, false) +
                            "  (gr_rect (start 0 0) (end 20 20) (layer \"Edge.Cuts\") (stroke (width 0.1) (type solid)))\n" + extra + ")\n";
    std::ofstream(dir / "b.kicad_pcb") << pcb;
    if (!dru.empty()) std::ofstream(dir / "b.kicad_dru") << "(version 1)\n" + dru;
    board = io::read_board_file((dir / "b.kicad_pcb").string()).board;
    rules = io::read_design_rules((dir / "b.kicad_pcb").string());
  }
  ~RuleBoard() { std::filesystem::remove_all(dir); }
  std::vector<route::PartEscape> analyse(bool soft_zones = false) {
    route::Obstacles obs(board, rules);
    obs.set_soft_zones(soft_zones);
    return route::analyse_escapes(board, rules, obs);
  }
};

// `ref` by value: GCC's -Wdangling-reference rejects a returned reference when an argument binds a temporary.
const route::PartEscape& part(const std::vector<route::PartEscape>& parts, std::string_view ref) {
  for (const auto& pe : parts)
    if (pe.ref == ref) return pe;
  FAIL("no part " << ref);
  return parts.front();
}

}  // namespace

TEST_CASE("escape analysis: pins of nets the existing copper already completes are satisfied, not obligations", "[escape][rules]") {
  // N2's pads on U1 and J1 are joined by a track; every other net is still open.
  RuleBoard rb("satisfied", "  (segment (start 8 5) (end 8 15) (width 0.25) (layer \"B.Cu\") (net 3))\n"
                            "  (via (at 8 5.9) (size 0.6) (drill 0.3) (layers \"F.Cu\" \"B.Cu\") (net 3))\n"
                            "  (segment (start 7.5 5) (end 8 5.9) (width 0.25) (layer \"F.Cu\") (net 3))\n"
                            "  (via (at 8 14.1) (size 0.6) (drill 0.3) (layers \"F.Cu\" \"B.Cu\") (net 3))\n"
                            "  (segment (start 7.5 15) (end 8 14.1) (width 0.25) (layer \"F.Cu\") (net 3))\n",
               "");
  const auto parts = rb.analyse();
  for (const char* ref : {"U1", "J1"}) {
    const auto& pe = part(parts, ref);
    CHECK(pe.satisfied == 1);
    CHECK(pe.pins == 9);
    CHECK(pe.escapable == 9);
  }
}

TEST_CASE("escape analysis: with soft zones a plane no pad touches keeps its net outstanding, as in the router's plan", "[escape][rules]") {
  // N9's two pads are joined by a track, and N9 has a B.Cu plane that neither touches. With hard fills the
  // router's planner has nothing to do for N9; with soft zones it connects a pad to the plane (doc 05 §26).
  const std::string pts = "(pts (xy 0.5 0.5) (xy 19.5 0.5) (xy 19.5 19.5) (xy 0.5 19.5))";
  RuleBoard rb("plane", "  (segment (start 14.5 5) (end 14.5 15) (width 0.25) (layer \"F.Cu\") (net 10))\n"
                        "  (zone (net 10) (net_name \"N9\") (layer \"B.Cu\") (hatch edge 0.5) (connect_pads (clearance 0.2)) (min_thickness 0.2)\n"
                        "    (fill yes (thermal_gap 0.3) (thermal_bridge_width 0.3))\n    (polygon " + pts + ")\n    (filled_polygon (layer \"B.Cu\") " + pts + "))\n",
               "");
  for (const char* ref : {"U1", "J1"}) {
    const auto hard = rb.analyse(false), soft = rb.analyse(true);
    CHECK(part(hard, ref).satisfied == 1);
    CHECK(part(hard, ref).pins == 9);
    CHECK(part(soft, ref).satisfied == 0);
    CHECK(part(soft, ref).pins == 10);
    CHECK(part(soft, ref).escapable == 10);
  }
  // The router's planner on the same board: every net has two pads, so one connection per outstanding net, and
  // with soft zones N9's is the pad-to-plane one.
  for (bool soft : {false, true}) {
    route::RouterOptions o;
    o.soft_zones = soft;
    o.work_budget = 2'000'000;
    o.time_limit_s = 3600;  // the work budget decides, also in a slow sanitizer build
    o.gpu_device = -1;
    const auto res = route::Router(rb.board, rb.rules, o).run();
    CHECK(res.connections == part(rb.analyse(soft), "U1").pins);
    CHECK(res.routed == res.connections);
  }
}

TEST_CASE("escape analysis: a pin that custom disallow rules wall in is dead, and named as such", "[escape][rules]") {
  // N3 may neither run tracks nor place vias in U1's courtyard, so its U1 pin has no way out; its J1 pin and the
  // other nets are unaffected. N5 may not use F.Cu, its pads' only layer.
  const std::string dru = "(rule \"N3 off U1\" (condition \"A.intersectsCourtyard('U1') && A.NetName == 'N3'\") (constraint disallow track via))\n"
                          "(rule \"N5 not on top\" (layer \"F.Cu\") (condition \"A.NetName == 'N5'\") (constraint disallow track))\n";
  RuleBoard ruled("walled", "", dru);
  const auto parts = ruled.analyse();
  const auto& u1 = part(parts, "U1");
  CHECK(u1.pins == 10);
  CHECK(u1.escapable == 8);
  std::set<std::string> dead;
  for (const auto& d : u1.dead) {
    dead.insert(ruled.board.pads[static_cast<std::size_t>(d.pad)].number);
    CHECK(d.reason.find("walled in by custom disallow rules") != std::string::npos);
  }
  CHECK(dead == std::set<std::string>{"4", "6"});
  const auto& j1 = part(parts, "J1");
  REQUIRE(j1.dead.size() == 1);  // N5 is off F.Cu everywhere
  CHECK(ruled.board.pads[static_cast<std::size_t>(j1.dead[0].pad)].number == "6");
  RuleBoard free_board("free", "", "");
  for (const auto& pe : free_board.analyse()) CHECK(pe.dead.empty());
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Custom-rule routing and DRC (doc 05 §27): disallow, physical hole clearance and via-only keepouts.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>
#include <tuple>

#include "drc/drc.hpp"
#include "drc/rule_engine.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "route/obstacles.hpp"
#include "route/router.hpp"

using namespace tmk;
namespace fs = std::filesystem;

namespace {

// Four layers, 20 x 10 mm; SIG and GND each connect 1 mm F.Cu pads at x = 4 (R1) and x = 16 (R2).
// `extra` adds keepouts or copper inside the board.
std::string board_text(const std::string& extra) {
  auto fp = [](const char* ref, double x) {
    return std::string("  (footprint \"R\" (layer \"F.Cu\") (at ") + std::to_string(x) + " 5)\n" +
           "    (property \"Reference\" \"" + ref + "\" (at 0 0) (layer \"F.SilkS\"))\n" +
           "    (pad \"1\" smd rect (at 0 -2) (size 1 1) (layers \"F.Cu\") (net 1 \"SIG\"))\n" +
           "    (pad \"2\" smd rect (at 0 2) (size 1 1) (layers \"F.Cu\") (net 2 \"GND\"))\n  )\n";
  };
  return "(kicad_pcb (version 20240108) (generator \"pcbnew\")\n"
         "  (layers (0 \"F.Cu\" signal) (1 \"In1.Cu\" signal) (2 \"In2.Cu\" signal) (31 \"B.Cu\" signal) (37 \"F.SilkS\" user)\n"
         "    (44 \"Edge.Cuts\" user))\n"
         "  (net 0 \"\") (net 1 \"SIG\") (net 2 \"GND\")\n" +
         fp("R1", 4) + fp("R2", 16) +
         "  (gr_rect (start 0 0) (end 20 10) (layer \"Edge.Cuts\") (stroke (width 0.1) (type solid)))\n" + extra + ")\n";
}

// A keepout on the outer layers across the middle (x 9..11): connections must cross it on an inner layer.
const char* kOuterWall =
    "  (zone (net 0) (net_name \"\") (layers \"F.Cu\" \"B.Cu\") (name \"wall\") (hatch edge 0.5) (connect_pads (clearance 0))\n"
    "    (min_thickness 0.25) (keepout (tracks not_allowed) (vias not_allowed) (pads allowed) (copperpour allowed) (footprints allowed))\n"
    "    (fill (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon (pts (xy 9 0) (xy 11 0) (xy 11 10) (xy 9 10))))\n";

const char* kInnerGndOnly =
    "(version 1)\n(rule \"inner GND only\" (layer inner) (condition \"A.NetName != 'GND'\") (constraint disallow track))\n";

struct Files {
  fs::path dir, pcb;
  Files(const std::string& name, const std::string& board, const std::string& dru) {
    // Per process: several builds may run their tests at the same time.
    dir = fs::temp_directory_path() / ("tmk_design_rules_" + name + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    pcb = dir / "b.kicad_pcb";
    std::ofstream(pcb) << board;
    if (!dru.empty()) std::ofstream(dir / "b.kicad_dru") << dru;
  }
  ~Files() { fs::remove_all(dir); }
};

route::RouteResult route_board(const Files& f) {
  const auto lb = io::read_board_file(f.pcb.string());
  const auto rules = io::read_design_rules(f.pcb.string());
  route::RouterOptions o;
  o.work_budget = 2'000'000;
  o.gpu_device = -1;
  o.time_limit_s = 600;
  return route::Router(lb.board, rules, o).run();
}

bool has_track(const route::RouteResult& r, model::NetId net, bool inner) {
  for (const auto& t : r.tracks)
    if (t.net == net && (t.layer == 1 || t.layer == 2) == inner) return true;
  return false;
}

}  // namespace

TEST_CASE("disallow track on inner layers: other nets avoid them, GND may still use them", "[rules][route]") {
  const Files free_board("free", board_text(kOuterWall), "");
  const auto before = route_board(free_board);
  CHECK(before.routed == 2);  // both nets cross the wall on an inner layer
  CHECK(has_track(before, 1, true));

  const Files ruled("ruled", board_text(kOuterWall), kInnerGndOnly);
  const auto after = route_board(ruled);
  CHECK(after.routed == 1);             // SIG cannot cross the wall
  CHECK_FALSE(has_track(after, 1, true));
  CHECK(has_track(after, 2, true));      // GND still uses an inner layer
}

TEST_CASE("physical_hole_clearance keeps vias out of same-net SMD pads, except where the condition excludes them",
          "[rules][route]") {
  const std::string rule =
      "(rule \"no via in pad\" (constraint physical_hole_clearance (min 0.05mm))\n"
      "  (condition \"A.Type == 'Via' && B.Type == 'Pad' && B.Pad_Type == 'SMD' && B.Reference != 'R2'\"))\n";
  const Files f("vip", board_text(""), rule);
  auto lb = io::read_board_file(f.pcb.string());
  const auto with_rule = io::read_design_rules(f.pcb.string());
  const model::DesignRules no_rule = [&] {
    auto r = with_rule;
    r.custom.clear();
    return r;
  }();
  const geom::Point r1_sig{4'000'000, 3'000'000}, r2_sig{16'000'000, 3'000'000};
  const Coord d = 600'000, drill = 300'000;
  {
    route::Obstacles obs(lb.board, no_rule);
    CHECK(obs.via_state(r1_sig, d, drill, 1, 0, false) == 0);  // KiCad's default rules allow a via in its own pad
  }
  route::Obstacles obs(lb.board, with_rule);
  CHECK(obs.via_state(r1_sig, d, drill, 1, 0, false) == 2);
  CHECK(obs.fixed_via_code(r1_sig, d, drill, 0, 1) == route::Obstacles::kBlocked);  // cached path agrees
  CHECK(obs.via_state(r2_sig, d, drill, 1, 0, false) == 0);                      // R2 is excluded
  CHECK(obs.via_state({4'000'000, 4'300'000}, d, drill, 1, 0, false) == 0);        // beside the pad
  CHECK_FALSE(obs.needs_exact_routing());  // net-independent rule: cache remains valid
}

// A plated hole without a net at (10, 3), on the straight line between the two SIG pads: drill 0.8, pad 1.2 mm.
const char* kHoleInTheWay =
    "  (footprint \"H\" (layer \"F.Cu\") (at 10 3)\n"
    "    (property \"Reference\" \"H1\" (at 0 0) (layer \"F.SilkS\"))\n"
    "    (pad \"1\" thru_hole circle (at 0 0) (size 1.2 1.2) (drill 0.8) (layers \"*.Cu\"))\n  )\n";
const char* kHoleKeepaway = "(version 1)\n(rule \"hole keepaway\" (constraint physical_hole_clearance (min 1mm)))\n";

TEST_CASE("physical_hole_clearance: new tracks keep the rule's distance from holes", "[rules][route]") {
  const Files plain("phc_track_plain", board_text(kHoleInTheWay), "");
  const Files ruled("phc_track_ruled", board_text(kHoleInTheWay), kHoleKeepaway);
  const geom::Shape hole = geom::Shape::point({10'000'000, 3'000'000}, 400'000);
  // A 0.25 mm track 1.2 mm from the hole's centre: 0.475 mm from the pad (legal by clearance), 0.675 mm from the hole.
  const geom::Point a{8'000'000, 4'200'000}, b{12'000'000, 4'200'000};
  {
    auto lb = io::read_board_file(plain.pcb.string());
    const auto rules = io::read_design_rules(plain.pcb.string());
    route::Obstacles obs(lb.board, rules);
    CHECK(obs.segment_state(a, b, 0, 250'000, 1, false) == 0);
    CHECK(obs.fixed_code({10'000'000, 4'200'000}, 0, 125'000, 0, 1) != route::Obstacles::kBlocked);
  }
  {
    auto lb = io::read_board_file(ruled.pcb.string());
    const auto rules = io::read_design_rules(ruled.pcb.string());
    route::Obstacles obs(lb.board, rules);
    for (int l = 0; l < 4; ++l) CHECK(obs.segment_state(a, b, l, 250'000, 1, false) == 2);  // the hole is on every layer
    CHECK(obs.fixed_code({10'000'000, 4'200'000}, 0, 125'000, 0, 1) == route::Obstacles::kBlocked);  // cached path agrees
    CHECK(obs.segment_state({8'000'000, 5'000'000}, {12'000'000, 5'000'000}, 0, 250'000, 1, false) == 0);  // 1.475 mm away
    CHECK_FALSE(obs.needs_exact_routing());
  }
  // Routed: without the rule the SIG track passes the hole closer than 1 mm; with it no track does.
  auto near_hole = [&](const route::RouteResult& r) {
    int n = 0;
    for (const auto& t : r.tracks) n += geom::closer_than(geom::Shape::segment(t.a, t.b, t.width / 2), hole, 1'000'000);
    return n;
  };
  const auto before = route_board(plain);
  REQUIRE(before.routed == 2);
  CHECK(near_hole(before) > 0);
  const auto after = route_board(ruled);
  CHECK(after.routed == 2);
  CHECK(near_hole(after) == 0);
}

TEST_CASE("physical_hole_clearance: via holes and routed copper of any net keep the rule's distance", "[rules][route]") {
  const Files f("phc_routed", board_text(""), kHoleKeepaway);
  auto lb = io::read_board_file(f.pcb.string());
  const auto with_rule = io::read_design_rules(f.pcb.string());
  const model::DesignRules no_rule = [&] {
    auto r = with_rule;
    r.custom.clear();
    return r;
  }();
  route::Obstacles ruled(lb.board, with_rule), plain(lb.board, no_rule);
  // Routed copper is registered after the models are built: a GND track on In1 (connection 7) and a GND via (connection 8).
  model::Track t;
  t.a = {6'000'000, 5'000'000};
  t.b = {14'000'000, 5'000'000};
  t.width = 250'000;
  t.layer = 1;
  t.net = 2;
  lb.board.tracks.push_back(t);
  model::Via v;
  v.pos = {10'000'000, 8'000'000};
  v.size = 600'000;
  v.drill = 300'000;
  v.layer_top = 0;
  v.layer_bottom = 3;
  v.net = 2;
  lb.board.vias.push_back(v);
  for (route::Obstacles* o : {&ruled, &plain}) {
    o->add_track(static_cast<int>(lb.board.tracks.size()) - 1, 7);
    o->add_via(static_cast<int>(lb.board.vias.size()) - 1, 8);
  }
  // A new via 0.9 mm beside the routed track: copper 0.475 mm apart, hole 0.625 mm from the track.
  const geom::Point vp{10'000'000, 5'900'000};
  for (model::NetId net : {1, 2}) {  // another net, and the track's own net
    CHECK(plain.via_state(vp, 600'000, 300'000, net, 0, false) == 0);
    CHECK(ruled.via_state(vp, 600'000, 300'000, net, 0, false) == 2);
    std::vector<int> owners;
    CHECK(ruled.via_state(vp, 600'000, 300'000, net, 0, true, &owners) == 1);  // rippable, not fixed
    CHECK(std::find(owners.begin(), owners.end(), 7) != owners.end());
    // The search's routed-copper check agrees with the exact one.
    CHECK(ruled.routed_state(geom::Shape::point(vp, 300'000), 1, net, drc::ItemKind::Via, false, nullptr, true, 150'000) == 2);
    CHECK(ruled.routed_via_state(geom::Shape::point(vp, 300'000), model::layer_bit(1), net, false, 150'000) == 2);
  }
  // A new track 0.9 mm beside the routed via: copper 0.475 mm apart, 0.625 mm from its hole.
  const geom::Point a{8'000'000, 8'900'000}, b{12'000'000, 8'900'000};
  for (model::NetId net : {1, 2}) {
    CHECK(plain.segment_state(a, b, 3, 250'000, net, false) == 0);
    CHECK(ruled.segment_state(a, b, 3, 250'000, net, false) == 2);
    std::vector<int> owners;
    CHECK(ruled.segment_state(a, b, 3, 250'000, net, true, &owners) == 1);
    CHECK(std::find(owners.begin(), owners.end(), 8) != owners.end());
    CHECK(ruled.routed_state(geom::Shape::segment(a, b, 125'000), 3, net, drc::ItemKind::Track, false, nullptr) == 2);
  }
}

TEST_CASE("physical_hole_clearance between a net's vias and its own tracks leaves the net without vias", "[rules][route]") {
  // Both nets need a via to cross the wall, and the tracks that end in a via touch its hole: KiCad reports each
  // such pair, so no via may be placed.
  const Files free_board("phc_own_free", board_text(kOuterWall), "");
  const auto before = route_board(free_board);
  REQUIRE(before.routed == 2);
  REQUIRE_FALSE(before.vias.empty());
  const Files f("phc_own", board_text(kOuterWall), kHoleKeepaway);
  const auto r = route_board(f);
  CHECK(r.vias.empty());
  CHECK(r.routed == 0);
  // A rule limited to vias against pads leaves vias usable.
  const Files pads_only("phc_own_pads", board_text(kOuterWall),
                        "(version 1)\n(rule \"no via in pad\" (constraint physical_hole_clearance (min 0.05mm))\n"
                        "  (condition \"A.Type == 'Via' && B.Type == 'Pad'\"))\n");
  CHECK(route_board(pads_only).routed == 2);
}

TEST_CASE("a keepout that forbids only vias blocks vias and lets tracks through", "[rules][route]") {
  const std::string ko =
      "  (zone (net 0) (net_name \"\") (layers \"B.Cu\") (name \"tc\") (hatch edge 0.5) (connect_pads (clearance 0))\n"
      "    (min_thickness 0.25) (keepout (tracks allowed) (vias not_allowed) (pads allowed) (copperpour not_allowed) (footprints allowed))\n"
      "    (fill (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon (pts (xy 8 4) (xy 12 4) (xy 12 6) (xy 8 6))))\n";
  const Files f("viako", board_text(ko), "");
  auto lb = io::read_board_file(f.pcb.string());
  const auto rules = io::read_design_rules(f.pcb.string());
  route::Obstacles obs(lb.board, rules);
  const geom::Point inside{10'000'000, 5'000'000};
  CHECK(obs.via_state(inside, 600'000, 300'000, 1, 0, false) == 2);
  CHECK(obs.fixed_via_code(inside, 600'000, 300'000, 0, 1) == route::Obstacles::kBlocked);
  CHECK(obs.segment_state({8'500'000, 5'000'000}, {11'500'000, 5'000'000}, 3, 250'000, 1, false) == 0);
}

namespace {
// Routed copper the DRC reports as items_not_allowed under the rules of `f` (the router's result added to the board).
int disallowed_routed(const Files& f, const route::RouteResult& r) {
  auto lb = io::read_board_file(f.pcb.string());
  const auto rules = io::read_design_rules(f.pcb.string());
  const auto fixed = lb.board.tracks.size() + lb.board.vias.size();
  lb.board.tracks.insert(lb.board.tracks.end(), r.tracks.begin(), r.tracks.end());
  lb.board.vias.insert(lb.board.vias.end(), r.vias.begin(), r.vias.end());
  const drc::RuleEngine re(lb.board, rules);
  int n = 0;
  for (const auto& it : drc::build_copper(lb.board).items) {
    if (it.kind != drc::ItemKind::Track && it.kind != drc::ItemKind::Via) continue;
    bool hit = false;
    for (int l = 0; l < lb.board.copper_count(); ++l)
      if (it.layers & model::layer_bit(l)) hit = hit || re.disallowed(it, l).has_value();
    n += hit;
  }
  return fixed == 0 ? n : -1;  // these boards start without copper
}

// Routes `f` with its custom rules removed: what the rules have to change.
route::RouteResult route_without_rules(const Files& f) {
  const auto lb = io::read_board_file(f.pcb.string());
  auto rules = io::read_design_rules(f.pcb.string());
  rules.custom.clear();
  route::RouterOptions o;
  o.work_budget = 2'000'000;
  o.gpu_device = -1;
  o.time_limit_s = 600;
  return route::Router(lb.board, rules, o).run();
}
}  // namespace

TEST_CASE("positional disallow: the router keeps a named net out of an area and lets other nets through", "[rules][route]") {
  // A rule area without keepout flags across the middle of F.Cu; the rule closes it to SIG only, so the cached
  // per-class obstacle codes (SIG and GND share a class) cannot represent it.
  const std::string area =
      "  (zone (net 0) (net_name \"\") (layers \"F.Cu\") (name \"noroute\") (hatch edge 0.5) (connect_pads (clearance 0))\n"
      "    (min_thickness 0.25) (keepout (tracks allowed) (vias allowed) (pads allowed) (copperpour allowed) (footprints allowed))\n"
      "    (fill (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon (pts (xy 9 0) (xy 11 0) (xy 11 10) (xy 9 10))))\n";
  const std::string rule =
      "(version 1)\n(rule \"SIG off noroute\" (condition \"A.intersectsArea('noroute') && A.NetName == 'SIG'\") (constraint disallow track via))\n";
  const Files f("area_net", board_text(area), rule);
  CHECK(disallowed_routed(f, route_without_rules(f)) > 0);  // SIG's straight F.Cu route crosses the area
  const auto r = route_board(f);
  CHECK(r.routed == 2);
  CHECK(disallowed_routed(f, r) == 0);
  bool gnd_crosses = false;  // on F.Cu, through the area
  for (const auto& t : r.tracks) gnd_crosses = gnd_crosses || (t.net == 2 && t.layer == 0 && std::min(t.a.x, t.b.x) < 10'000'000 && std::max(t.a.x, t.b.x) > 10'000'000);
  CHECK(gnd_crosses);
}

TEST_CASE("positional disallow: no vias in a footprint courtyard", "[rules][route]") {
  // The outer-layer wall forces both nets onto an inner layer; U1's courtyard (x 7..13) covers the wall's
  // surroundings, so the vias must sit beside the pads instead.
  const std::string u1 =
      "  (footprint \"U\" (layer \"F.Cu\") (at 10 5)\n"
      "    (property \"Reference\" \"U1\" (at 0 0) (layer \"F.SilkS\"))\n"
      "    (fp_rect (start -3 -4.5) (end 3 4.5) (layer \"F.CrtYd\") (stroke (width 0.05) (type solid)))\n  )\n";
  const std::string rule = "(version 1)\n(rule \"no vias under U1\" (condition \"A.intersectsCourtyard('U1')\") (constraint disallow via))\n";
  const Files f("court_vias", board_text(std::string(kOuterWall) + u1), rule);
  CHECK(disallowed_routed(f, route_without_rules(f)) > 0);
  const auto r = route_board(f);
  CHECK(r.routed == 2);
  CHECK_FALSE(r.vias.empty());
  CHECK(disallowed_routed(f, r) == 0);
}

TEST_CASE("disallow via also forbids blind and buried vias", "[rules][route]") {
  const std::string rule = "(version 1)\n(rule \"no SIG vias\" (condition \"A.NetName == 'SIG'\") (constraint disallow via))\n";
  const Files f("noblind", board_text(kOuterWall), rule);
  const auto lb = io::read_board_file(f.pcb.string());
  auto rules = io::read_design_rules(f.pcb.string());
  rules.minimums.allow_blind_buried_vias = true;
  route::RouterOptions o;
  o.work_budget = 2'000'000;
  o.gpu_device = -1;
  o.time_limit_s = 600;
  o.blind_vias = true;
  const auto r = route::Router(lb.board, rules, o).run();
  for (const auto& v : r.vias) CHECK(v.net != 1);
  CHECK(r.routed == 1);  // SIG needs a via to cross the wall; GND does not care
}

TEST_CASE("disallow blind_via or buried_via or micro_via alone stops the router placing that via type", "[rules][route]") {
  // Vias are forbidden on B.Cu everywhere, so no through via fits; the wall is crossed on In1 through a via from
  // F.Cu to In1: a micro via or a blind via, whichever the options allow.
  const std::string no_bottom_vias =
      "  (zone (net 0) (net_name \"\") (layers \"B.Cu\") (name \"nobv\") (hatch edge 0.5) (connect_pads (clearance 0))\n"
      "    (min_thickness 0.25) (keepout (tracks allowed) (vias not_allowed) (pads allowed) (copperpour allowed) (footprints allowed))\n"
      "    (fill (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon (pts (xy 0 0) (xy 20 0) (xy 20 10) (xy 0 10))))\n";
  const std::string board = board_text(std::string(kOuterWall) + no_bottom_vias);
  auto run = [&](const std::string& name, const std::string& word, bool blind, bool micro) {
    const Files f(name, board, word.empty() ? std::string() : "(version 1)\n(rule \"SIG\" (condition \"A.NetName == 'SIG'\") (constraint disallow " + word + "))\n");
    const auto lb = io::read_board_file(f.pcb.string());
    auto rules = io::read_design_rules(f.pcb.string());
    rules.minimums.allow_blind_buried_vias = true;
    rules.minimums.allow_microvias = true;
    route::RouterOptions o;
    o.work_budget = 2'000'000;
    o.gpu_device = -1;
    o.time_limit_s = 600;
    o.blind_vias = blind;
    o.micro_vias = micro;
    return route::Router(lb.board, rules, o).run();
  };
  auto sig_vias = [](const route::RouteResult& r, model::ViaType type) {
    int n = 0;
    for (const auto& v : r.vias) n += v.net == 1 && v.type == type;
    return n;
  };
  {
    const auto r = run("bv_free", "", true, false);
    REQUIRE(r.routed == 2);
    CHECK(sig_vias(r, model::ViaType::Blind) > 0);
  }
  for (const char* word : {"blind_via", "buried_via"}) {  // one via type in the board model: either word forbids both
    const auto r = run(std::string("bv_") + word, word, true, false);
    CHECK(r.routed == 1);  // GND still crosses
    for (const auto& v : r.vias) CHECK(v.net != 1);
  }
  {
    const auto r = run("mv_free", "", false, true);
    REQUIRE(r.routed == 2);
    CHECK(sig_vias(r, model::ViaType::Micro) > 0);
  }
  {
    const auto r = run("mv_rule", "micro_via", false, true);
    CHECK(r.routed == 1);
    for (const auto& v : r.vias) CHECK(v.net != 1);
  }
  {
    // With both types available, SIG falls back to the type the rule leaves it.
    const auto r = run("mv_rule_blind", "micro_via", true, true);
    CHECK(r.routed == 2);
    CHECK(sig_vias(r, model::ViaType::Micro) == 0);
    CHECK(sig_vias(r, model::ViaType::Blind) > 0);
    const auto q = run("bv_rule_micro", "blind_via", true, true);
    CHECK(q.routed == 2);
    CHECK(sig_vias(q, model::ViaType::Blind) == 0);
    CHECK(sig_vias(q, model::ViaType::Micro) > 0);
  }
}

TEST_CASE("fixed via codes: the one-pass check equals the per-layer reference", "[rules][route]") {
  // Keepouts (outer wall, via-only), fixed tracks of both nets on several layers, and a physical hole rule.
  const std::string extra = std::string(kOuterWall) +
                            "  (zone (net 0) (net_name \"\") (layers \"B.Cu\") (name \"tc\") (hatch edge 0.5) (connect_pads (clearance 0))\n"
                            "    (min_thickness 0.25) (keepout (tracks allowed) (vias not_allowed) (pads allowed) (copperpour not_allowed) (footprints allowed))\n"
                            "    (fill (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon (pts (xy 13 6) (xy 15 6) (xy 15 8) (xy 13 8))))\n"
                            "  (segment (start 2 8) (end 18 8) (width 0.25) (layer \"In1.Cu\") (net 1))\n"
                            "  (segment (start 5 1) (end 5 9) (width 0.3) (layer \"B.Cu\") (net 2))\n"
                            "  (segment (start 1 1.5) (end 19 1.5) (width 0.2) (layer \"F.Cu\") (net 2))\n";
  const std::string rule =
      "(version 1)\n(rule \"no via in pad\" (constraint physical_hole_clearance (min 0.05mm))\n"
      "  (condition \"A.Type == 'Via' && B.Type == 'Pad' && B.Pad_Type == 'SMD' && B.Reference != 'R2'\"))\n";
  const Files f("vref", board_text(extra), rule);
  auto lb = io::read_board_file(f.pcb.string());
  const auto rules = io::read_design_rules(f.pcb.string());
  route::Obstacles obs(lb.board, rules);
  int compared = 0, blocked = 0, coded = 0;
  for (const auto& [d, drill, margin] : {std::tuple<Coord, Coord, Coord>{600'000, 300'000, 0}, {450'000, 200'000, 36'000}}) {
    for (Coord y = -500'000; y <= 10'500'000; y += 130'000)
      for (Coord x = -500'000; x <= 20'500'000; x += 130'000)
        for (model::NetId net : {1, 2}) {
          const geom::Point p{x, y};
          const auto fast = obs.fixed_via_code(p, d, drill, margin, net);
          REQUIRE(fast == obs.fixed_via_code_reference(p, d, drill, margin, net));
          ++compared;
          blocked += fast == route::Obstacles::kBlocked;
          coded += fast > 0;
        }
  }
  // The sample covers every outcome: free, blocked, and legal only for one net.
  CHECK(blocked > 0);
  CHECK(coded > 0);
  CHECK(blocked + coded < compared);
}

TEST_CASE("fixed via codes: one-pass and per-layer paths agree with soft zones and vias off pads", "[rules][route][planes]") {
  // The one-pass check repeats fixed_code's copper loop, so the plane-aware options (doc 05 §26) must act in both.
  const std::string path = std::string(TM_SOURCE_DIR) + "/tests/boards/plane_smd/plane_smd.kicad_pcb";
  if (!fs::exists(path)) SKIP("board missing: " + path);
  auto lb = io::read_board_file(path);
  const auto rules = io::read_design_rules(path);
  const geom::Box box = lb.board.edge_bbox();
  REQUIRE(!box.empty());
  const auto nets = static_cast<model::NetId>(lb.board.nets.size());
  for (const auto& [soft, off_pads, exempt] : {std::tuple<bool, bool, bool>{false, false, false}, {true, false, false}, {false, true, false},
                                               {true, true, false}, {true, true, true}}) {
    route::Obstacles obs(lb.board, rules);
    obs.set_soft_zones(soft);
    obs.set_vias_off_pads(off_pads, 2'000'000);
    obs.set_pad_via_exempt(exempt);
    int compared = 0, blocked = 0, differs = 0;
    route::Obstacles base(lb.board, rules);  // the same without vias off pads
    base.set_soft_zones(soft);
    auto compare = [&](geom::Point p, model::NetId net) {
      const auto fast = obs.fixed_via_code(p, 450'000, 200'000, 0, net);
      REQUIRE(fast == obs.fixed_via_code_reference(p, 450'000, 200'000, 0, net));
      ++compared;
      blocked += fast == route::Obstacles::kBlocked;
      differs += fast != base.fixed_via_code(p, 450'000, 200'000, 0, net);
    };
    for (Coord y = box.y0 - 300'000; y <= box.y1 + 300'000; y += 370'000)
      for (Coord x = box.x0 - 300'000; x <= box.x1 + 300'000; x += 370'000)
        for (model::NetId net = 1; net < nets; net += std::max<model::NetId>(1, nets / 3)) compare({x, y}, net);
    // A via of the pad's own net in each pad: where "vias off pads" decides.
    for (const auto& pd : lb.board.pads)
      if (pd.net > 0) compare(pd.pos, pd.net);
    CAPTURE(soft, off_pads, exempt);
    CHECK(blocked > 0);
    // Not vacuous: hard plane fills block every via on this board, soft ones do not; vias off pads then changes
    // answers in the pads, and the exemption cancels it.
    CHECK((blocked < compared) == soft);
    CHECK((differs > 0) == (soft && off_pads && !exempt));
  }
}

TEST_CASE("disallow rules the router cannot apply are named in the rule warnings", "[rules]") {
  const std::string area =
      "  (zone (net 0) (net_name \"\") (layers \"F.Cu\") (name \"noroute\") (hatch edge 0.5) (connect_pads (clearance 0))\n"
      "    (min_thickness 0.25) (keepout (tracks allowed) (vias allowed) (pads allowed) (copperpour allowed) (footprints allowed))\n"
      "    (fill (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon (pts (xy 9 0) (xy 11 0) (xy 11 10) (xy 9 10))))\n";
  const std::string rules = std::string(kInnerGndOnly) +
                            "(rule \"by area\" (condition \"A.intersectsArea('noroute')\") (constraint disallow track via))\n"
                            "(rule \"broken\" (condition \"A.NetName != 'GND' &&& (\") (constraint disallow track))\n"
                            "(rule \"odd property\" (condition \"A.Frobnicate == 3\") (constraint disallow track))\n";
  const Files f("warn", board_text(area), rules);
  const auto lb = io::read_board_file(f.pcb.string());
  const auto dr = io::read_design_rules(f.pcb.string());
  const drc::RuleEngine re(lb.board, dr);
  auto names = [&](const std::string& rule) {
    return std::any_of(re.warnings().begin(), re.warnings().end(), [&](const std::string& w) { return w.find("rule '" + rule + "'") != std::string::npos; });
  };
  CHECK(names("broken"));
  CHECK(names("odd property"));
  CHECK_FALSE(names("inner GND only"));  // applied by the router as a layer mask
  CHECK_FALSE(names("by area"));         // applied by the router to each new track and via
  CHECK(re.positional_disallow());
  // The per-net masks are unchanged: the area rule is not a rule about whole layers.
  for (int l = 0; l < 4; ++l) CHECK(re.track_allowed(1, l) == (l == 0 || l == 3));
  CHECK(re.via_allowed(1));
}

TEST_CASE("DRC reports disallowed tracks and vias in pads as KiCad does", "[rules][drc]") {
  const std::string copper =
      "  (segment (start 6 3) (end 14 3) (width 0.25) (layer \"In1.Cu\") (net 1))\n"
      "  (segment (start 6 7) (end 14 7) (width 0.25) (layer \"In2.Cu\") (net 2))\n"
      "  (via (at 4 3) (size 0.6) (drill 0.3) (layers \"F.Cu\" \"B.Cu\") (net 1))\n";
  const std::string rules = std::string(kInnerGndOnly) +
                            "(rule \"no via in pad\" (constraint physical_hole_clearance (min 0.05mm))\n"
                            "  (condition \"A.Type == 'Via' && B.Type == 'Pad' && B.Pad_Type == 'SMD'\"))\n";
  auto count = [](const drc::DrcReport& r, const std::string& type) {
    int n = 0;
    for (const auto& v : r.violations) n += v.type == type;
    return n;
  };
  const Files plain("drc_plain", board_text(copper), "");
  const auto lb0 = io::read_board_file(plain.pcb.string());
  const auto r0 = drc::run_drc(lb0.board, io::read_design_rules(plain.pcb.string()));
  CHECK(count(r0, "items_not_allowed") == 0);
  CHECK(count(r0, "hole_clearance") == 0);

  const Files ruled("drc_ruled", board_text(copper), rules);
  const auto lb = io::read_board_file(ruled.pcb.string());
  const auto r = drc::run_drc(lb.board, io::read_design_rules(ruled.pcb.string()));
  CHECK(count(r, "items_not_allowed") == 1);  // the SIG track on In1; the GND track on In2 is allowed
  CHECK(count(r, "hole_clearance") == 1);     // the via in R1's SIG pad (same net)
}

namespace {
const char* kParityCopper =
    "(segment (start 6 3) (end 9 3) (width 0.2) (layer \"F.Cu\") (net 1))\n"
    "(segment (start 6 5) (end 9 5) (width 0.25) (layer \"F.Cu\") (net 1))\n"
    "(segment (start 6 7) (end 9 7) (width 0.4) (layer \"B.Cu\") (net 2))\n"
    "(via (at 12 3) (size 0.8) (drill 0.4) (layers \"F.Cu\" \"B.Cu\") (net 1))\n"
    "(via blind (at 12 5) (size 0.8) (drill 0.4) (layers \"F.Cu\" \"In1.Cu\") (net 1))\n"
    "(via blind (at 12 7) (size 0.8) (drill 0.4) (layers \"In1.Cu\" \"In2.Cu\") (net 1))\n"
    "(via micro (at 14 7) (size 0.8) (drill 0.4) (layers \"F.Cu\" \"In1.Cu\") (net 1))\n";

std::string parity_rule(const std::string& condition, const std::string& items = "track",
                        const std::string& severity = "", const std::string& name = "parity") {
  return "(rule \"" + name + "\" (condition \"" + condition + "\") (constraint disallow " + items + ")" +
         (severity.empty() ? "" : " (severity " + severity + ")") + ")\n";
}

int parity_hits(const std::string& condition, const std::string& items = "track") {
  const Files f("parity", board_text(kParityCopper), parity_rule(condition, items));
  const auto lb = io::read_board_file(f.pcb.string());
  const auto rules = io::read_design_rules(f.pcb.string());
  const drc::RuleEngine engine(lb.board, rules);
  const auto copper = drc::build_copper(lb.board);
  int hits = 0;
  for (const auto& it : copper.items) {
    bool hit = false;
    for (int layer = 0; layer < lb.board.copper_count(); ++layer)
      if (it.layers & model::layer_bit(layer)) hit = hit || engine.disallowed(it, layer).has_value();
    hits += hit;
  }
  return hits;
}

// A new 0.25 mm F.Cu track end or 0.6 mm through via of SIG at `p`, as the router asks about it.
drc::CopperItem probe(drc::ItemKind kind, geom::Point p) {
  drc::CopperItem it;
  it.kind = kind;
  it.net = 1;
  it.width = kind == drc::ItemKind::Via ? 600'000 : 250'000;
  it.layers = kind == drc::ItemKind::Via ? model::LayerMask{0xF} : model::layer_bit(0);
  it.shapes = {geom::Shape::point(p, it.width / 2)};
  it.box = it.shapes[0].box;
  it.pos = p;
  return it;
}
}  // namespace

TEST_CASE("router rule probes: bound selectors and box prefilters agree with the direct geometry tests", "[rules][route]") {
  // A concave F.Cu rule area and a front courtyard on a flipped and an unflipped footprint, probed on a grid.
  const std::string extra =
      "  (zone (net 0) (net_name \"\") (layers \"F.Cu\") (name \"noroute\") (hatch edge 0.5) (connect_pads (clearance 0))\n"
      "    (min_thickness 0.25) (keepout (tracks allowed) (vias allowed) (pads allowed) (copperpour allowed) (footprints allowed))\n"
      "    (fill (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon (pts (xy 7 1) (xy 11 1) (xy 11 3) (xy 9 3) (xy 9 6) (xy 7 6))))\n"
      "  (footprint \"U\" (layer \"F.Cu\") (at 12 7)\n    (property \"Reference\" \"U1\" (at 0 0) (layer \"F.SilkS\"))\n"
      "    (fp_rect (start -1.5 -1.5) (end 1.5 1.5) (layer \"F.CrtYd\") (stroke (width 0.05) (type solid)))\n  )\n"
      "  (footprint \"U\" (layer \"B.Cu\") (at 12 2)\n    (property \"Reference\" \"U2\" (at 0 0) (layer \"B.SilkS\"))\n"
      "    (fp_rect (start -1.5 -1.5) (end 1.5 1.5) (layer \"B.CrtYd\") (stroke (width 0.05) (type solid)))\n  )\n";
  const Files f("prefilter", board_text(extra), "");
  const auto lb = io::read_board_file(f.pcb.string());
  const auto base = io::read_design_rules(f.pcb.string());
  const auto courtyards = drc::build_courtyards(lb.board);
  const model::Zone* area = nullptr;
  for (const auto& z : lb.board.zones)
    if (z.name == "noroute") area = &z;
  REQUIRE(area);
  struct Case {
    const char* condition;
    std::function<bool(const drc::CopperItem&)> reference;
  };
  const std::vector<Case> cases = {
      {"A.intersectsArea('nor*')", [&](const drc::CopperItem& it) { return drc::area_matches(it, *area, false); }},
      {"A.enclosedByArea('noroute')", [&](const drc::CopperItem& it) { return drc::area_matches(it, *area, true); }},
      {"A.intersectsFrontCourtyard('U?')", [&](const drc::CopperItem& it) { return drc::courtyard_matches(it, courtyards, "U?", "intersectsFrontCourtyard"); }},
  };
  for (const auto& c : cases) {
    auto rules = base;
    rules.custom = {model::CustomRule{"probe", c.condition, "", "", {model::Constraint{"disallow", std::nullopt, std::nullopt, std::nullopt, {"track", "via"}}}}};
    const drc::RuleEngine engine(lb.board, rules);
    REQUIRE(engine.positional_disallow());
    int hits = 0, n = 0;
    for (Coord x = 5'000'000; x <= 15'000'000; x += 370'000)
      for (Coord y = 0; y <= 10'000'000; y += 370'000)
        for (const auto kind : {drc::ItemKind::Track, drc::ItemKind::Via}) {
          const auto it = probe(kind, {x, y});
          const bool expected = c.reference(it);
          CHECK(engine.probe_disallowed(it) == expected);
          hits += expected;
          ++n;
        }
    CHECK(hits > 0);
    CHECK(hits < n);
  }
}

TEST_CASE("KiCad rule numbers retain units, fractional nanometres and comparison precision", "[rules][drc]") {
  // KiCad 10.0.3: width_gt_mm, width_eq_mil, literal_mm_mil and fractional_nm_*.
  CHECK(parity_hits("A.Width > 0.3mm") == 1);
  CHECK(parity_hits("A.Width >= 0.25mm") == 2);
  CHECK(parity_hits("A.Width < 0.25mm") == 1);
  CHECK(parity_hits("A.Width <= 0.25mm") == 2);
  CHECK(parity_hits("A.Width == 9.84251968503937mil") == 1);
  CHECK(parity_hits("1mm == 39.37007874015748mil") == 3);
  CHECK(parity_hits("0.0000001mm > 0mm") == 3);
  CHECK(parity_hits("0.0000001mm != 0mm") == 3);
  CHECK(parity_hits("A.Width == 0.0000001mm") == 0);
  CHECK(parity_hits("A.Width > 0.01in") == 1);
  CHECK(parity_hits("A.Width == 0.25 mm") == 1);
  CHECK(parity_hits("A.Width == 250000deg") == 1);
  CHECK(parity_hits("A.Width == 250fs") == 1);
  CHECK(parity_hits("A.Width == 0.25ps") == 1);
  CHECK(parity_hits("1ps == 1000fs") == 3);
  CHECK(parity_hits("A.Width == 0.25MM || A.NetName == 'SIG'") == 0);
  CHECK(parity_hits("A.Width > 250um || A.NetName == 'SIG'") == 0);
}

TEST_CASE("KiCad rule compilation drops lone unitless literals and unknown symbols structurally", "[rules][drc]") {
  // KiCad 10.0.3: width unitless OR probes and unknown_short_circuit drop the whole rule.
  for (const auto* condition : {"A.Width == 0.25", "A.Width != 0.25", "A.Width > 0.25",
                               "A.Width > 0.25 || A.NetName == 'SIG'",
                               "A.NetName == 'SIG' || A.Foo == 1mm",
                               "A.NetName == 'SIG' || A.notAFunction()",
                               "A.Parent.Reference == 'R1'", "Parent.Reference == 'R1'"}) {
    CAPTURE(condition);
    CHECK(parity_hits(condition, "track pad via") == 0);
    const Files f("drop", board_text(kParityCopper), parity_rule(condition));
    const auto lb = io::read_board_file(f.pcb.string());
    const auto rules = io::read_design_rules(f.pcb.string());
    const drc::RuleEngine engine(lb.board, rules);
    REQUIRE_FALSE(engine.warnings().empty());
    CHECK(engine.warnings().front().find("rule 'parity'") != std::string::npos);
    CHECK(engine.track_allowed(1, 0));
  }
  CHECK(parity_hits("L == 'F.Cu'") == 0);
  CHECK(parity_hits("A.NetName == 'SIG") == 2);
}

TEST_CASE("KiCad quoted numbers stay strings, while pad dimensions and positions use nanometres", "[rules][drc]") {
  // KiCad 10.0.3: size_x_quoted and quoted Width probes are valid type mismatches, not dimensional conversion.
  CHECK(parity_hits("A.Width == '0.25mm'") == 0);
  CHECK(parity_hits("A.Width != '0.25mm'") == 3);
  CHECK(parity_hits("A.Width > '0.25mm'") == 3);
  CHECK(parity_hits("A.Size_X == 1mm", "pad") == 4);
  CHECK(parity_hits("A.Size_Y == 1mm", "pad") == 4);
  CHECK(parity_hits("A.Size_X == '1mm'", "pad") == 0);
  // KiCad 10.0.3: position_x excludes tracks; pad anchors are absolute, not footprint origins.
  CHECK(parity_hits("A.Position_X > 10mm", "track pad via") == 6);
  CHECK(parity_hits("A.Position_Y == 3mm", "track pad via") == 3);
  CHECK(parity_hits("A.Position_X != 0mm", "track pad via") == 8);
  // The router evaluates both on each new track or via: pad sizes never match one, positions match vias only.
  for (const auto* condition : {"A.Size_X == 1mm", "A.Position_X > 10mm"}) {
    const Files f("positional", board_text(kParityCopper), parity_rule(condition, "track via"));
    const auto lb = io::read_board_file(f.pcb.string());
    const auto rules = io::read_design_rules(f.pcb.string());
    const drc::RuleEngine engine(lb.board, rules);
    CHECK(engine.track_allowed(1, 0));
    CHECK(engine.via_allowed(1));
    CHECK(engine.positional_disallow());
    const bool by_position = std::string(condition).find("Position") != std::string::npos;
    CHECK(engine.probe_disallowed(probe(drc::ItemKind::Via, {12'000'000, 5'000'000})) == by_position);
    CHECK_FALSE(engine.probe_disallowed(probe(drc::ItemKind::Via, {8'000'000, 5'000'000})));
    CHECK_FALSE(engine.probe_disallowed(probe(drc::ItemKind::Track, {12'000'000, 5'000'000})));  // tracks have no position
  }
}

TEST_CASE("KiCad rule Layer is the item layer and blind and buried disallow use the span", "[rules][drc]") {
  // KiCad 10.0.3: item_layer_front/back/inequality exclude all vias, unlike existsOnLayer.
  CHECK(parity_hits("A.Layer == 'F.Cu'", "track pad via") == 6);
  CHECK(parity_hits("A.Layer != 'F.Cu'", "track pad via") == 1);
  CHECK(parity_hits("A.existsOnLayer('F.Cu')", "track pad via") == 9);
  // KiCad 10.0.3: subtype_blind_via/buried_via each select one distinct span; micro stays separate.
  CHECK(parity_hits("true", "blind_via") == 1);
  CHECK(parity_hits("true", "buried_via") == 1);
  CHECK(parity_hits("true", "micro_via") == 1);
  CHECK(parity_hits("true", "through_via") == 1);
}

TEST_CASE("KiCad disallow severity follows later typed rules without clearing other item types", "[rules][drc]") {
  // KiCad 10.0.3: later_ignore clears its tracks; earlier_ignore does not; different disallow types accumulate.
  const std::string ban = parity_rule("true", "track via", "", "ban");
  const std::string ignore = parity_rule("A.NetName == 'SIG'", "track via", "ignore", "exception");
  for (const bool later : {false, true}) {
    const Files f("priority", board_text(kParityCopper), later ? ban + ignore : ignore + ban);
    const auto lb = io::read_board_file(f.pcb.string());
    const auto rules = io::read_design_rules(f.pcb.string());
    const drc::RuleEngine engine(lb.board, rules);
    CHECK(engine.track_allowed(1, 0) == later);
    CHECK(engine.via_allowed(1) == later);
    CHECK_FALSE(engine.track_allowed(2, 3));
    const auto copper = drc::build_copper(lb.board);
    int hits = 0;
    for (const auto& it : copper.items) hits += engine.disallowed(it, 0).has_value();
    CHECK(hits == (later ? 1 : 7));
  }
  const Files f("typed_priority", board_text(kParityCopper),
                parity_rule("true") + parity_rule("true", "via", "ignore", "only vias"));
  const auto lb = io::read_board_file(f.pcb.string());
  const auto rules = io::read_design_rules(f.pcb.string());
  const drc::RuleEngine engine(lb.board, rules);
  CHECK_FALSE(engine.track_allowed(1, 0));
  CHECK(engine.via_allowed(1));
}

TEST_CASE("KiCad multilayer pad Layer and transformed pad anchors follow their footprint", "[rules][drc]") {
  // KiCad 10.0.3: item_layer_pad_* and position_anchor_* pin PTH side and absolute shifted/rotated pad centres.
  const std::string extra =
      "(footprint \"P\" (layer \"F.Cu\") (at 10 10)\n"
      " (property \"Reference\" \"P1\" (at 0 0) (layer \"F.SilkS\"))\n"
      " (pad \"1\" thru_hole rect (at 2 0) (size 2 1) (drill 0.4) (layers \"*.Cu\") (net 1 \"SIG\")))\n"
      "(footprint \"P\" (layer \"B.Cu\") (at 10 15)\n"
      " (property \"Reference\" \"P2\" (at 0 0) (layer \"B.SilkS\"))\n"
      " (pad \"1\" thru_hole rect (at 0 0) (size 2 1) (drill 0.4) (layers \"*.Cu\") (net 1 \"SIG\")))\n"
      "(footprint \"P\" (layer \"F.Cu\") (at 10 10 90)\n"
      " (property \"Reference\" \"P3\" (at 0 0) (layer \"F.SilkS\"))\n"
      " (pad \"1\" smd rect (at 2 0 90) (size 2 1) (layers \"F.Cu\") (net 1 \"SIG\")))\n";
  for (const auto& [condition, reference] : {
           std::pair{"A.Layer == 'F.Cu'", "P1"}, {"A.Layer == 'B.Cu'", "P2"},
           {"A.Position_X == 12mm", "P1"}, {"A.Position_Y == 8mm", "P3"}}) {
    const Files f("pad_anchor", board_text(extra), parity_rule(condition, "pad"));
    const auto lb = io::read_board_file(f.pcb.string());
    const auto rules = io::read_design_rules(f.pcb.string());
    const drc::RuleEngine engine(lb.board, rules);
    const auto copper = drc::build_copper(lb.board);
    bool found = false;
    for (const auto& item : copper.items)
      if (item.kind == drc::ItemKind::Pad && lb.board.footprints[static_cast<std::size_t>(item.footprint)].reference == reference) {
        found = true;
        for (int layer = 0; layer < 4; ++layer) CHECK(engine.disallowed(item, layer).has_value());
      }
    REQUIRE(found);
  }
  CHECK(parity_hits("A.Position_X <= 0mm", "track pad via") == 3);
  CHECK(parity_hits("A.Position_X == 0mm", "track pad via") == 0);
  CHECK(parity_hits("A.Width == 250000 || 1mm == 0mm") == 1);
  CHECK(parity_hits("A.Width > 0.25 || 1mm == 0mm") == 3);
}

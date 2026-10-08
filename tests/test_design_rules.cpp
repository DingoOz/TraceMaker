// SPDX-License-Identifier: GPL-3.0-or-later
// Custom-rule routing and DRC (doc 05 §27): disallow, physical hole clearance and via-only keepouts.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
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
    dir = fs::temp_directory_path() / ("tmk_design_rules_" + name);
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
  CHECK(names("by area"));
  CHECK(names("broken"));
  CHECK(names("odd property"));
  CHECK_FALSE(names("inner GND only"));  // applied by the router as a layer mask
  // None of the three changes what the router allows.
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

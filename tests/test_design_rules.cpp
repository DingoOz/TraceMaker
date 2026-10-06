// SPDX-License-Identifier: GPL-3.0-or-later
// Custom-rule routing and DRC (doc 05 §16): disallow, physical hole clearance and via-only keepouts.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>

#include "drc/drc.hpp"
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

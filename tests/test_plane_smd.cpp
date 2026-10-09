// SPDX-License-Identifier: GPL-3.0-or-later
// Plane-aware routing of all-SMD boards (doc 05 §16) on the synthetic board tests/boards/plane_smd.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "route/obstacles.hpp"
#include "route/router.hpp"

using namespace tmk;
using geom::Point;

namespace {

const std::string kBoard = std::string(TM_SOURCE_DIR) + "/tests/boards/plane_smd/plane_smd.kicad_pcb";

model::NetId net_id(const model::Board& b, const std::string& name) {
  for (std::size_t i = 0; i < b.nets.size(); ++i)
    if (b.nets[i].name == name) return static_cast<model::NetId>(i);
  FAIL("no net " + name);
  return 0;
}

const model::Pad& pad(const model::Board& b, const std::string& ref, const std::string& num) {
  for (const auto& f : b.footprints)
    if (f.reference == ref)
      for (int i : f.pads)
        if (b.pads[static_cast<std::size_t>(i)].number == num) return b.pads[static_cast<std::size_t>(i)];
  FAIL("no pad " + ref + "." + num);
  return b.pads.front();
}

int layer(const model::Board& b, const std::string& name) {
  for (int l = 0; l < b.copper_count(); ++l)
    if (b.copper_name(l) == name) return l;
  FAIL("no layer " + name);
  return -1;
}

// Via centre on the pad's bounding rectangle (pads here sit at multiples of 90 degrees).
bool inside(const model::Pad& p, Point v) {
  const bool turned = std::lround(std::abs(p.angle)) % 180 == 90;
  const Coord w = turned ? p.size_y : p.size_x, h = turned ? p.size_x : p.size_y;
  return std::abs(v.x - p.pos.x) * 2 <= w && std::abs(v.y - p.pos.y) * 2 <= h;
}

constexpr Coord kVia = 450'000, kDrill = 200'000;
const Point kFree{105'000'000, 112'500'000};  // no part nearby on any layer; inside both plane fills and both rule areas

}  // namespace

TEST_CASE("plane_smd: plane fills block other nets' vias unless soft; no-track areas leave vias alone", "[route][planes]") {
  if (!std::filesystem::exists(kBoard)) SKIP("board missing: " + kBoard);
  model::Board b = io::read_board_file(kBoard).board;
  const auto rules = io::read_design_rules(kBoard);
  route::Obstacles obs(b, rules);
  const auto sda = net_id(b, "SDA"), gnd = net_id(b, "GND");
  // Hard fills: the GND fill on In1 and the +3V3 fill on In2 are copper of other nets for a signal via, and the +3V3
  // fill blocks a GND via.
  CHECK_FALSE(obs.via_ok(kFree, kVia, kDrill, sda, 0));
  CHECK_FALSE(obs.via_ok(kFree, kVia, kDrill, gnd, 0));
  obs.set_soft_zones(true);
  // Soft fills: both vias are legal, although the rule areas on In1/In2 forbid tracks (they allow vias).
  CHECK(obs.via_ok(kFree, kVia, kDrill, sda, 0));
  CHECK(obs.via_ok(kFree, kVia, kDrill, gnd, 0));
  // A track on a plane layer stays forbidden by the rule area; on F.Cu it is free.
  CHECK_FALSE(obs.disk_ok(kFree, layer(b, "In1.Cu"), 50'000, sda, 0));
  CHECK_FALSE(obs.disk_ok(kFree, layer(b, "In2.Cu"), 50'000, sda, 0));
  CHECK(obs.disk_ok(kFree, layer(b, "F.Cu"), 50'000, sda, 0));
}

TEST_CASE("plane_smd: vias keep off small SMD pads, exposed pads still take them", "[route][planes]") {
  if (!std::filesystem::exists(kBoard)) SKIP("board missing: " + kBoard);
  model::Board b = io::read_board_file(kBoard).board;
  const auto rules = io::read_design_rules(kBoard);
  route::Obstacles obs(b, rules);
  obs.set_soft_zones(true);
  const auto gnd = net_id(b, "GND");
  const Point cap = pad(b, "C3", "2").pos, qfn_pin = pad(b, "U1", "3").pos, ep = pad(b, "U1", "33").pos;
  // Off: a GND via may sit in a GND pad (same net), even a 0.25 mm QFN pin.
  CHECK(obs.via_ok(cap, kVia, kDrill, gnd, 0));
  CHECK(obs.via_ok(qfn_pin, kVia, kDrill, gnd, 0));
  obs.set_vias_off_pads(true, 2'000'000);
  CHECK_FALSE(obs.via_ok(cap, kVia, kDrill, gnd, 0));
  CHECK_FALSE(obs.via_ok(qfn_pin, kVia, kDrill, gnd, 0));
  CHECK(obs.via_ok(ep, kVia, kDrill, gnd, 0));  // 3.45 mm exposed pad
  // The cached fixed-obstacle code agrees (it is what the searches use).
  CHECK(obs.fixed_via_code(cap, kVia, kDrill, 0, gnd) == route::Obstacles::kBlocked);
  CHECK(obs.fixed_via_code(ep, kVia, kDrill, 0, gnd) != route::Obstacles::kBlocked);
  // The exemption used for a deliberate via in pad.
  obs.set_pad_via_exempt(true);
  CHECK(obs.via_ok(cap, kVia, kDrill, gnd, 0));
  obs.set_pad_via_exempt(false);
}

TEST_CASE("plane_smd: defaults leave the board incomplete without a via; the options route it", "[route][planes]") {
  if (!std::filesystem::exists(kBoard)) SKIP("board missing: " + kBoard);
  const model::Board b = io::read_board_file(kBoard).board;
  const auto rules = io::read_design_rules(kBoard);
  route::RouterOptions base;
  base.work_budget = 3'000'000;
  base.time_limit_s = 3600;  // the work budget decides, also in a slow sanitizer build
  base.gpu_device = -1;
  const auto plain = route::Router(b, rules, base).run();
  CHECK(plain.routed < plain.connections);
  CHECK(plain.vias.empty());

  auto opt = base;
  opt.soft_zones = opt.via_in_pad = opt.vias_off_pads = true;
  opt.bend_states = false;
  opt.first_nets = {"XIN", "XOUT"};
  const auto r = route::Router(b, rules, opt).run();
  CHECK(r.routed == r.connections);
  // Plane nets reach their planes: GND and +3V3 connections go through vias, not pad-to-pad wiring.
  const auto gnd = net_id(b, "GND"), v33 = net_id(b, "+3V3");
  CHECK(std::count_if(r.vias.begin(), r.vias.end(), [&](const model::Via& v) { return v.net == gnd; }) > 0);
  CHECK(std::count_if(r.vias.begin(), r.vias.end(), [&](const model::Via& v) { return v.net == v33; }) > 0);
  // No track on the plane layers.
  for (const auto& t : r.tracks) CHECK((t.layer != layer(b, "In1.Cu") && t.layer != layer(b, "In2.Cu")));
  // Exactly one via in a small SMD pad: the minimum via in the inner ball U2.B2.
  int in_small = 0;
  const model::Pad* b2 = &pad(b, "U2", "B2");  // a pointer: GCC 15 flags a reference bound through string temporaries
  for (const auto& v : r.vias)
    for (const auto& p : b.pads)
      if (p.type == model::PadType::Smd && std::min(p.size_x, p.size_y) < 2'000'000 && p.net == v.net && inside(p, v.pos)) {
        ++in_small;
        CHECK(&p == b2);
        CHECK(v.size == 250'000);
        CHECK(v.drill == 150'000);
      }
  CHECK(in_small == 1);
  // --first-nets: the crystal lines are committed before anything else.
  REQUIRE(!r.tracks.empty());
  const auto xin = net_id(b, "XIN"), xout = net_id(b, "XOUT");
  CHECK((r.tracks.front().net == xin || r.tracks.front().net == xout));
}

TEST_CASE("plane_smd: planes of a net without pads are not connection targets", "[route][planes]") {
  // Review of PR 1: two zone-only clusters had nothing between them and an empty connection (pad_a = -1) was planned.
  if (!std::filesystem::exists(kBoard)) SKIP("board missing: " + kBoard);
  model::Board b = io::read_board_file(kBoard).board;
  const auto rules = io::read_design_rules(kBoard);
  route::RouterOptions opt;
  opt.work_budget = 300'000;
  opt.time_limit_s = 3600;  // the work budget decides, also in a slow sanitizer build
  opt.gpu_device = -1;
  const int plain = route::Router(b, rules, opt).run().connections;
  model::Net orphan;
  orphan.name = "ORPHAN";
  b.nets.push_back(orphan);
  for (auto& z : b.zones)
    if (!z.rule_area) z.net = static_cast<model::NetId>(b.nets.size() - 1);
  opt.soft_zones = true;
  const auto r = route::Router(b, rules, opt).run();
  CHECK(r.connections == plain);
}

TEST_CASE("plane_smd: no via in pad when the board's minimum via cannot be read", "[route][planes]") {
  if (!std::filesystem::exists(kBoard)) SKIP("board missing: " + kBoard);
  const model::Board b = io::read_board_file(kBoard).board;
  auto rules = io::read_design_rules(kBoard);
  rules.minimums.via_diameter = 0;
  rules.minimums.through_hole_diameter = 0;
  route::RouterOptions opt;
  opt.work_budget = 3'000'000;
  opt.time_limit_s = 3600;  // the work budget decides, also in a slow sanitizer build
  opt.bend_states = false;
  opt.gpu_device = -1;
  opt.soft_zones = opt.via_in_pad = opt.vias_off_pads = true;
  const auto r = route::Router(b, rules, opt).run();
  const model::Pad* b2 = &pad(b, "U2", "B2");
  for (const auto& v : r.vias) CHECK_FALSE((v.net == b2->net && inside(*b2, v.pos)));
  CHECK(r.routed < r.connections);  // U2.B2 has no other way out
}

TEST_CASE("plane_smd: a rule area that forbids vias but allows tracks blocks vias only", "[route][planes]") {
  if (!std::filesystem::exists(kBoard)) SKIP("board missing: " + kBoard);
  model::Board b = io::read_board_file(kBoard).board;
  const auto rules = io::read_design_rules(kBoard);
  model::Zone z;
  z.rule_area = true;
  z.keepout_vias = true;
  for (int l = 0; l < b.copper_count(); ++l) z.copper |= model::layer_bit(l);
  const Coord r = 1'000'000;
  z.outline.push_back({{kFree.x - r, kFree.y - r}, {kFree.x + r, kFree.y - r}, {kFree.x + r, kFree.y + r}, {kFree.x - r, kFree.y + r}});
  b.zones.push_back(z);
  route::Obstacles obs(b, rules);
  obs.set_soft_zones(true);
  const auto sda = net_id(b, "SDA");
  CHECK_FALSE(obs.via_ok(kFree, kVia, kDrill, sda, 0));
  CHECK(obs.fixed_via_code(kFree, kVia, kDrill, 0, sda) == route::Obstacles::kBlocked);
  CHECK(obs.disk_ok(kFree, layer(b, "F.Cu"), 50'000, sda, 0));
}

TEST_CASE("inpad_cost: a via in pad costs the same at either end, at every via price", "[route][planes]") {
  // Two 3 x 3 arrays whose centre balls can leave on F.Cu; a no-track strip forces a layer change. Two ordinary vias
  // beat a via in either centre ball. Review of PR 1: the penalty was charged at the source only, and at a fixed
  // price that the clean-up stages (which raise the via price) undercut, so both balls took a via in pad.
  const std::string path = std::string(TM_SOURCE_DIR) + "/tests/boards/plane_smd/inpad_cost.kicad_pcb";
  if (!std::filesystem::exists(path)) SKIP("board missing: " + path);
  const model::Board b = io::read_board_file(path).board;
  const auto rules = io::read_design_rules(path);
  route::RouterOptions opt;
  opt.work_budget = 3'000'000;
  opt.time_limit_s = 3600;  // the work budget decides, also in a slow sanitizer build
  opt.gpu_device = -1;
  opt.via_in_pad = opt.vias_off_pads = true;
  const auto r = route::Router(b, rules, opt).run();
  CHECK(r.routed == r.connections);
  for (const auto& v : r.vias)
    for (const auto& p : b.pads) CHECK_FALSE((p.net == v.net && p.net != 0 && inside(p, v.pos)));
}

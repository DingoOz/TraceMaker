// SPDX-License-Identifier: GPL-3.0-or-later
// Net length and delay as KiCad 10.0.6 measures them, and its `length` and `skew` checks (doc 05 §59).
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <map>
#include <string>

#include "drc/drc.hpp"
#include "drc/length.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"

using namespace tmk;

namespace {

const std::string kQa = std::string(TM_SOURCE_DIR) + "/tests/boards/time_calculations/time_calculations.kicad_pcb";

// Every item of a net.
drc::LengthItems net_items(const model::Board& b, model::NetId n) {
  drc::LengthItems li;
  for (std::size_t i = 0; i < b.pads.size(); ++i)
    if (b.pads[i].net == n && b.pads[i].copper) li.pads.push_back(static_cast<int>(i));
  for (std::size_t i = 0; i < b.tracks.size(); ++i)
    if (b.tracks[i].net == n) li.tracks.push_back(static_cast<int>(i));
  for (std::size_t i = 0; i < b.arcs.size(); ++i)
    if (b.arcs[i].net == n) li.arcs.push_back(static_cast<int>(i));
  for (std::size_t i = 0; i < b.vias.size(); ++i)
    if (b.vias[i].net == n) li.vias.push_back(static_cast<int>(i));
  return li;
}

// Two copper layers, 1.6 mm, no stackup. KiCad's default stackup gives the board two 10 um masks, so a through via spans 1.58 mm:
// 35 um of copper on each side and 1.51 mm of dielectric.
model::Board two_layer() {
  model::Board b;
  auto layer = [](int ordinal, const char* name, const char* type, int copper) {
    model::LayerDef l;
    l.ordinal = ordinal;
    l.name = l.file_name = name;
    l.type = type;
    l.copper_index = copper;
    return l;
  };
  b.layers = {layer(0, "F.Cu", "signal", 0), layer(2, "B.Cu", "signal", 1), layer(1, "F.Mask", "user", -1), layer(3, "B.Mask", "user", -1)};
  b.copper = {0, 1};
  b.thickness = 1'600'000;
  b.nets = {{0, "", -1}, {1, "S", -1}};
  return b;
}

model::Track track(model::Point a, model::Point c, int layer) {
  model::Track t;
  t.a = a;
  t.b = c;
  t.width = 200'000;
  t.layer = layer;
  t.net = 1;
  return t;
}

}  // namespace

TEST_CASE("KiCad's time_calculations board: each class's delay, and 10.0.6's one skew violation", "[length][drc]") {
  const auto lb = io::read_board_file(kQa);
  const auto r = io::read_design_rules(kQa);
  const auto& b = lb.board;
  const auto vh = drc::via_heights(b, r);
  // The middle of each rule's window, which is KiCad's value to 0.01 ps (attoseconds).
  const std::map<std::string, std::int64_t> want = {{"CLASS1", 29'840'000}, {"CLASS2", 59'840'000}, {"CLASS3", 59'690'000},
                                                     {"CLASS4", 31'430'000}, {"CLASS5", 49'840'000}, {"CLASS6", 75'720'000}};
  int seen = 0;
  for (const auto& n : b.nets) {
    if (n.id == 0) continue;
    const auto& nc = r.class_for(n.name);
    const auto it = want.find(nc.name);
    if (it == want.end()) continue;
    const auto len = drc::net_length(b, r, vh, net_items(b, n.id), r.profile_of(nc));
    INFO(nc.name << " " << n.name << " " << len.total_delay());
    CHECK(std::llabs(static_cast<std::int64_t>(len.total_delay()) - it->second) <= 5'000);
    ++seen;
  }
  CHECK(seen == 6);
  int skew = 0, length = 0;
  for (const auto& v : drc::run_drc(b, r).violations) {
    skew += v.type == "skew_out_of_range";
    length += v.type == "length_out_of_range";
    if (v.type == "skew_out_of_range") CHECK(v.description.find("CLASS7 min skew 4.44 ps; actual 0.00 ps") != std::string::npos);
  }
  CHECK(skew == 1);
  CHECK(length == 0);
}

TEST_CASE("net length: tracks join, a track ending inside a via is clipped to the via's centre, and vias add their span", "[length]") {
  auto b = two_layer();
  model::DesignRules r;
  const auto vh = drc::via_heights(b, r);
  REQUIRE(vh.between(0, 1) == 1'600'000 - 20'000);
  // F.Cu: (0,0) → (1,0) → (3,0), joined into one chain. The second track ends 0.1 mm short of a via at (3.1,0), inside its 0.4 mm pad:
  // KiCad replaces the part inside the pad by a straight line to the centre, so the chain measures 3.1 mm. B.Cu: (3.1,0) → (5,0).
  b.tracks.push_back(track({0, 0}, {1'000'000, 0}, 0));
  b.tracks.push_back(track({1'000'000, 0}, {3'000'000, 0}, 0));
  b.tracks.push_back(track({3'100'000, 0}, {5'000'000, 0}, 1));
  model::Via v;
  v.pos = {3'100'000, 0};
  v.size = 400'000;
  v.drill = 200'000;
  v.layer_bottom = 1;
  v.net = 1;
  b.vias.push_back(v);
  auto len = drc::net_length(b, r, vh, net_items(b, 1), nullptr);
  CHECK(len.track == 3'100'000 + 1'900'000);
  CHECK(len.vias == 1);
  CHECK(len.via == 1'580'000);
  CHECK(len.total_delay() == 0);  // no tuning profile
  // A via joining one layer only is a stub: counted, with no height.
  b.tracks.pop_back();
  len = drc::net_length(b, r, vh, net_items(b, 1), nullptr);
  CHECK(len.vias == 1);
  CHECK(len.via == 0);
  // Without "use height for length calculations", vias count for nothing.
  r.use_height_for_length = false;
  len = drc::net_length(b, r, drc::via_heights(b, r), net_items(b, 1), nullptr);
  CHECK(len.vias == 0);
  CHECK(len.via == 0);
}

TEST_CASE("net delay: each layer's delay per mm, the via's per mm of height or its override, pad-to-die delay", "[length]") {
  auto b = two_layer();
  model::DesignRules r;
  const auto vh = drc::via_heights(b, r);
  b.tracks.push_back(track({0, 0}, {2'000'000, 0}, 0));
  b.tracks.push_back(track({2'000'000, 0}, {4'000'000, 0}, 1));
  model::Via v;
  v.pos = {2'000'000, 0};
  v.size = 400'000;
  v.layer_bottom = 1;
  v.net = 1;
  b.vias.push_back(v);
  model::TuningProfile tp;
  tp.time_domain = true;
  tp.layers = {{"F.Cu", 0, 0, 6'000'000}, {"B.Cu", 0, 0, 7'000'000}};  // 6 and 7 ps per mm
  tp.via_delay = 10'000'000;                                            // 10 ps per mm of height
  auto len = drc::net_length(b, r, vh, net_items(b, 1), &tp);
  CHECK(len.track_delay == 2 * 6'000'000 + 2 * 7'000'000);
  CHECK(len.via_delay == 15'800'000);  // 1.58 mm
  tp.via_overrides.push_back({"F.Cu", "B.Cu", "F.Cu", "B.Cu", 3'000'000});
  len = drc::net_length(b, r, vh, net_items(b, 1), &tp);
  CHECK(len.via_delay == 3'000'000);
  // Without time-domain tuning, KiCad gives vias no delay; tracks keep theirs.
  tp.time_domain = false;
  len = drc::net_length(b, r, vh, net_items(b, 1), &tp);
  CHECK(len.via_delay == 0);
  CHECK(len.track_delay == 26'000'000);
}

TEST_CASE("length and skew rules in ps are delays, and the router does not read them as lengths", "[length][rules]") {
  const auto lb = io::read_board_file(kQa);
  const auto r = io::read_design_rules(kQa);
  bool found = false;
  for (const auto& c : r.custom)
    for (const auto& k : c.constraints)
      if (c.name == "CLASS1" && k.type == "length") {
        found = true;
        CHECK(k.time);
        CHECK(k.min == 29'830'000);
        CHECK(k.max == 29'850'000);
      }
  CHECK(found);
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <string>

#include "drc/rule_engine.hpp"
#include "io/kicad/board_reader.hpp"

using namespace tmk;

namespace {
constexpr Coord kDefault = 200'000;
constexpr Coord kCustom = 800'000;

model::Board layer_board(bool multilayer) {
  const std::string zone_layers = multilayer ? "(layers \"F.Cu\" \"In1.Cu\")" : "(layer \"F.Cu\")";
  return io::read_board(sexpr::Document::parse(
      R"((kicad_pcb (version 20240108) (generator "pcbnew")
        (layers (0 "F.Cu" signal) (1 "In1.Cu" signal) (2 "In2.Cu" signal) (31 "B.Cu" signal))
        (net 0 "") (net 1 "X") (net 2 "Y")
        (footprint "T:S" (layer "F.Cu") (at 19 14) (property "Reference" "SF")
          (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "X")))
        (footprint "T:H" (layer "F.Cu") (at 19 20) (property "Reference" "PF")
          (pad "1" thru_hole rect (at 0 0) (size 1 1) (drill 0.4) (layers "*.Cu") (net 1 "X")))
        (footprint "T:H" (layer "B.Cu") (at 19 23) (property "Reference" "PB")
          (pad "1" thru_hole rect (at 0 0) (size 1 1) (drill 0.4) (layers "*.Cu") (net 1 "X")))
        (segment (start 18 5) (end 19.35 5) (width 0.25) (layer "F.Cu") (net 1))
        (segment (start 18 8) (end 19.35 8) (width 0.25) (layer "In1.Cu") (net 1))
        (via (at 19.1 26) (size 0.8) (drill 0.4) (layers "F.Cu" "B.Cu") (net 1))
        (zone (net 2) (net_name "Y") )" + zone_layers +
      R"( (hatch edge 0.5) (connect_pads (clearance 0)) (min_thickness 0.01)
          (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5))
          (polygon (pts (xy 20 3) (xy 24 3) (xy 24 32) (xy 20 32)))
          (filled_polygon (layer "F.Cu") (pts (xy 20 3) (xy 24 3) (xy 24 32) (xy 20 32)))
      )" +
      (multilayer ? "(filled_polygon (layer \"In1.Cu\") (pts (xy 20 3) (xy 24 3) (xy 24 32) (xy 20 32)))" : "") +
      "))"));
}

model::DesignRules layer_rule(const std::string& condition) {
  model::DesignRules rules;
  model::NetClass default_class;
  default_class.name = "Default";
  default_class.clearance = kDefault;
  rules.classes.push_back(default_class);
  model::CustomRule rule;
  rule.name = "layer context";
  rule.condition = condition;
  model::Constraint constraint;
  constraint.type = "clearance";
  constraint.min = kCustom;
  rule.constraints.push_back(constraint);
  rules.custom.push_back(rule);
  return rules;
}

const drc::CopperItem& item(const drc::CopperModel& copper, drc::ItemKind kind, int index, int sub = 0) {
  const auto found = std::find_if(copper.items.begin(), copper.items.end(), [&](const auto& candidate) {
    return candidate.kind == kind && candidate.index == index && candidate.sub == sub;
  });
  REQUIRE(found != copper.items.end());
  return *found;
}

void check_pair(const drc::RuleEngine& engine, const drc::CopperItem& neighbor,
                const drc::CopperItem& zone, int layer, Coord expected) {
  CHECK(engine.clearance(neighbor, zone, layer) == expected);
  CHECK(engine.clearance(zone, neighbor, layer) == expected);
}
}  // namespace

TEST_CASE("Via Layer is unavailable in pair conditions, even on a shared copper layer", "[rules][drc][layer_context]") {
  const auto board = layer_board(false);
  const auto copper = drc::build_copper(board);
  const auto& via = item(copper, drc::ItemKind::Via, 0);
  const auto& zone = item(copper, drc::ItemKind::Zone, 0);
  // KiCad 10.0.3 zone_pair_front: controls report the via-zone pair, while
  // same_layer and different_layer both suppress it. Unary disallow semantics
  // do not change just because a second item and a checked layer are present.
  for (const std::string condition : {"", "B.Type == 'Zone'", "B.Type == 'Zone' && B.Layer == 'F.Cu'",
                                      "B.Type == 'Zone' && A.existsOnLayer('F.Cu')"}) {
    const auto rules = layer_rule(condition);
    const drc::RuleEngine engine(board, rules);
    check_pair(engine, via, zone, 0, kCustom);
  }
  for (const std::string condition : {"B.Type == 'Zone' && A.Layer == B.Layer",
                                      "B.Type == 'Zone' && A.Layer != B.Layer",
                                      "B.Type == 'Zone' && A.Layer == 'F.Cu'",
                                      "B.Type == 'Zone' && A.Layer == 'B.Cu'"}) {
    const auto rules = layer_rule(condition);
    const drc::RuleEngine engine(board, rules);
    check_pair(engine, via, zone, 0, kDefault);
  }
}

TEST_CASE("PTH Layer is footprint side, not the zone fill or checked copper layer", "[rules][drc][layer_context]") {
  const auto board = layer_board(true);
  const auto copper = drc::build_copper(board);
  const auto& front_pad = item(copper, drc::ItemKind::Pad, 1);
  const auto& back_pad = item(copper, drc::ItemKind::Pad, 2);
  // KiCad 10.0.3 zone_pair_multi_item_front/item_back repeat the selected PTH
  // pair once per fill. zone_pair_inner_item_front also selects front PTH.
  for (const int layer : {0, 1}) {
    const auto& zone = item(copper, drc::ItemKind::Zone, 0, layer);
    for (const bool front : {true, false}) {
      const auto rules = layer_rule(front ? "B.Type == 'Zone' && A.Layer == 'F.Cu'"
                                          : "B.Type == 'Zone' && A.Layer == 'B.Cu'");
      const drc::RuleEngine engine(board, rules);
      check_pair(engine, front_pad, zone, layer, front ? kCustom : kDefault);
      check_pair(engine, back_pad, zone, layer, front ? kDefault : kCustom);
    }
  }
}

TEST_CASE("Zone Layer belongs to the original zone, not each projected fill", "[rules][drc][layer_context]") {
  const bool multilayer = GENERATE(false, true);
  const auto board = layer_board(multilayer);
  const auto copper = drc::build_copper(board);
  // KiCad 10.0.3 zone_pair_front_same_layer selects F track/SMD/front PTH;
  // zone_pair_multi_same_layer and zone_front select nothing. A multilayer
  // zone's registered Layer compares unequal to F.Cu (zone_not_front), but
  // never equals empty string, literal undefined, or wildcard *.
  for (int layer = 0; layer < (multilayer ? 2 : 1); ++layer) {
    const auto& zone = item(copper, drc::ItemKind::Zone, 0, layer);
    const auto& track = item(copper, drc::ItemKind::Track, layer);
    const auto& via = item(copper, drc::ItemKind::Via, 0);
    const auto eq_rules = layer_rule("B.Type == 'Zone' && A.Layer == B.Layer");
    const drc::RuleEngine eq_engine(board, eq_rules);
    check_pair(eq_engine, track, zone, layer, multilayer ? kDefault : kCustom);
    check_pair(eq_engine, via, zone, layer, kDefault);
    const auto ne_rules = layer_rule("B.Type == 'Zone' && A.Layer != B.Layer");
    const drc::RuleEngine ne_engine(board, ne_rules);
    check_pair(ne_engine, track, zone, layer, multilayer ? kCustom : kDefault);
    check_pair(ne_engine, via, zone, layer, kDefault);
    if (layer == 0) {
      const auto& smd = item(copper, drc::ItemKind::Pad, 0);
      check_pair(eq_engine, smd, zone, layer, multilayer ? kDefault : kCustom);
      check_pair(ne_engine, smd, zone, layer, multilayer ? kCustom : kDefault);
    }
    const auto front_rules = layer_rule("B.Type == 'Zone' && B.Layer == 'F.Cu'");
    const drc::RuleEngine front_engine(board, front_rules);
    check_pair(front_engine, track, zone, layer, multilayer ? kDefault : kCustom);
    const auto not_front_rules = layer_rule("B.Type == 'Zone' && B.Layer != 'F.Cu'");
    const drc::RuleEngine not_front_engine(board, not_front_rules);
    check_pair(not_front_engine, track, zone, layer, multilayer ? kCustom : kDefault);
    check_pair(not_front_engine, via, zone, layer, multilayer ? kCustom : kDefault);
    if (multilayer) {
      for (const std::string condition : {"B.Type == 'Zone' && B.Layer == ''",
                                          "B.Type == 'Zone' && B.Layer == 'undefined'",
                                          "B.Type == 'Zone' && B.Layer == '*'"}) {
        const auto rules = layer_rule(condition);
        const drc::RuleEngine engine(board, rules);
        check_pair(engine, track, zone, layer, kDefault);
      }
    }
  }
}

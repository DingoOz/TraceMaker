// SPDX-License-Identifier: GPL-3.0-or-later
// Buses (doc 05 §38): pattern syntax, member order, detection in the net names, the skew limit from custom rules,
// and the measurement of a routed group.
#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

#include "drc/rule_engine.hpp"
#include "route/bus.hpp"

using namespace tmk;
using geom::Point;

namespace {

// A board with `parts` components and one pad per (net, component) pair listed in `pads`.
struct Builder {
  model::Board b;
  Builder() { b.nets.push_back({}); }
  model::NetId net(const std::string& name) {
    for (const auto& n : b.nets)
      if (n.name == name && n.id != 0) return n.id;
    model::Net n;
    n.id = static_cast<model::NetId>(b.nets.size());
    n.name = name;
    b.nets.push_back(n);
    return n.id;
  }
  int part(const std::string& ref) {
    for (std::size_t i = 0; i < b.footprints.size(); ++i)
      if (b.footprints[i].reference == ref) return static_cast<int>(i);
    model::Footprint f;
    f.reference = ref;
    b.footprints.push_back(f);
    return static_cast<int>(b.footprints.size() - 1);
  }
  void pad(const std::string& net_name, const std::string& ref) {
    model::Pad p;
    p.footprint = part(ref);
    p.net = net(net_name);
    p.copper = model::layer_bit(0);
    p.number = std::to_string(b.pads.size() + 1);
    p.pos = Point{static_cast<Coord>(b.pads.size()) * 1'000'000, 0};
    p.size_x = p.size_y = 500'000;
    b.footprints[static_cast<std::size_t>(p.footprint)].pads.push_back(static_cast<int>(b.pads.size()));
    b.pads.push_back(p);
  }
  void link(const std::string& net_name, std::initializer_list<const char*> refs) {
    for (const char* r : refs) pad(net_name, r);
  }
};

std::vector<std::string> names(const model::Board& b, const route::BusGroup& g) {
  std::vector<std::string> out;
  for (const auto n : g.members) out.push_back(b.nets[static_cast<std::size_t>(n)].name);
  return out;
}

model::Track track(model::NetId net, Coord x0, Coord x1, int layer) {
  model::Track t;
  t.a = Point{x0, 0};
  t.b = Point{x1, 0};
  t.width = 200'000;
  t.layer = layer;
  t.net = net;
  return t;
}

model::Via via(model::NetId net) {
  model::Via v;
  v.net = net;
  v.size = 600'000;
  v.drill = 300'000;
  v.layer_bottom = 1;
  return v;
}

}  // namespace

TEST_CASE("bus: natural order of net names", "[bus]") {
  CHECK(route::natural_less("D2", "D10"));
  CHECK_FALSE(route::natural_less("D10", "D2"));
  CHECK(route::natural_less("/MEM/DQ9", "/MEM/DQ10"));
  CHECK(route::natural_less("A[3]", "A[12]"));
  CHECK(route::natural_less("D07", "D8"));
  CHECK(route::natural_less("D", "D0"));
  CHECK(route::natural_less("A1", "B0"));
  CHECK_FALSE(route::natural_less("D5", "D5"));
  // Equal up to leading zeros: still a strict order, so a sort is stable across runs.
  CHECK(route::natural_less("D05", "D5") != route::natural_less("D5", "D05"));
}

TEST_CASE("bus: pattern syntax", "[bus]") {
  using route::bus_pattern_match;
  // An index range is a number in the range, not a character class.
  CHECK(bus_pattern_match("/D[0-15]", "/D0"));
  CHECK(bus_pattern_match("/D[0-15]", "/D15"));
  CHECK(bus_pattern_match("/D[0-15]", "/D9"));
  CHECK_FALSE(bus_pattern_match("/D[0-15]", "/D16"));
  CHECK_FALSE(bus_pattern_match("/D[0-15]", "/D"));
  CHECK_FALSE(bus_pattern_match("/D[0-15]", "/D1x"));
  CHECK_FALSE(bus_pattern_match("/D[0-15]", "D1"));  // the whole name, sheet path included
  CHECK(bus_pattern_match("/D[8..15]", "/D12"));
  CHECK_FALSE(bus_pattern_match("/D[8..15]", "/D7"));
  CHECK(bus_pattern_match("/D[0-7]", "/D07"));       // zero-padded indices are numbers too
  // Wildcards beside a range; everything else is literal there.
  CHECK(bus_pattern_match("*DQ[0-7]_N", "/MEM/DQ3_N"));
  CHECK(bus_pattern_match("/LCD_?[0-7]", "/LCD_R5"));
  CHECK_FALSE(bus_pattern_match("/LCD_?[0-7]", "/LCD_5"));
  CHECK_FALSE(bus_pattern_match("/A.[0-3]", "/Ax2"));  // '.' is a dot, not "any character"
  // Nets named with brackets: the range sits in literal brackets.
  CHECK(bus_pattern_match("A[[0-15]]", "A[12]"));
  CHECK_FALSE(bus_pattern_match("A[[0-15]]", "A12"));
  // Without a range: KiCad's net-class matching (wildcards, or a full regular expression).
  CHECK(bus_pattern_match("/LCD_*", "/LCD_HSYNC"));
  CHECK_FALSE(bus_pattern_match("/LCD_*", "/SPI_SCK"));
  CHECK(bus_pattern_match("/SPI_(SCK|MOSI)", "/SPI_MOSI"));
  CHECK_FALSE(bus_pattern_match("/SPI_(SCK|MOSI)", "/SPI_MISO"));
  // A reversed range is no range (and no valid character class either): nothing matches.
  CHECK_FALSE(bus_pattern_match("D[9-7]", "D8"));
}

TEST_CASE("bus: a group from --bus NAME=PATTERN", "[bus]") {
  Builder s;
  for (const char* n : {"/D10", "/D2", "/D0", "/D1", "/D11", "/D3", "/CLK"}) s.link(n, {"U1", "U2"});
  s.net("/D4");  // matches, but has no pad: not a member
  const auto g = route::bus_from_option(s.b, "DATA=/D[0-11]");
  CHECK(g.name == "DATA");
  CHECK(g.source == "option");
  CHECK(g.skew_limit == 0);
  CHECK(names(s.b, g) == std::vector<std::string>{"/D0", "/D1", "/D2", "/D3", "/D10", "/D11"});
  CHECK_THROWS_AS(route::bus_from_option(s.b, "/D[0-7]"), std::runtime_error);    // no name
  CHECK_THROWS_AS(route::bus_from_option(s.b, "DATA="), std::runtime_error);       // no pattern
  CHECK_THROWS_AS(route::bus_from_option(s.b, "X=/CLK"), std::runtime_error);      // one net is no bus
  CHECK_THROWS_AS(route::bus_from_option(s.b, "X=/NOPE*"), std::runtime_error);
}

TEST_CASE("bus: detection in the net names", "[bus]") {
  Builder s;
  for (int i = 0; i < 8; ++i) s.link(("/D" + std::to_string(i)).c_str(), {"U1", "U2"});
  for (int i = 0; i < 4; ++i) s.link(("A[" + std::to_string(i) + "]").c_str(), {"U1", "U3"});
  // Three nets with an index are not enough; nor are four that do not share two components.
  for (int i = 0; i < 3; ++i) s.link(("/IRQ" + std::to_string(i)).c_str(), {"U1", "U2"});
  s.link("/LED0", {"U1", "R1"});
  s.link("/LED1", {"U1", "R2"});
  s.link("/LED2", {"U1", "R3"});
  s.link("/LED3", {"U1", "R4"});
  // KiCad's generated names never form a bus, however regular.
  for (int i = 1; i <= 5; ++i) s.link(("Net-(U1-Pad" + std::to_string(i) + ")").c_str(), {"U1", "U2"});
  // No index at all, a bare number, and a year-like number are not indices.
  s.link("/CLK", {"U1", "U2"});
  for (const char* n : {"1", "2", "3", "4"}) s.link(n, {"U1", "U2"});
  for (const char* n : {"/V12000", "/V12001", "/V12002", "/V12003"}) s.link(n, {"U1", "U2"});

  const auto gs = route::detect_buses(s.b);
  REQUIRE(gs.size() == 2);
  CHECK(gs[0].name == "/D");
  CHECK(gs[0].source == "detected");
  CHECK(names(s.b, gs[0]) == std::vector<std::string>{"/D0", "/D1", "/D2", "/D3", "/D4", "/D5", "/D6", "/D7"});
  CHECK(gs[1].name == "A");
  CHECK(names(s.b, gs[1]) == std::vector<std::string>{"A[0]", "A[1]", "A[2]", "A[3]"});
  CHECK(route::detect_buses(s.b, 9).empty());

  SECTION("one stem, two destinations: two groups named by their components; a net with a third pad still belongs") {
    Builder t;
    for (int i = 0; i < 4; ++i) t.link(("/DQ" + std::to_string(i)).c_str(), {"U1", "U2"});
    for (int i = 4; i < 9; ++i) t.link(("/DQ" + std::to_string(i)).c_str(), {"U1", "U3"});
    t.pad("/DQ8", "TP1");
    const auto two = route::detect_buses(t.b);
    REQUIRE(two.size() == 2);
    CHECK(two[0].name == "/DQ(U1-U2)");
    CHECK(two[0].members.size() == 4);
    CHECK(two[1].name == "/DQ(U1-U3)");
    CHECK(names(t.b, two[1]) == std::vector<std::string>{"/DQ4", "/DQ5", "/DQ6", "/DQ7", "/DQ8"});
  }
}

TEST_CASE("bus: skew limit from the board's custom rules", "[bus]") {
  Builder s;
  for (int i = 0; i < 4; ++i) s.link(("/DQ" + std::to_string(i)).c_str(), {"U1", "U2"});
  s.link("/OTHER0", {"U1", "U2"});
  model::DesignRules r;
  model::NetClass def;
  def.name = "Default";
  r.classes.push_back(def);
  model::NetClass bus = def;
  bus.name = "BUS";
  r.classes.push_back(bus);
  r.patterns.emplace_back("/DQ*", "BUS");
  model::CustomRule cr;
  cr.name = "bus_skew";
  cr.condition = "A.NetClass == 'BUS'";
  model::Constraint k;
  k.type = "skew";
  k.max = 800'000;
  cr.constraints.push_back(k);
  r.custom.push_back(cr);

  const auto g = route::bus_from_option(s.b, "DQ=/DQ[0-3]");
  {
    const drc::RuleEngine re(s.b, r);
    const auto lim = route::bus_rule_skew(re, g);
    REQUIRE(lim.has_value());
    CHECK(*lim == 800'000);
    // One member outside the class: the rule does not describe the group, so there is no limit (never invented).
    CHECK_FALSE(route::bus_rule_skew(re, route::bus_from_option(s.b, "MIX=/*[0-3]")).has_value());
  }
  {
    // A skew rule for differential pairs only is no bus limit.
    auto r2 = r;
    r2.custom.front().constraints.front().items.push_back("within_diff_pairs");
    const drc::RuleEngine re(s.b, r2);
    CHECK_FALSE(route::bus_rule_skew(re, g).has_value());
  }
  {
    auto r3 = r;
    r3.custom.clear();
    const drc::RuleEngine re(s.b, r3);
    CHECK_FALSE(route::bus_rule_skew(re, g).has_value());
  }
}

TEST_CASE("bus: measurement of a routed group", "[bus]") {
  Builder s;
  for (int i = 0; i < 5; ++i) s.link(("/D" + std::to_string(i)).c_str(), {"U1", "U2"});
  const auto g = route::bus_from_option(s.b, "DATA=/D[0-4]");
  REQUIRE(g.members.size() == 5);
  std::vector<model::Track> tracks;
  std::vector<model::Via> vias;
  // D0, D1, D3: front, via, back, 10 mm / 10.4 mm / 9.9 mm. D2: front only (left the bus). D4: not routed.
  const Coord len[] = {10'000'000, 10'400'000, 7'000'000, 9'900'000};
  for (int i = 0; i < 4; ++i) {
    const auto n = g.members[static_cast<std::size_t>(i)];
    if (i == 2) {
      tracks.push_back(track(n, 0, len[i], 0));
      continue;
    }
    tracks.push_back(track(n, 0, len[i] / 2, 0));
    tracks.push_back(track(n, len[i] / 2, len[i], 1));
    vias.push_back(via(n));
    vias.back().pos = Point{len[i] / 2, 0};  // where the two tracks meet
  }
  tracks.push_back(track(g.members[4], 0, 3'000'000, 0));  // a stub: copper, but the net is not joined
  std::vector<std::uint8_t> joined(s.b.nets.size(), 1);
  joined[static_cast<std::size_t>(g.members[4])] = 0;

  const auto st = route::measure_bus(s.b, {}, tracks, {}, vias, joined, g);
  REQUIRE(st.members.size() == 5);
  CHECK(st.routed == 4);
  CHECK(st.followed == 3);
  CHECK(st.left == 1);
  CHECK(st.ref_vias == 1);
  CHECK(st.ref_layers == (model::layer_bit(0) | model::layer_bit(1)));
  CHECK(st.members[0].followed);
  CHECK_FALSE(st.members[2].followed);
  CHECK(st.members[2].routed);
  CHECK_FALSE(st.members[4].routed);
  CHECK_FALSE(st.members[4].followed);
  CHECK(st.vias_min == 0);
  CHECK(st.vias_max == 1);
  CHECK(st.members[1].length == 10'400'000.0);
  CHECK(st.members[1].vias == 1);
  // Skew over the routed members only: 10.4 - 7.0 mm; the unrouted stub does not count.
  CHECK(st.length_min == 7'000'000.0);
  CHECK(st.length_max == 10'400'000.0);
  CHECK(st.skew() == 3'400'000.0);

  SECTION("vias count by their height, as in KiCad's skew check") {
    // No stackup block, two layers, 1.6 mm: KiCad's default stackup, 35 um copper each side of a 1.53 mm core
    // (this board lists no mask layers), both outer layers in full.
    s.b.layers.resize(2);
    s.b.layers[0].name = "F.Cu";
    s.b.layers[1].name = "B.Cu";
    s.b.copper = {0, 1};
    const auto vh = route::via_heights(s.b, model::DesignRules{});
    CHECK(vh.between(0, 1) == 1'600'000);
    CHECK(vh.between(1, 0) == 1'600'000);
    CHECK(vh.between(0, 0) == 0);
    const auto sv = route::measure_bus(s.b, vh, tracks, {}, vias, joined, g);
    CHECK(sv.members[1].via_length == 1'600'000.0);
    CHECK(sv.members[1].length == 12'000'000.0);
    CHECK(sv.members[2].via_length == 0.0);  // no via
    // The member without a via is shorter by the via's height too: 12.0 - 7.0 mm.
    CHECK(sv.skew() == 5'000'000.0);
    // A via no track of a second layer ends in adds nothing (KiCad: a stub via).
    std::vector<model::Via> stub{via(g.members[2])};
    stub[0].pos = Point{3'000'000, 5'000'000};
    CHECK(route::net_via_length(s.b, vh, g.members[2], tracks, {}, stub) == 0.0);
    // The project may turn the height off.
    model::DesignRules flat;
    flat.use_height_for_length = false;
    CHECK(route::via_heights(s.b, flat).between(0, 1) == 0);
  }
  SECTION("via heights from a stackup: outer copper in full, inner copper by half") {
    s.b.layers.resize(4);
    s.b.copper = {0, 1, 2, 3};
    s.b.stackup.present = true;
    auto layer = [&](const char* name, const char* type, Coord th, int ci) {
      model::StackupLayer l;
      l.name = name;
      l.type = type;
      l.thickness = th;
      l.copper_index = ci;
      s.b.stackup.layers.push_back(l);
    };
    layer("F.Mask", "Top Solder Mask", 10'000, -1);
    layer("F.Cu", "copper", 35'000, 0);
    layer("dielectric 1", "prepreg", 100'000, -1);
    layer("In1.Cu", "copper", 18'000, 1);
    layer("dielectric 2", "core", 1'200'000, -1);
    layer("In2.Cu", "copper", 18'000, 2);
    layer("dielectric 3", "prepreg", 100'000, -1);
    layer("B.Cu", "copper", 35'000, 3);
    layer("B.Mask", "Bottom Solder Mask", 10'000, -1);
    const auto vh = route::via_heights(s.b, model::DesignRules{});
    CHECK(vh.between(0, 1) == 35'000 + 100'000 + 9'000);
    CHECK(vh.between(1, 2) == 9'000 + 1'200'000 + 9'000);
    CHECK(vh.between(0, 3) == 35'000 + 100'000 + 18'000 + 1'200'000 + 18'000 + 100'000 + 35'000);
    CHECK(vh.between(2, 3) == 9'000 + 100'000 + 35'000);
  }
  SECTION("ties in the reference go to the first member") {
    std::vector<model::Track> t2{track(g.members[0], 0, 5'000'000, 0), track(g.members[1], 0, 5'000'000, 1)};
    std::vector<std::uint8_t> j2(s.b.nets.size(), 0);
    j2[static_cast<std::size_t>(g.members[0])] = j2[static_cast<std::size_t>(g.members[1])] = 1;
    const auto s2 = route::measure_bus(s.b, {}, t2, {}, {}, j2, g);
    CHECK(s2.routed == 2);
    CHECK(s2.ref_layers == model::layer_bit(0));
    CHECK(s2.followed == 1);
    CHECK(s2.left == 1);
    CHECK(s2.skew() == 0.0);
  }
  SECTION("nothing routed") {
    const auto s3 = route::measure_bus(s.b, {}, {}, {}, {}, std::vector<std::uint8_t>(s.b.nets.size(), 0), g);
    CHECK(s3.routed == 0);
    CHECK(s3.followed == 0);
    CHECK(s3.skew() == 0.0);
  }
}

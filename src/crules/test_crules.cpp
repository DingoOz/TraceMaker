// Unit tests for component-aware layout rules (tm::crules, design doc 15 §9.1 L0-L2).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "crules/catalogue.hpp"
#include "crules/detect.hpp"
#include "crules/engine.hpp"
#include "crules/names.hpp"
#include "crules/topology.hpp"
#include "geom/shape.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"

using namespace tmk;
using namespace tmk::crules;

namespace {

constexpr Coord MM = 1'000'000;

// A synthetic board builder: footprints with two-or-more pads on named nets, 10 x 10 mm grid positions.
struct Builder {
  model::Board b;
  Builder() {
    b.nets.push_back({0, "", -1});
    model::LayerDef f{0, "F.Cu", "F.Cu", "signal", "", 0}, bk{31, "B.Cu", "B.Cu", "signal", "", 1};
    b.layers = {f, bk};
    b.copper = {0, 1};
    // A 100 x 80 mm outline.
    for (auto [a, c] : {std::pair<Point, Point>{{0, 0}, {100 * MM, 0}}, {{100 * MM, 0}, {100 * MM, 80 * MM}}, {{100 * MM, 80 * MM}, {0, 80 * MM}}, {{0, 80 * MM}, {0, 0}}}) {
      model::Graphic g;
      g.layer = "Edge.Cuts";
      g.a = a;
      g.b = c;
      b.graphics.push_back(g);
    }
  }
  model::NetId net(const std::string& name) {
    for (std::size_t i = 0; i < b.nets.size(); ++i)
      if (b.nets[i].name == name) return static_cast<model::NetId>(i);
    b.nets.push_back({static_cast<model::NetId>(b.nets.size()), name, -1});
    b.net_index[name] = b.nets.back().id;
    return b.nets.back().id;
  }
  // pads: (number, net, pin function); positioned 1 mm apart along x from `at`.
  int add(const std::string& ref, const std::string& lib, const std::string& value, Point at,
          const std::vector<std::tuple<std::string, std::string, std::string>>& pads, bool smd = true) {
    model::Footprint fp;
    fp.reference = ref;
    fp.lib_id = lib;
    fp.value = value;
    fp.pos = at;
    const int fi = static_cast<int>(b.footprints.size());
    Coord x = 0;
    for (const auto& [num, nn, pf] : pads) {
      model::Pad p;
      p.footprint = fi;
      p.number = num;
      p.pos = Point{at.x + x, at.y};
      p.size_x = p.size_y = 600'000;
      p.copper = smd ? model::layer_bit(0) : (model::layer_bit(0) | model::layer_bit(1));
      p.type = smd ? model::PadType::Smd : model::PadType::ThruHole;
      p.net = nn.empty() ? 0 : net(nn);
      p.pinfunction = pf;
      fp.pads.push_back(static_cast<int>(b.pads.size()));
      b.pads.push_back(p);
      x += MM;
    }
    b.footprints.push_back(fp);
    return fi;
  }
};

const Instance* find(const Detection& d, const model::Board& b, const std::string& cat_id, const std::string& ref) {
  const int ci = builtin_catalogue().index_of(cat_id);
  for (const auto& in : d.instances)
    if (in.category == ci && b.footprints[static_cast<std::size_t>(in.anchor)].reference == ref) return &in;
  return nullptr;
}

std::string role_refs(const model::Board& b, const Instance& in, const std::string& role) {
  const Role* r = in.role(role);
  if (!r) return "";
  std::string s;
  for (int f : r->parts) s += (s.empty() ? "" : ",") + b.footprints[static_cast<std::size_t>(f)].reference;
  return s;
}

// MCU U1 with a crystal Y1 + load caps C1/C2, a decap C3, an LDO U2 with caps C4/C5, a USB micro-B J1 with an
// ESD array U3, a CAN transceiver U4 on connector J2.
Builder demo_board() {
  Builder B;
  B.add("U1", "Package_QFP:LQFP-48_7x7mm_P0.5mm", "STM32F103C8", {50 * MM, 40 * MM},
        {{"1", "+3V3", "VDD"}, {"2", "GND", "VSS"}, {"5", "/OSC_IN", "PD0-OSC_IN"}, {"6", "/OSC_OUT", "PD1-OSC_OUT"},
         {"33", "/USB_DM", "PA11"}, {"34", "/USB_DP", "PA12"}, {"40", "/CAN_RX", "PB8"}, {"41", "/CAN_TX", "PB9"}});
  B.add("Y1", "Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm", "8MHz", {60 * MM, 50 * MM}, {{"1", "/OSC_IN", "1"}, {"2", "GND", "2"}, {"3", "/OSC_OUT", "3"}, {"4", "GND", "4"}});
  B.add("C1", "Capacitor_SMD:C_0603_1608Metric", "20pF", {62 * MM, 55 * MM}, {{"1", "/OSC_IN", ""}, {"2", "GND", ""}});
  B.add("C2", "Capacitor_SMD:C_0603_1608Metric", "20pF", {66 * MM, 55 * MM}, {{"1", "/OSC_OUT", ""}, {"2", "GND", ""}});
  B.add("C3", "Capacitor_SMD:C_0603_1608Metric", "100nF", {48 * MM, 44 * MM}, {{"1", "+3V3", ""}, {"2", "GND", ""}});
  B.add("U2", "Package_TO_SOT_SMD:SOT-223-3_TabPin2", "AMS1117-3.3", {20 * MM, 20 * MM}, {{"1", "GND", "GND"}, {"2", "+3V3", "VO"}, {"3", "+5V", "VI"}});
  B.add("C4", "Capacitor_SMD:C_0805_2012Metric", "10uF", {15 * MM, 25 * MM}, {{"1", "+5V", ""}, {"2", "GND", ""}});
  B.add("C5", "Capacitor_SMD:C_0805_2012Metric", "10uF", {25 * MM, 25 * MM}, {{"1", "+3V3", ""}, {"2", "GND", ""}});
  B.add("J1", "Connector_USB:USB_Micro-B_Molex_47346-0001", "USB_B_Micro", {5 * MM, 60 * MM},
        {{"1", "+5V", "VBUS"}, {"2", "/USB_DM", "D-"}, {"3", "/USB_DP", "D+"}, {"4", "", "ID"}, {"5", "GND", "GND"}});
  B.add("U3", "Package_TO_SOT_SMD:SOT-23-6", "USBLC6-2SC6", {30 * MM, 60 * MM},
        {{"1", "/USB_DP", "I/O1"}, {"2", "GND", "GND"}, {"3", "/USB_DM", "I/O2"}, {"5", "+5V", "VBUS"}});
  B.add("U4", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", "MCP2551", {70 * MM, 20 * MM},
        {{"1", "/CAN_TX", "TXD"}, {"2", "GND", "VSS"}, {"3", "+5V", "VDD"}, {"4", "/CAN_RX", "RXD"}, {"6", "/CANL", "CANL"}, {"7", "/CANH", "CANH"}});
  B.add("J2", "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical", "CAN", {95 * MM, 20 * MM},
        {{"1", "/CANH", "Pin_1"}, {"2", "/CANL", "Pin_2"}, {"3", "GND", "Pin_3"}}, false);
  B.add("H1", "MountingHole:MountingHole_3.2mm_M3", "MountingHole", {3 * MM, 3 * MM}, {{"1", "", ""}, {"2", "", ""}}, false);
  return B;
}

}  // namespace

TEST_CASE("catalogue: embedded JSON loads with every category and rule", "[crules]") {
  const Catalogue& cat = builtin_catalogue();
  CHECK(cat.version == 1);
  CHECK(cat.categories.size() == 42);
  CHECK(cat.rule_count() == 193);
  CHECK(cat.apply == 70);
  CHECK(cat.suggest == 40);
  std::set<std::string> ids;
  for (const auto& c : cat.categories) {
    CHECK(!c.detect.empty());
    for (const auto& d : c.detect) CHECK((d.weight >= 0 && d.weight <= 100));
    for (const auto& r : c.rules) {
      CHECK(ids.insert(r.id).second);
      CHECK(r.id.starts_with(c.prefix + "-"));
      CHECK(!r.kind.empty());
      CHECK(!r.sources.empty());
      CHECK(!r.enforce.empty());
    }
  }
  // Catalogue order is kept (doc 15 §2.4).
  CHECK(cat.categories.front().id == "usb2");
  CHECK(cat.index_of("crystal") < cat.index_of("ic_decoupling"));
}

TEST_CASE("catalogue: schema errors are reported, not ignored", "[crules]") {
  CHECK_THROWS(parse_catalogue(R"({"categories":[{"id":"x","detect":[{"signal":"colour","pattern":"a","weight":1}],"rules":[]}]})"));
  CHECK_THROWS(parse_catalogue(R"({"categories":[{"id":"x","detect":[{"signal":"value","pattern":"(","weight":1}],"rules":[]}]})"));
  CHECK_THROWS(parse_catalogue(R"({"categories":[{"id":"x","detect":[{"signal":"value","pattern":"a","weight":101}],"rules":[]}]})"));
  CHECK_THROWS(parse_catalogue(R"({"categories":[{"id":"x","detect":[],"rules":[{"id":"X-01","kind":"edge","severity":"maybe"}]}]})"));
}

TEST_CASE("names: references, supplies, voltages, capacitances", "[crules]") {
  CHECK(ref_letters("C12") == "C");
  CHECK(ref_letters("ic3") == "IC");
  CHECK(natural_less("C2", "C10"));
  CHECK_FALSE(natural_less("C10", "C2"));
  CHECK(natural_less("C10", "D1"));
  CHECK(natural_less("R1", "R01") != natural_less("R01", "R1"));
  CHECK(power_like_name("/+3V3"));
  CHECK(power_like_name("VDDA"));
  CHECK_FALSE(power_like_name("/OSC_IN"));
  CHECK(ground_like_name("AGND"));
  CHECK(analog_supply_name("/VREF+"));
  CHECK(net_millivolts("+5V") == 5000);
  CHECK(net_millivolts("3V3") == 3300);
  CHECK(net_millivolts("/+3.3V") == 3300);
  CHECK(net_millivolts("VCC_12V") == 12000);
  CHECK_FALSE(net_millivolts("VDD").has_value());
  CHECK(capacitance_pf("100nF") == 100'000);
  CHECK(capacitance_pf("4u7") == 4'700'000);
  CHECK(capacitance_pf("0.1uF/50V") == 100'000);
  CHECK(capacitance_pf("22p") == 22);
  CHECK(capacitance_pf("10\xc2\xb5" "F") == 10'000'000);
  CHECK_FALSE(capacitance_pf("C").has_value());
}

TEST_CASE("detection and role binding on a synthetic board with pin names", "[crules]") {
  Builder B = demo_board();
  const auto& b = B.b;
  const Detection d = detect(b, builtin_catalogue());
  CHECK(d.board_has_pin_names);

  const Instance* xt = find(d, b, "crystal", "Y1");
  REQUIRE(xt);
  CHECK(xt->confidence >= 70);
  CHECK(role_refs(b, *xt, "ic") == "U1");
  CHECK(role_refs(b, *xt, "load_caps") == "C1,C2");
  REQUIRE(xt->role("xin"));
  CHECK(b.pads[static_cast<std::size_t>(xt->role("xin")->pads.front())].number == "5");
  REQUIRE(xt->role("xout"));
  CHECK(b.pads[static_cast<std::size_t>(xt->role("xout")->pads.front())].number == "6");

  const Instance* usb = find(d, b, "usb2", "J1");
  REQUIRE(usb);
  CHECK(usb->confidence >= 70);
  CHECK(role_refs(b, *usb, "esd") == "U3");
  CHECK(role_refs(b, *usb, "transceiver") == "U1");
  REQUIRE(usb->role("dp"));
  CHECK(b.nets[static_cast<std::size_t>(usb->role("dp")->nets.front())].name == "/USB_DP");

  const Instance* esd = find(d, b, "esd_tvs", "U3");
  REQUIRE(esd);
  CHECK(role_refs(b, *esd, "connector") == "J1");

  const Instance* ldo = find(d, b, "ldo", "U2");
  REQUIRE(ldo);
  CHECK(role_refs(b, *ldo, "cin") == "C4");
  CHECK(role_refs(b, *ldo, "cout") == "C5");

  CHECK(find(d, b, "ic_decoupling", "C3"));
  CHECK_FALSE(find(d, b, "ic_decoupling", "C1"));  // a load cap is not a decoupling cap
  const Instance* can = find(d, b, "can", "U4");
  REQUIRE(can);
  CHECK(role_refs(b, *can, "connector") == "J2");
  CHECK(find(d, b, "mounting_hole", "H1"));
  CHECK(find(d, b, "connector_general", "J2"));
  CHECK_FALSE(find(d, b, "connector_general", "U1"));
}

TEST_CASE("detection is deterministic and independent of footprint order", "[crules]") {
  Builder B = demo_board();
  const Detection d1 = detect(B.b, builtin_catalogue());
  const auto j1 = report_json(B.b, builtin_catalogue(), d1, evaluate(B.b, nullptr, builtin_catalogue(), d1, Mode::Soft)).dump();
  const Detection d2 = detect(B.b, builtin_catalogue());
  CHECK(report_json(B.b, builtin_catalogue(), d2, evaluate(B.b, nullptr, builtin_catalogue(), d2, Mode::Soft)).dump() == j1);
  // Reverse the footprints (pads keep their footprint index remapped): same instances by reference.
  model::Board r = B.b;
  const int n = static_cast<int>(r.footprints.size());
  std::reverse(r.footprints.begin(), r.footprints.end());
  for (auto& p : r.pads) p.footprint = n - 1 - p.footprint;
  const Detection d3 = detect(r, builtin_catalogue());
  REQUIRE(d3.instances.size() == d1.instances.size());
  for (std::size_t i = 0; i < d1.instances.size(); ++i) {
    CHECK(d3.instances[i].category == d1.instances[i].category);
    CHECK(r.footprints[static_cast<std::size_t>(d3.instances[i].anchor)].reference == B.b.footprints[static_cast<std::size_t>(d1.instances[i].anchor)].reference);
    CHECK(d3.instances[i].confidence == d1.instances[i].confidence);
  }
}

TEST_CASE("boards without pin names: binding by nets and pad numbers, capped confidence", "[crules]") {
  Builder B = demo_board();
  for (auto& p : B.b.pads) p.pinfunction.clear();
  const auto& b = B.b;
  const Detection d = detect(b, builtin_catalogue());
  CHECK_FALSE(d.board_has_pin_names);
  const Instance* usb = find(d, b, "usb2", "J1");
  REQUIRE(usb);
  CHECK(usb->capped);
  CHECK(usb->confidence == kNoPinNameCap);
  CHECK(role_refs(b, *usb, "esd") == "U3");
  const Instance* xt = find(d, b, "crystal", "Y1");
  REQUIRE(xt);
  CHECK(xt->confidence >= 70);  // no pin_name detector: not capped
  CHECK(role_refs(b, *xt, "ic") == "U1");
  const Instance* ldo = find(d, b, "ldo", "U2");
  REQUIRE(ldo);  // rails from the net voltages: +5V in, +3V3 out
  CHECK(role_refs(b, *ldo, "cin") == "C4");
  CHECK(role_refs(b, *ldo, "cout") == "C5");
}

TEST_CASE("D25 ties are reproduced and the generalised decoupling is a superset", "[crules]") {
  Builder B = demo_board();
  B.add("C6", "Capacitor_SMD:C_0603_1608Metric", "100nF", {52 * MM, 44 * MM}, {{"1", "/VREF", ""}, {"2", "GND", ""}});
  B.add("U5", "Package_SO:SOIC-8", "ADC", {54 * MM, 46 * MM}, {{"1", "/VREF", "VREF"}, {"2", "GND", "GND"}});
  const auto all = [](int) { return true; };
  const auto plain = decap_ties(B.b, all, false);
  const auto gen = decap_ties(B.b, all, true);
  std::set<std::pair<int, int>> p, g;
  for (const auto& t : plain) p.emplace(t.cap_pad, t.ic_pad);
  for (const auto& t : gen) g.emplace(t.cap_pad, t.ic_pad);
  for (const auto& x : p) CHECK(g.count(x));
  bool vref = false;
  for (const auto& t : gen) vref |= B.b.footprints[static_cast<std::size_t>(t.cap_fp)].reference == "C6";
  CHECK(vref);
  bool vref_plain = false;
  for (const auto& t : plain) vref_plain |= B.b.footprints[static_cast<std::size_t>(t.cap_fp)].reference == "C6";
  CHECK_FALSE(vref_plain);
}

TEST_CASE("evaluation: proximity measured, statuses follow the mode", "[crules]") {
  Builder B = demo_board();
  const auto& b = B.b;
  const Catalogue& cat = builtin_catalogue();
  const Detection d = detect(b, cat);
  const Evaluation rep = evaluate(b, nullptr, cat, d, Mode::Report);
  const Evaluation soft = evaluate(b, nullptr, cat, d, Mode::Soft);
  REQUIRE(rep.rules.size() == soft.rules.size());
  CHECK(evaluate(b, nullptr, cat, d, Mode::Off).rules.empty());
  bool saw = false;
  for (std::size_t i = 0; i < rep.rules.size(); ++i) {
    const auto& r = rep.rules[i];
    const auto& s = soft.rules[i];
    CHECK(r.status != Status::Applied);  // report mode applies nothing
    if (r.spec->id == "XTAL-01") {
      saw = true;
      REQUIRE(r.measure);
      // Y1 pad 1 at (60,50) to U1 pad 5 at (52,40): 12.8 mm.
      CHECK(r.measure->value_mm > 12.0);
      CHECK_FALSE(r.measure->met);
      CHECK(s.status == Status::Applied);
    }
    if (r.spec->id == "RFM-02") CHECK(s.status != Status::Applied);
    // J2's pads (no courtyard) sit at x = 95..97 mm on a 100 mm wide board: 3 mm from the right edge.
    if (r.spec->id == "CONN-01" && b.footprints[static_cast<std::size_t>(d.instances[static_cast<std::size_t>(r.instance)].anchor)].reference == "J2") {
      REQUIRE(r.measure);
      CHECK(r.measure->value_mm == 3.0);
      CHECK_FALSE(r.measure->met);
    }
  }
  CHECK(saw);
  // Hard rules below the apply threshold are demoted to soft (doc 15 §3.2).
  for (const auto& e : soft.rules)
    if (d.instances[static_cast<std::size_t>(e.instance)].confidence < cat.apply) CHECK(e.severity != Severity::Hard);
}

TEST_CASE("placement affinities: each part pulled by one rule, locked parts never", "[crules]") {
  Builder B = demo_board();
  const Catalogue& cat = builtin_catalogue();
  const Detection d = detect(B.b, cat);
  CHECK(placement_affinities(B.b, cat, d, Mode::Report).empty());
  const auto aff = placement_affinities(B.b, cat, d, Mode::Soft);
  REQUIRE(!aff.empty());
  std::map<int, std::string> owner;
  bool esd_conn = false, xtal = false;
  for (const auto& a : aff) {
    const int sat = B.b.pads[static_cast<std::size_t>(a.pad_a)].footprint;
    const std::string group = a.rule.substr(0, a.rule.find('-'));
    if (owner.count(sat)) CHECK(owner[sat] == group);
    owner[sat] = group;
    CHECK(B.b.pads[static_cast<std::size_t>(a.pad_a)].footprint != B.b.pads[static_cast<std::size_t>(a.pad_b)].footprint);
    if (B.b.footprints[static_cast<std::size_t>(sat)].reference == "U3" && B.b.footprints[static_cast<std::size_t>(B.b.pads[static_cast<std::size_t>(a.pad_b)].footprint)].reference == "J1") {
      esd_conn = true;
      CHECK(a.weight == 20);
    }
    xtal |= a.rule == "XTAL-01";
  }
  CHECK(esd_conn);
  CHECK(xtal);
  // Lock the crystal: no pseudo-net pulls it.
  B.b.footprints[1].locked = true;
  for (const auto& a : placement_affinities(B.b, cat, detect(B.b, cat), Mode::Soft)) CHECK(B.b.pads[static_cast<std::size_t>(a.pad_a)].footprint != 1);
}

TEST_CASE("keep-outs: crystal area on the free layer, tracks only, named; sidecar rule", "[crules]") {
  Builder B = demo_board();
  const Catalogue& cat = builtin_catalogue();
  const Detection d = detect(B.b, cat);
  const auto kos = generate_keepouts(B.b, cat, d);
  const GeneratedKeepout* xk = nullptr;
  for (const auto& k : kos)
    if (k.rule == "XTAL-04") xk = &k;
  REQUIRE(xk);
  CHECK(xk->zone.rule_area);
  CHECK(xk->zone.keepout_tracks);
  CHECK_FALSE(xk->zone.keepout_vias);
  CHECK(xk->zone.copper == model::layer_bit(1));  // SMD parts on F.Cu: the keep-out is on B.Cu
  CHECK(xk->zone.name == "tmk:XTAL-04:Y1");
  REQUIRE(!xk->zone.outline.empty());
  // The crystal pads and load caps are inside the area.
  for (const auto& p : B.b.pads) {
    const std::string& r = B.b.footprints[static_cast<std::size_t>(p.footprint)].reference;
    if (r == "Y1" || r == "C1" || r == "C2") CHECK(geom::point_in_polygon(p.pos, xk->zone.outline.front()));
  }
  const std::string dru = dru_sidecar(B.b, cat, d);
  CHECK(dru.starts_with("(version 1)"));
  CHECK(dru.find("(rule \"tmk XTAL-04 Y1\"") != std::string::npos);
  CHECK(dru.find("A.intersectsCourtyard('Y1')") != std::string::npos);
  CHECK(dru.find("A.NetName != '/OSC_IN'") != std::string::npos);
  CHECK(dru.find("A.NetName != 'GND'") != std::string::npos);
  // Through-hole crystal: pads on every layer, no keep-out, reported.
  Builder T = demo_board();
  for (int pi : T.b.footprints[1].pads) T.b.pads[static_cast<std::size_t>(pi)].copper = model::layer_bit(0) | model::layer_bit(1);
  std::vector<std::string> why;
  for (const auto& k : generate_keepouts(T.b, cat, detect(T.b, cat), &why)) CHECK(k.rule != "XTAL-04");
  bool reported = false;
  for (const auto& w : why) reported |= w.starts_with("XTAL-04 Y1:");
  CHECK(reported);
}

TEST_CASE("USB 2.0 pairs for coupled routing (USB2-02)", "[crules]") {
  Builder B = demo_board();
  const Catalogue& cat = builtin_catalogue();
  const auto pairs = usb_pairs(B.b, cat, detect(B.b, cat));
  REQUIRE(pairs.size() == 1);
  CHECK(B.b.nets[static_cast<std::size_t>(pairs[0].first)].name == "/USB_DP");
  CHECK(B.b.nets[static_cast<std::size_t>(pairs[0].second)].name == "/USB_DM");
  // Report mode applies nothing; soft applies USB2-02 (routing preference).
  const Detection d = detect(B.b, cat);
  for (const auto& e : evaluate(B.b, nullptr, cat, d, Mode::Soft).rules)
    if (e.spec->id == "USB2-02") CHECK(e.status == Status::Applied);
  for (const auto& e : evaluate(B.b, nullptr, cat, d, Mode::Report).rules)
    if (e.spec->id == "USB2-02") CHECK(e.status == Status::NotApplied);
}

TEST_CASE("PCBench fixture: detection report is stable and finds the crystal", "[crules][fixture]") {
  const std::string path = std::string(TM_SOURCE_DIR) + "/bench/data/freerouting/scripts/benchmark/fixtures/PCBench/1Bitsy_1bitsy/unrouted.kicad_pcb";
  if (!std::filesystem::exists(path)) SKIP("fixture missing: " + path);
  const auto lb = io::read_board_file(path);
  const auto rules = io::read_design_rules(path);
  const Catalogue& cat = builtin_catalogue();
  const Detection d = detect(lb.board, cat);
  CHECK(!d.instances.empty());
  bool crystal = false, decap = false;
  for (const auto& in : d.instances) {
    crystal |= cat.categories[static_cast<std::size_t>(in.category)].id == "crystal";
    decap |= cat.categories[static_cast<std::size_t>(in.category)].id == "ic_decoupling";
  }
  CHECK(crystal);
  CHECK(decap);
  const auto j1 = report_json(lb.board, cat, d, evaluate(lb.board, &rules, cat, d, Mode::Soft)).dump();
  const Detection d2 = detect(lb.board, cat);
  CHECK(report_json(lb.board, cat, d2, evaluate(lb.board, &rules, cat, d2, Mode::Soft)).dump() == j1);
}

// ---- User override file (doc 15 §3.5 level 1, §6.3) -------------------------------------------------------------

namespace {

Overrides ovr(const std::string& json) { return parse_overrides(json, builtin_catalogue(), "test.json"); }

std::string error_of(const std::string& json) {
  try {
    ovr(json);
  } catch (const std::exception& e) {
    return e.what();
  }
  return "";
}

const EffectiveRule* rule_of(const Evaluation& ev, const Detection& d, const model::Board& b, const std::string& id, const std::string& ref) {
  for (const auto& e : ev.rules)
    if (e.spec->id == id && b.footprints[static_cast<std::size_t>(d.instances[static_cast<std::size_t>(e.instance)].anchor)].reference == ref) return &e;
  return nullptr;
}

}  // namespace

TEST_CASE("overrides: every form parses, in file order", "[crules][overrides]") {
  const Overrides o = ovr(R"({"version": 1, "comment": "board-specific",
    "disable": ["XTAL-04", "USB2-04@J1", "mounting_hole", "crystal@Y1"],
    "assert": {"J2": "usb2", "U4": ["can", "esd_tvs"]},
    "deny": [{"category": "ldo", "ref": "U2"}],
    "set": [{"rule": "XTAL-02", "param": "max_mm", "value": 5}, {"rule": "XTAL-01@Y1", "param": "max_mm", "value": 8},
            {"rule": "XTAL-01", "ref": "Y1", "param": "max_mm", "value": 9}]})");
  REQUIRE(o.entries.size() == 11);
  using K = OverrideEntry::Kind;
  CHECK(o.entries[0].kind == K::Disable);
  CHECK(o.entries[0].rule == "XTAL-04");
  CHECK(o.entries[0].category == "crystal");
  CHECK(o.entries[0].ref.empty());
  CHECK(o.entries[1].rule == "USB2-04");
  CHECK(o.entries[1].ref == "J1");
  CHECK(o.entries[2].rule.empty());
  CHECK(o.entries[2].category == "mounting_hole");
  CHECK(o.entries[3].category == "crystal");
  CHECK(o.entries[3].ref == "Y1");
  CHECK(o.entries[4].kind == K::Assert);
  CHECK(o.entries[4].text == "assert usb2@J2");
  CHECK(o.entries[6].category == "esd_tvs");
  CHECK(o.entries[7].kind == K::Deny);
  CHECK(o.entries[7].text == "deny ldo@U2");
  CHECK(o.entries[8].kind == K::Set);
  CHECK(o.entries[8].value == 5);
  CHECK(o.entries[9].ref == "Y1");
  CHECK(o.entries[10].ref == "Y1");
  CHECK(o.entries[10].text == "set XTAL-01@Y1 max_mm = 9");
  CHECK(ovr("{}").empty());
}

TEST_CASE("overrides: typos and contradictions are errors with a clear message", "[crules][overrides]") {
  auto has = [](const std::string& json, const std::string& msg) {
    const std::string e = error_of(json);
    INFO(json << " -> " << e);
    CHECK(e.find(msg) != std::string::npos);
  };
  has(R"({"disabel": ["XTAL-04"]})", "unknown key 'disabel'");
  has(R"({"disable": ["XTAL-4"]})", "unknown rule id or category 'XTAL-4' (did you mean XTAL-04?)");
  has(R"({"disable": ["cristal"]})", "did you mean crystal?");
  has(R"({"disable": ["XTAL-04@"]})", "not ID or ID@REF");
  has(R"({"disable": "XTAL-04"})", "expected a list");
  has(R"({"assert": {"J2": "usb"}})", "unknown category 'usb'");
  has(R"({"deny": [{"category": "buck"}]})", "'ref' must be a non-empty string");
  has(R"({"deny": [{"category": "buck", "ref": "U7", "why": "x"}]})", "unknown key 'why'");
  has(R"({"set": [{"rule": "XTAL-02", "param": "max", "value": 5}]})", "has no parameter 'max'");
  has(R"({"set": [{"rule": "XTAL-02", "param": "max_mm", "value": "5"}]})", "is a number");
  has(R"({"set": [{"rule": "XTAL-02", "param": "max_mm", "value": -1}]})", "non-negative");
  has(R"({"set": [{"rule": "XTAL-02", "param": "max_mm"}]})", "'value' is missing");
  has(R"({"set": [{"rule": "XTAL-02@Y1", "ref": "Y1", "param": "max_mm", "value": 1}]})", "not both");
  has(R"({"set": [{"rule": "XTAL-99", "param": "max_mm", "value": 1}]})", "unknown rule id 'XTAL-99'");
  has(R"({"assert": {"U2": "ldo"}, "deny": {"U2": "ldo"}})", "both asserted and denied");
  has(R"({"version": 2})", "version must be 1");
  has(R"({"stackup_preset": "jlc"})", "unknown key 'stackup_preset'");
  has("disable: [XTAL-04]", "not valid JSON");
  has(R"({"x": 1})", "test.json: ");
  CHECK(error_of(R"({"disable": ["XTAL-04"]})").empty());
  // YAML is refused with the conversion command.
  try {
    load_overrides_file("board.tracemaker_rules.yaml", builtin_catalogue());
    FAIL("YAML accepted");
  } catch (const std::exception& e) {
    CHECK(std::string(e.what()).find("scripts/crules_override.py") != std::string::npos);
  }
  // References are checked against the board.
  Builder B = demo_board();
  auto detect_error = [&](const std::string& json) {
    const Overrides o = ovr(json);
    try {
      detect(B.b, builtin_catalogue(), &o);
    } catch (const std::exception& e) {
      return std::string(e.what());
    }
    return std::string();
  };
  CHECK(detect_error(R"({"disable": ["XTAL-04@Y9"]})").find("disable XTAL-04@Y9: no footprint Y9 on the board") != std::string::npos);
  CHECK(detect_error(R"({"assert": {"U2": ["ldo", "buck"]}})").find("exclude each other") != std::string::npos);
  CHECK(detect_error(R"({"disable": ["XTAL-04@Y1"]})").empty());
}

TEST_CASE("overrides: disable a rule, a rule on one part, a category", "[crules][overrides]") {
  Builder B = demo_board();
  const auto& b = B.b;
  const Catalogue& cat = builtin_catalogue();
  {
    bool ko = false;
    for (const auto& k : generate_keepouts(b, cat, detect(b, cat))) ko |= k.rule == "XTAL-04";
    CHECK(ko);  // without overrides the crystal keep-out exists
  }
  const Overrides o1 = ovr(R"({"disable": ["XTAL-04", "XTAL-01@Y1", "USB2-02"]})");
  const Detection d = detect(b, cat, &o1);
  CHECK(d.override_unused.empty());
  for (const auto& k : generate_keepouts(b, cat, d)) CHECK(k.rule != "XTAL-04");
  CHECK(dru_sidecar(b, cat, d).find("XTAL-04") == std::string::npos);
  CHECK(usb_pairs(b, cat, d).empty());
  bool xtal02 = false;
  for (const auto& a : placement_affinities(b, cat, d, Mode::Soft)) {
    CHECK(a.rule != "XTAL-01");
    xtal02 |= a.rule == "XTAL-02";
  }
  CHECK(xtal02);
  const Evaluation ev = evaluate(b, nullptr, cat, d, Mode::On);
  const EffectiveRule* x1 = rule_of(ev, d, b, "XTAL-01", "Y1");
  REQUIRE(x1);
  CHECK(x1->status == Status::NotApplied);
  CHECK(x1->detail == "overridden by user: disabled (disable XTAL-01@Y1)");
  CHECK_FALSE(x1->measure);
  const EffectiveRule* x2 = rule_of(ev, d, b, "XTAL-02", "Y1");
  REQUIRE(x2);
  CHECK(x2->status == Status::Applied);
  const std::string text = report_text(b, cat, d, ev);
  CHECK(text.find("user overrides (test.json, doc 15 §6.3): 3 entries") != std::string::npos);
  CHECK(text.find("overridden by user: disabled (disable XTAL-04)") != std::string::npos);
  const auto j = report_json(b, cat, d, ev);
  CHECK(j["user_overrides"]["entries"].size() == 3);
  // A whole category: no crystal rule is applied, the instance is still reported.
  const Overrides o2 = ovr(R"({"disable": ["crystal"]})");
  const Detection d2 = detect(b, cat, &o2);
  REQUIRE(find(d2, b, "crystal", "Y1"));
  for (const auto& a : placement_affinities(b, cat, d2, Mode::Soft)) CHECK_FALSE(a.rule.starts_with("XTAL-"));
  for (const auto& e : evaluate(b, nullptr, cat, d2, Mode::On).rules)
    if (e.spec->id.starts_with("XTAL-")) CHECK(e.detail.starts_with("overridden by user: disabled (disable crystal)"));
  // An entry that matches no instance is a warning in the report, not silently dropped.
  const Overrides o3 = ovr(R"({"disable": ["USB2-04@J2", "BUCK-06"]})");
  const Detection d3 = detect(b, cat, &o3);
  REQUIRE(d3.override_unused.size() == 2);
  CHECK(d3.override_unused[0] == "disable BUCK-06: no buck instance on this board");
  CHECK(d3.override_unused[1] == "disable USB2-04@J2: J2 is not detected as usb2 (assert it to force the category)");
  CHECK(report_text(b, cat, d3, evaluate(b, nullptr, cat, d3, Mode::Soft)).find("warning: override matched nothing: disable BUCK-06") != std::string::npos);
}

TEST_CASE("overrides: deny and assert change which instances exist", "[crules][overrides]") {
  Builder B = demo_board();
  const auto& b = B.b;
  const Catalogue& cat = builtin_catalogue();
  const Overrides o = ovr(R"({"deny": {"U2": "ldo"}, "assert": {"J2": "usb2", "U1": "connector_general"}})");
  const Detection d = detect(b, cat, &o);
  CHECK_FALSE(find(d, b, "ldo", "U2"));
  bool listed = false;
  for (const auto& p : d.possible)
    listed |= cat.categories[static_cast<std::size_t>(p.category)].id == "ldo" && p.superseded_by == "denied by user (test.json)";
  CHECK(listed);
  for (const auto& a : placement_affinities(b, cat, d, Mode::Soft)) CHECK_FALSE(a.rule.starts_with("LDO-"));
  const Instance* u = find(d, b, "usb2", "J2");
  REQUIRE(u);
  CHECK(u->asserted);
  CHECK(u->confidence == 100);
  CHECK(std::find(u->evidence.begin(), u->evidence.end(), "user assert") != u->evidence.end());
  // The binder rejects U1 as a connector; the user's assert wins and binds the anchor alone.
  const Instance* c = find(d, b, "connector_general", "U1");
  REQUIRE(c);
  CHECK(c->asserted);
  CHECK(role_refs(b, *c, "connector") == "U1");
  CHECK(report_text(b, cat, d, evaluate(b, nullptr, cat, d, Mode::Soft)).find("(asserted by user)") != std::string::npos);
  // Assert wins a conflict within an exclusive group (doc 15 §3.4): U2 forced to buck supersedes its ldo detection.
  const Overrides ob = ovr(R"({"assert": {"U2": "buck"}})");
  const Detection db = detect(b, cat, &ob);
  CHECK(find(db, b, "buck", "U2"));
  CHECK_FALSE(find(db, b, "ldo", "U2"));
}

TEST_CASE("overrides: set changes parameters; @REF wins over global, later wins among equals", "[crules][overrides]") {
  Builder B = demo_board();
  const auto& b = B.b;
  const Catalogue& cat = builtin_catalogue();
  // XTAL-01 measures 12.8 mm against its catalogue limit (violated).
  for (const char* json :
       {R"({"set": [{"rule": "XTAL-01@Y1", "param": "max_mm", "value": 20}, {"rule": "XTAL-01", "param": "max_mm", "value": 5}]})",
        R"({"set": [{"rule": "XTAL-01", "param": "max_mm", "value": 5}, {"rule": "XTAL-01", "param": "max_mm", "value": 7}, {"rule": "XTAL-01", "ref": "Y1", "param": "max_mm", "value": 20}]})"}) {
    const Overrides o = ovr(json);
    const Detection d = detect(b, cat, &o);
    const Evaluation ev = evaluate(b, nullptr, cat, d, Mode::Soft);
    const EffectiveRule* x1 = rule_of(ev, d, b, "XTAL-01", "Y1");
    REQUIRE(x1);
    REQUIRE(x1->measure);
    CHECK(x1->measure->limit_mm == 20.0);
    CHECK(x1->measure->met);
    CHECK(x1->status == Status::Applied);
    CHECK(x1->detail.find("overridden by user: max_mm = 20 (catalogue ") != std::string::npos);
    CHECK(rule_params(d.instances[static_cast<std::size_t>(x1->instance)], *x1->spec)["max_mm"] == 20);
    CHECK(x1->spec->params["max_mm"] != 20);  // the catalogue itself is untouched
  }
  // A larger keep-out margin grows the generated area.
  auto width = [](const std::vector<GeneratedKeepout>& kos) {
    for (const auto& k : kos)
      if (k.rule == "XTAL-04") {
        Coord lo = k.zone.outline.front().front().x, hi = lo;
        for (const auto& q : k.zone.outline.front()) lo = std::min(lo, q.x), hi = std::max(hi, q.x);
        return hi - lo;
      }
    return Coord{0};
  };
  const Detection d0 = detect(b, cat);
  const Overrides o = ovr(R"({"set": [{"rule": "XTAL-04", "param": "margin_mm", "value": 3}]})");
  const Detection d1 = detect(b, cat, &o);
  CHECK(width(generate_keepouts(b, cat, d1)) > width(generate_keepouts(b, cat, d0)) + 4 * MM);
  // Deterministic: the same file gives the same report.
  const auto j1 = report_json(b, cat, d1, evaluate(b, nullptr, cat, d1, Mode::On)).dump();
  const Detection d2 = detect(b, cat, &o);
  CHECK(report_json(b, cat, d2, evaluate(b, nullptr, cat, d2, Mode::On)).dump() == j1);
}

// ---- Ethernet magnetics void (ETH-05) ----------------------------------------------------------------------------

namespace {

// PHY U10 -- MDI pairs -- discrete magnetics T1 (8 pads) -- line pairs -- RJ45 J3; a 4-pad common-mode choke L5 on
// the PHY's MDI pairs is not magnetics.
Builder eth_board(const std::string& rj45_lib = "Connector_RJ:RJ45_Amphenol_54602-x08_Horizontal", bool tht_magnetics = false) {
  Builder B;
  B.add("U10", "Package_DFN_QFN:QFN-24-1EP_4x4mm_P0.5mm", "LAN8720A", {30 * MM, 40 * MM},
        {{"1", "/ETH_TXP", "TXP"}, {"2", "/ETH_TXN", "TXN"}, {"3", "/ETH_RXP", "RXP"}, {"4", "/ETH_RXN", "RXN"}, {"5", "GND", "VSS"}, {"6", "+3V3", "VDD"}});
  B.add("T1", "Transformer_SMD:Pulse_H1102NL", "H1102NL", {50 * MM, 40 * MM},
        {{"1", "/ETH_TXP", "TD+"}, {"2", "/CT", "CT"}, {"3", "/ETH_TXN", "TD-"}, {"6", "/ETH_RXP", "RD+"}, {"7", "/CT", "CT"}, {"8", "/ETH_RXN", "RD-"},
         {"9", "/LINE_RXN", "RX-"}, {"11", "/LINE_RXP", "RX+"}, {"14", "/LINE_TXN", "TX-"}, {"16", "/LINE_TXP", "TX+"}},
        !tht_magnetics);
  B.add("J3", rj45_lib, "RJ45", {80 * MM, 40 * MM},
        {{"1", "/LINE_TXP", "TD+"}, {"2", "/LINE_TXN", "TD-"}, {"3", "/LINE_RXP", "RD+"}, {"6", "/LINE_RXN", "RD-"}, {"SH", "/CHASSIS", "SHIELD"}}, false);
  B.add("L5", "Inductor_SMD:L_CommonModeChoke_Wuerth_WE-SL5", "CMC", {40 * MM, 50 * MM},
        {{"1", "/ETH_TXP", ""}, {"2", "/ETH_TXN", ""}, {"3", "/ETH_RXP", ""}, {"4", "/ETH_RXN", ""}});
  return B;
}

}  // namespace

TEST_CASE("ethernet: discrete magnetics bound and voided on the adjacent layer (ETH-05)", "[crules][ethernet]") {
  Builder B = eth_board();
  const auto& b = B.b;
  const Catalogue& cat = builtin_catalogue();
  const Detection d = detect(b, cat);
  const Instance* j = find(d, b, "ethernet", "J3");
  REQUIRE(j);
  CHECK(j->confidence >= cat.apply);
  CHECK(role_refs(b, *j, "rj45") == "J3");
  CHECK(role_refs(b, *j, "magnetics") == "T1");  // not the 4-pad choke
  const Instance* u = find(d, b, "ethernet", "U10");
  REQUIRE(u);
  CHECK(role_refs(b, *u, "phy") == "U10");
  CHECK(role_refs(b, *u, "magnetics") == "T1");
  // One void for the shared magnetics, on B.Cu (the SMD transformer's pads are on F.Cu): tracks, vias and zones.
  std::vector<const GeneratedKeepout*> eth;
  const auto kos = generate_keepouts(b, cat, d);
  for (const auto& k : kos)
    if (k.rule == "ETH-05") eth.push_back(&k);
  REQUIRE(eth.size() == 1);
  CHECK(eth[0]->zone.name == "tmk:ETH-05:J3");
  CHECK(eth[0]->zone.copper == model::layer_bit(1));
  CHECK(eth[0]->zone.keepout_tracks);
  CHECK(eth[0]->zone.keepout_vias);
  CHECK(eth[0]->zone.keepout_pour);
  for (int pi : b.footprints[1].pads) CHECK(geom::point_in_polygon(b.pads[static_cast<std::size_t>(pi)].pos, eth[0]->zone.outline.front()));
  // Statuses: applied only in `on`; the PHY's instance points to the shared void.
  const Evaluation on = evaluate(b, nullptr, cat, d, Mode::On);
  const EffectiveRule* ej = rule_of(on, d, b, "ETH-05", "J3");
  REQUIRE(ej);
  CHECK(ej->status == Status::Applied);
  CHECK(ej->detail.find("tracks, vias, zones") != std::string::npos);
  const EffectiveRule* eu = rule_of(on, d, b, "ETH-05", "U10");
  REQUIRE(eu);
  CHECK(eu->status == Status::NotApplied);
  CHECK(eu->detail.find("another Ethernet instance") != std::string::npos);
  for (const Mode m : {Mode::Report, Mode::Soft}) {
    const Evaluation ev = evaluate(b, nullptr, cat, d, m);
    const EffectiveRule* x = rule_of(ev, d, b, "ETH-05", "J3");
    REQUIRE(x);
    CHECK(x->status == Status::NotApplied);
  }
  // Sidecar: no ground exemption for a void, zones disallowed too.
  const std::string dru = dru_sidecar(b, cat, d);
  const auto at = dru.find("(rule \"tmk ETH-05 J3\"");
  REQUIRE(at != std::string::npos);
  const std::string rule = dru.substr(at, dru.find("\n(rule", at + 1) - at);
  CHECK(rule.find("(constraint disallow track via zone)") != std::string::npos);
  CHECK(rule.find("A.intersectsCourtyard('T1')") != std::string::npos);
  CHECK(rule.find("A.NetName != '/ETH_TXP'") != std::string::npos);
  CHECK(rule.find("'GND'") == std::string::npos);
  // The user can switch it off.
  const Overrides o = ovr(R"({"disable": ["ETH-05"]})");
  for (const auto& k : generate_keepouts(b, cat, detect(b, cat, &o))) CHECK(k.rule != "ETH-05");
}

TEST_CASE("ethernet: four layers, MagJack, through-hole magnetics", "[crules][ethernet]") {
  const Catalogue& cat = builtin_catalogue();
  {  // Four copper layers: the void is on In1.Cu only ("same, adjacent"; F.Cu holds the pads).
    Builder B = eth_board();
    B.b.layers.push_back({1, "In1.Cu", "In1.Cu", "signal", "", 2});
    B.b.layers.push_back({2, "In2.Cu", "In2.Cu", "signal", "", 3});
    B.b.copper = {0, 2, 3, 1};
    for (auto& p : B.b.pads)
      if (p.copper == (model::layer_bit(0) | model::layer_bit(1))) p.copper = model::layer_bit(0) | model::layer_bit(1) | model::layer_bit(2) | model::layer_bit(3);
    int n = 0;
    for (const auto& k : generate_keepouts(B.b, cat, detect(B.b, cat)))
      if (k.rule == "ETH-05") {
        ++n;
        CHECK(k.zone.copper == model::layer_bit(1));
        CHECK(k.zone.layers == std::vector<std::string>{"In1.Cu"});
      }
    CHECK(n == 1);
  }
  {  // RJ45 with integrated magnetics: no void, reported as not required.
    Builder B = eth_board("Connector_RJ:RJ45_Hanrun_HR911105A_Horizontal");
    const Detection d = detect(B.b, cat);
    const Instance* j = find(d, B.b, "ethernet", "J3");
    REQUIRE(j);
    CHECK(j->role("integrated_magnetics"));
    CHECK_FALSE(j->role("magnetics"));
    const Evaluation ev = evaluate(B.b, nullptr, cat, d, Mode::On);
    const EffectiveRule* e = rule_of(ev, d, B.b, "ETH-05", "J3");
    REQUIRE(e);
    INFO(e->detail);
    CHECK(e->detail.find("not required") != std::string::npos);
  }
  {  // Through-hole magnetics on two layers: no free layer, reported, never a partial void.
    Builder B = eth_board("Connector_RJ:RJ45_Amphenol_54602-x08_Horizontal", true);
    std::vector<std::string> why;
    for (const auto& k : generate_keepouts(B.b, cat, detect(B.b, cat), &why)) CHECK(k.rule != "ETH-05");
    bool reported = false;
    for (const auto& w : why) reported |= w.starts_with("ETH-05 J3:");
    CHECK(reported);
  }
  {  // No magnetics at all (PHY wired straight to a plain RJ45 is not a design TraceMaker fixes): role unbound.
    Builder B;
    B.add("J3", "Connector_RJ:RJ45_Amphenol_54602-x08_Horizontal", "RJ45", {80 * MM, 40 * MM},
          {{"1", "/ETH_TXP", "TD+"}, {"2", "/ETH_TXN", "TD-"}, {"3", "/ETH_RXP", "RD+"}, {"6", "/ETH_RXN", "RD-"}}, false);
    B.add("U10", "Package_DFN_QFN:QFN-24", "LAN8720A", {30 * MM, 40 * MM}, {{"1", "/ETH_TXP", "TXP"}, {"2", "/ETH_TXN", "TXN"}, {"3", "/ETH_RXP", "RXP"}, {"4", "/ETH_RXN", "RXN"}});
    const Detection d = detect(B.b, cat);
    const Evaluation ev = evaluate(B.b, nullptr, cat, d, Mode::On);
    const EffectiveRule* e = rule_of(ev, d, B.b, "ETH-05", "J3");
    REQUIRE(e);
    INFO(e->detail);
    CHECK(e->detail.find("role magnetics not bound") != std::string::npos);
  }
}

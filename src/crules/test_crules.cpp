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

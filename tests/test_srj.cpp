// SPDX-License-Identifier: GPL-3.0-or-later
// Simple Route JSON reader, KiCad converter and trace writer (doc 08 §9) on the hand-made problems in tests/srj.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <numbers>

#include "drc/copper.hpp"
#include "geom/shape.hpp"
#include "io/kicad/board_editor.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "io/srj/srj.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace tmk;

namespace {

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  REQUIRE(f.good());
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::string mixed_text() { return slurp(fs::path(TM_SOURCE_DIR) / "tests/srj/mixed.json"); }

io::LoadedBoard load(const std::string& pcb) {
  io::LoadedBoard lb;
  lb.doc = sexpr::Document::parse(pcb);
  lb.board = io::read_board(lb.doc);
  return lb;
}

const model::Pad* pad_at(const model::Board& b, double x_mm, double y_mm, int layer) {
  for (const auto& p : b.pads)
    if (p.pos == geom::Point{mm_to_nm(x_mm), mm_to_nm(y_mm)} && (p.copper & model::layer_bit(layer))) return &p;
  return nullptr;
}

bool inside(const model::Pad& pad, geom::Point q) {
  const auto shapes = drc::pad_shapes(pad);
  return std::any_of(shapes.begin(), shapes.end(), [&](const geom::Shape& s) { return geom::closer_than_disk(s, q, 0, 2); });
}

}  // namespace

TEST_CASE("SRJ layer names follow tscircuit's stack order", "[srj]") {
  CHECK(io::srj_layer_index("top", 4) == 0);
  CHECK(io::srj_layer_index("inner1", 4) == 1);
  CHECK(io::srj_layer_index("inner2", 4) == 2);
  CHECK(io::srj_layer_index("bottom", 4) == 3);
  CHECK(io::srj_layer_index("inner3", 4) == -1);
  CHECK(io::srj_layer_index("inner1", 2) == -1);
  CHECK(io::srj_layer_index("F.Cu", 2) == -1);
  for (int z = 0; z < 6; ++z) CHECK(io::srj_layer_index(io::srj_layer_name(z, 6), 6) == z);
}

TEST_CASE("SRJ reader: nets, shapes, layers and rules", "[srj]") {
  const auto p = io::read_srj(mixed_text());
  CHECK(p.layer_count == 4);
  CHECK(p.outline.size() == 6);
  REQUIRE(p.obstacles.size() == 10);
  REQUIRE(p.connections.size() == 4);
  // C1 and C2 share the point "C_pad": one net, named after its first connection.
  REQUIRE(p.nets.size() == 3);
  CHECK(p.nets[0].name == "A");
  CHECK(p.nets[1].name == "B");
  CHECK(p.nets[2].name == "C1");
  CHECK(p.nets[2].connections == std::vector<int>{2, 3});
  CHECK(p.connections[3].net == 2);
  CHECK(p.connections[3].width == 0.3);
  // Obstacles join a net through the connection name, a port id or a point id; a keep-out and a net nobody
  // asks to connect have none.
  CHECK(p.obstacles[0].net == 0);
  CHECK(p.obstacles[0].rotation == 30.0);
  CHECK(p.obstacles[1].net == 0);
  CHECK(p.obstacles[1].circle);
  CHECK(p.obstacles[2].net == 1);
  CHECK(p.obstacles[2].layers == std::vector<int>{0, 1, 2, 3});
  CHECK(p.obstacles[3].layers == std::vector<int>{3});
  CHECK(p.obstacles[4].net == -1);
  CHECK(p.obstacles[5].non_plated_hole);
  CHECK(p.obstacles[6].copper_pour);
  CHECK(p.obstacles[6].net == 0);
  CHECK(p.obstacles[7].net == 2);
  CHECK(p.obstacles[8].net == 2);
  CHECK(p.obstacles[9].net == -1);
  // A multi-layer point.
  CHECK(p.connections[1].points[0].layers == std::vector<int>{0, 3});
  CHECK(p.connections[1].points[0].pcb_port_id == "port_b1");
  CHECK(p.rules.min_trace_width == 0.15);
  CHECK(p.rules.nominal_trace_width == 0.2);
  CHECK(p.rules.via_pad == 0.6);
  CHECK(p.rules.via_hole == 0.3);
  CHECK(p.rules.obstacle_margin == 0.2);
  CHECK(p.rules.trace_to_pad == 0.25);
  CHECK(p.rules.blind_buried_vias);
  CHECK_FALSE(p.rules.via_in_pad);
}

TEST_CASE("SRJ reader rejects what it cannot interpret", "[srj]") {
  json d = json::parse(mixed_text());
  CHECK_THROWS(io::read_srj("{"));
  CHECK_THROWS(io::read_srj("[]"));
  auto broken = [&](auto&& edit) {
    json x = d;
    edit(x);
    return x.dump();
  };
  CHECK_THROWS(io::read_srj(broken([](json& x) { x.erase("layerCount"); })));
  CHECK_THROWS(io::read_srj(broken([](json& x) { x.erase("bounds"); })));
  CHECK_THROWS(io::read_srj(broken([](json& x) { x["minTraceWidth"] = 0; })));
  CHECK_THROWS(io::read_srj(broken([](json& x) { x["obstacles"][0]["layers"] = {"F.Cu"}; })));
  CHECK_THROWS(io::read_srj(broken([](json& x) { x["obstacles"][0]["type"] = "oval"; })));
  CHECK_THROWS(io::read_srj(broken([](json& x) { x["obstacles"][1]["shape"] = "pill"; })));
  CHECK_THROWS(io::read_srj(broken([](json& x) { x["connections"][0]["pointsToConnect"][0]["layer"] = "inner7"; })));
  CHECK_THROWS(io::read_srj(broken([](json& x) { x["connections"][0]["pointsToConnect"][0].erase("layer"); })));
  CHECK_THROWS(io::read_srj(broken([](json& x) { x["obstacles"][5]["connectedTo"] = {"A"}; })));  // a connected non-plated hole
  // Features that are read but not honoured are named.
  const auto p = io::read_srj(broken([](json& x) {
    x["traces"] = json::array({json::object()});
    x["allowJumpers"] = true;
  }));
  CHECK(p.warnings.size() == 2);
}

TEST_CASE("SRJ via sizes resolve as tscircuit's getViaDimensions", "[srj]") {
  json d = json::parse(mixed_text());
  d.erase("minViaPadDiameter");
  d.erase("minViaHoleDiameter");
  auto files = io::srj_to_kicad(io::read_srj(d.dump()));
  CHECK(files.via_diameter == 300'000);  // fallback pad 0.3, hole half of it
  CHECK(files.via_drill == 150'000);
  d["minViaDiameter"] = 0.5;  // deprecated single diameter: the pad
  files = io::srj_to_kicad(io::read_srj(d.dump()));
  CHECK(files.via_diameter == 500'000);
  CHECK(files.via_drill == 250'000);
  d["min_via_pad_diameter"] = 0.45;  // the explicit fields win
  d["min_via_hole_diameter"] = 0.2;
  files = io::srj_to_kicad(io::read_srj(d.dump()));
  CHECK(files.via_diameter == 450'000);
  CHECK(files.via_drill == 200'000);
}

TEST_CASE("SRJ to KiCad: pads, nets, outline and zones as the board reader sees them", "[srj]") {
  const auto problem = io::read_srj(mixed_text());
  const auto files = io::srj_to_kicad(problem);
  // Deterministic: the same bytes again.
  const auto again = io::srj_to_kicad(io::read_srj(mixed_text()));
  CHECK(files.pcb == again.pcb);
  CHECK(files.pro == again.pro);
  CHECK(files.dru == again.dru);
  CHECK(files.srj_pairs == 4);
  CHECK(files.board_pairs == 4);
  CHECK(files.terminal_pads == 1);

  const auto lb = load(files.pcb);
  const auto& b = lb.board;
  REQUIRE(b.copper_count() == 4);
  CHECK(b.copper_name(1) == "In1.Cu");
  CHECK(b.net_by_name("A") > 0);
  CHECK(b.net_by_name("B") > 0);
  CHECK(b.net_by_name("C1") > 0);
  CHECK(b.net_by_name("C2") == 0);
  // y is mirrored: SRJ (-6, 4) is KiCad (-6, -4).
  const auto* a1 = pad_at(b, -6, -4, 0);
  REQUIRE(a1);
  CHECK(a1->net == b.net_by_name("A"));
  CHECK(a1->type == model::PadType::Smd);
  // Rotated 30 degrees counter-clockwise as drawn: the SRJ corner (w/2, h/2) turned by +30 degrees, y mirrored.
  const double c = std::cos(std::numbers::pi / 6), s = std::sin(std::numbers::pi / 6);
  const double cx = -6 + 0.5 * c - 0.3 * s, cy = 4 + 0.5 * s + 0.3 * c;
  CHECK(inside(*a1, {mm_to_nm(cx - 0.01 * c), -mm_to_nm(cy - 0.01 * s)}));
  CHECK_FALSE(inside(*a1, {mm_to_nm(cx + 0.01 * c), -mm_to_nm(cy + 0.01 * s)}));
  // The mirror image of that corner is outside: the rotation did not flip.
  CHECK_FALSE(inside(*a1, {mm_to_nm(-6 + 0.5 * c - 0.3 * s - 0.01), -mm_to_nm(4 - 0.5 * s - 0.3 * c + 0.01)}));
  const auto* a2 = pad_at(b, 6, -4, 0);
  REQUIRE(a2);
  CHECK(a2->shape == model::PadShape::Circle);
  CHECK(a2->size_x == 800'000);
  // The connected four-layer obstacle is a plated hole, drilled at half its smaller side.
  const auto* b1 = pad_at(b, -6, 4, 2);
  REQUIRE(b1);
  CHECK(b1->type == model::PadType::ThruHole);
  CHECK(b1->drill_x == 600'000);
  CHECK(b1->net == b.net_by_name("B"));
  const auto* b2 = pad_at(b, 6, 4, 3);
  REQUIRE(b2);
  CHECK(b2->copper == model::layer_bit(3));
  // The keep-out is one net-less pad on each of its layers and none elsewhere.
  for (int z : {0, 1}) {
    const auto* k = pad_at(b, 0, 0, z);
    REQUIRE(k);
    CHECK(k->net == 0);
    CHECK(k->copper == model::layer_bit(z));
  }
  CHECK(pad_at(b, 0, 0, 2) == nullptr);
  // A connected obstacle no connection asks for has no net: the ratsnest must not ask for it either.
  const auto* nc = pad_at(b, 8, 0, 3);
  REQUIRE(nc);
  CHECK(nc->net == 0);
  const auto hole = std::find_if(b.pads.begin(), b.pads.end(), [](const model::Pad& p) { return p.type == model::PadType::NpThruHole; });
  REQUIRE(hole != b.pads.end());
  CHECK(hole->pos == geom::Point{0, -6'000'000});
  CHECK(hole->drill_x == 1'000'000);
  // The point on bare board got a round pad of its connection's width.
  const auto* t = pad_at(b, -3, 7, 0);
  REQUIRE(t);
  CHECK(t->net == b.net_by_name("C1"));
  CHECK(t->size_x == 300'000);
  // The pour is an unfilled zone of its net.
  REQUIRE(b.zones.size() == 1);
  CHECK(b.zones[0].net == b.net_by_name("A"));
  CHECK(b.zones[0].copper == model::layer_bit(2));
  CHECK(b.zones[0].fills.empty());
  CHECK_FALSE(b.zones[0].rule_area);
  // The outline polygon, not the bounds: the notch at the top left is cut.
  const auto bb = b.edge_bbox();
  CHECK(bb.x0 == -10'000'000);
  CHECK(bb.y0 == -10'000'000);
  CHECK(bb.x1 == 10'000'000);
  const auto edge = std::find_if(b.graphics.begin(), b.graphics.end(), [](const model::Graphic& g) { return g.layer == "Edge.Cuts"; });
  REQUIRE(edge != b.graphics.end());
  CHECK(edge->pts.size() == 6);
  CHECK(std::find(edge->pts.begin(), edge->pts.end(), geom::Point{0, -8'000'000}) != edge->pts.end());
}

TEST_CASE("SRJ to KiCad: the project carries the problem's rules, no more and no less", "[srj]") {
  const auto problem = io::read_srj(mixed_text());
  const fs::path dir = fs::temp_directory_path() / "tm_srj_rules_test";
  fs::create_directories(dir);
  auto write = [&](const io::SrjBoardFiles& f) {
    std::ofstream(dir / "b.kicad_pcb", std::ios::binary) << f.pcb;
    std::ofstream(dir / "b.kicad_pro", std::ios::binary) << f.pro;
    fs::remove(dir / "b.kicad_dru");
    if (!f.dru.empty()) std::ofstream(dir / "b.kicad_dru", std::ios::binary) << f.dru;
    return io::read_design_rules((dir / "b.kicad_pcb").string());
  };
  auto r = write(io::srj_to_kicad(problem));
  CHECK(r.warnings.empty());
  CHECK(r.minimums.clearance == 200'000);
  CHECK(r.minimums.track_width == 150'000);
  CHECK(r.minimums.via_diameter == 600'000);
  CHECK(r.minimums.through_hole_diameter == 300'000);
  CHECK(r.minimums.via_annular_width == 150'000);
  CHECK(r.minimums.hole_clearance == 250'000);
  CHECK(r.minimums.hole_to_hole == 400'000);
  CHECK(r.minimums.copper_edge_clearance == 300'000);
  CHECK(r.minimums.allow_blind_buried_vias);
  CHECK_FALSE(r.minimums.allow_microvias);
  CHECK(r.default_class().clearance == 200'000);
  CHECK(r.default_class().track_width == 200'000);
  CHECK(r.default_class().via_diameter == 600'000);
  CHECK(r.default_class().via_drill == 300'000);
  // The net with a 0.3 mm connection routes at 0.3 mm; the others at the nominal width.
  CHECK(r.class_for("C1").track_width == 300'000);
  CHECK(r.class_for("A").track_width == 200'000);
  // Trace-to-pad clearance differs from the general one: a custom rule.
  REQUIRE(r.custom.size() == 1);
  CHECK(r.custom[0].constraints.at(0).type == "clearance");
  CHECK(r.custom[0].constraints.at(0).min == 250'000);

  // Options replace what the problem says; a problem that says nothing gets tscircuit's fallback and no rule file.
  json d = json::parse(mixed_text());
  for (const char* k : {"defaultObstacleMargin", "minTraceToPadEdgeClearance", "minTraceToHoleEdgeClearance", "minBoardEdgeClearance",
                        "minViaHoleEdgeToViaHoleEdgeClearance", "allowBlindAndBuriedVias"})
    d.erase(k);
  const auto bare = io::read_srj(d.dump());
  r = write(io::srj_to_kicad(bare));
  CHECK(r.minimums.clearance == 150'000);
  CHECK(r.minimums.hole_clearance == 0);
  CHECK(r.minimums.hole_to_hole == 0);
  CHECK(r.minimums.copper_edge_clearance == 0);
  CHECK_FALSE(r.minimums.allow_blind_buried_vias);
  CHECK(r.custom.empty());
  io::SrjImportOptions opt;
  opt.clearance = 100'000;
  opt.hole_to_hole = 100'000;
  r = write(io::srj_to_kicad(bare, opt));
  CHECK(r.minimums.clearance == 100'000);
  CHECK(r.default_class().clearance == 100'000);
  CHECK(r.minimums.hole_to_hole == 100'000);
  fs::remove_all(dir);
}

TEST_CASE("SRJ to KiCad: one layer, a rectangular hole, a point on a foreign obstacle", "[srj]") {
  const auto p = io::read_srj(R"({"layerCount": 1, "minTraceWidth": 0.2, "bounds": {"minX": 0, "maxX": 10, "minY": 0, "maxY": 5},
    "obstacles": [
      {"type": "rect", "layers": ["top"], "center": {"x": 2, "y": 2}, "width": 1, "height": 1, "connectedTo": ["n"]},
      {"type": "rect", "isNonPlatedHole": true, "layers": ["top"], "center": {"x": 5, "y": 2}, "width": 2, "height": 1, "connectedTo": []},
      {"type": "rect", "layers": ["top"], "center": {"x": 8, "y": 2}, "width": 1, "height": 1, "connectedTo": ["other"]}],
    "connections": [{"name": "n", "pointsToConnect": [{"x": 2, "y": 2, "layer": "top"}, {"x": 8.2, "y": 2.1, "layer": "bottom"}]}]})");
  const auto files = io::srj_to_kicad(p);
  const auto lb = load(files.pcb);
  // KiCad has two copper layers at least; the second is closed by a rule area. The slot gets a keep-out too.
  CHECK(lb.board.copper_count() == 2);
  int areas = 0;
  for (const auto& z : lb.board.zones) areas += z.rule_area && z.keepout_tracks && z.keepout_vias;
  CHECK(areas == 2);
  const auto hole = std::find_if(lb.board.pads.begin(), lb.board.pads.end(), [](const model::Pad& q) { return q.type == model::PadType::NpThruHole; });
  REQUIRE(hole != lb.board.pads.end());
  CHECK(hole->drill_oval);
  CHECK(hole->drill_x == 2'000'000);
  CHECK(hole->drill_y == 1'000'000);
  // The second point lies on an obstacle that shares nothing with it (off its centre, so tscircuit's position key
  // does not join them either): it gets its own pad and the importer says so.
  CHECK(files.terminal_pads == 1);
  CHECK(std::any_of(files.warnings.begin(), files.warnings.end(), [](const std::string& w) { return w.find("not connected to them") != std::string::npos; }));
}

TEST_CASE("SRJ export: traces follow the routed copper and unrouted connections are named", "[srj]") {
  const std::string text = mixed_text();
  auto lb = load(io::srj_to_kicad(io::read_srj(text)).pcb);
  const model::NetId na = lb.board.net_by_name("A"), nb = lb.board.net_by_name("B"), nc = lb.board.net_by_name("C1");
  io::BoardEditor ed(lb);
  auto track = [&](double x0, double y0, double x1, double y1, int layer, model::NetId net) {
    model::Track t;
    t.a = {mm_to_nm(x0), -mm_to_nm(y0)};  // SRJ coordinates in, KiCad's on the board
    t.b = {mm_to_nm(x1), -mm_to_nm(y1)};
    t.width = 200'000;
    t.layer = layer;
    t.net = net;
    ed.add_track(t);
  };
  // A: one straight track pad to pad, and a stub that leaves it in the middle (a T junction and loose copper).
  track(-6, 4, 6, 4, 0, na);
  track(0, 4, 0, 5, 0, na);
  // B: from the plated hole on an inner layer to a blind via, on to the bottom pad.
  track(-6, -4, 0, -4, 1, nb);
  track(0, -4, 6, -4, 3, nb);
  model::Via v;
  v.pos = {0, 4'000'000};
  v.size = 600'000;
  v.drill = 300'000;
  v.layer_top = 1;
  v.layer_bottom = 3;
  v.type = model::ViaType::Blind;
  v.net = nb;
  ed.add_via(v);
  // C1 is routed, C2 is not.
  track(0, -6, 3, -7, 0, nc);
  const model::Board routed = io::read_board(lb.doc);
  REQUIRE(routed.tracks.size() == 5);

  const auto res = io::srj_export(text, routed);
  CHECK(res.connections == 4);
  CHECK(res.routed == 3);
  CHECK(res.unrouted == std::vector<std::string>{"C2"});
  CHECK(res.traces == 3);
  CHECK(res.extra_traces == 1);
  CHECK(res.vias == 1);
  CHECK(res.warnings.empty());
  CHECK(io::srj_export(text, routed).json == res.json);

  const json out = json::parse(res.json);
  // The problem is kept as it was; only `traces` is new.
  json expect = json::parse(text);
  json without = out;
  without.erase("traces");
  CHECK(without == expect);
  REQUIRE(out["traces"].size() == 4);
  auto trace = [&](const std::string& id) -> const json& {
    for (const auto& t : out["traces"])
      if (t["pcb_trace_id"] == id) return t;
    FAIL("no trace " << id);
    return out;
  };
  const json& a = trace("A")["route"];
  REQUIRE(a.size() == 3);  // cut where the stub joins
  CHECK(a[0]["x"] == -6.0);
  CHECK(a[0]["y"] == 4.0);
  CHECK(a[1]["x"] == 0.0);
  CHECK(a[2]["x"] == 6.0);
  CHECK(a[2]["layer"] == "top");
  CHECK(a[0]["width"] == 0.2);
  const json& stub = trace("A_extra_0");
  CHECK(stub["connection_name"] == "A");
  CHECK(stub["route"].size() == 2);
  const json& br = trace("B")["route"];
  REQUIRE(br.size() == 5);
  CHECK(br[0]["layer"] == "inner1");
  CHECK(br[0]["y"] == -4.0);
  CHECK(br[2]["route_type"] == "via");
  CHECK(br[2]["from_layer"] == "inner1");
  CHECK(br[2]["to_layer"] == "bottom");
  CHECK(br[2]["layers"] == json::array({"inner1", "inner2", "bottom"}));
  CHECK(br[2]["via_diameter"] == 0.6);
  CHECK(br[2]["via_hole_diameter"] == 0.3);
  CHECK(br[3]["layer"] == "bottom");
  CHECK(br[4]["x"] == 6.0);

  // Every wire point lies on a track of its net and layer, or inside a pad of its net.
  for (const auto& t : out["traces"]) {
    const model::NetId net = routed.net_by_name(t["connection_name"].get<std::string>());
    REQUIRE(net > 0);
    for (const auto& r : t["route"]) {
      if (r["route_type"] != "wire") continue;
      const geom::Point q{mm_to_nm(r["x"].get<double>()), -mm_to_nm(r["y"].get<double>())};
      const int layer = io::srj_layer_index(r["layer"].get<std::string>(), 4);
      bool on = false;
      for (const auto& k : routed.tracks) on = on || (k.net == net && k.layer == layer && geom::point_seg_closer(q, k.a, k.b, 2));
      for (const auto& pad : routed.pads) on = on || (pad.net == net && (pad.copper & model::layer_bit(layer)) && inside(pad, q));
      CHECK(on);
    }
  }
}

TEST_CASE("SRJ export refuses a board that is not the problem's", "[srj]") {
  const std::string text = mixed_text();
  json d = json::parse(text);
  d["layerCount"] = 2;
  for (auto& o : d["obstacles"]) o["layers"] = {"top"};
  for (auto& c : d["connections"])
    for (auto& q : c["pointsToConnect"]) {
      q.erase("layers");
      q["layer"] = "top";
    }
  const auto two = load(io::srj_to_kicad(io::read_srj(d.dump())).pcb);
  CHECK_THROWS(io::srj_export(text, two.board));
}

TEST_CASE("SRJ export joins copper that touches, as KiCad's connectivity does", "[srj]") {
  // Two pads; the copper between them is in three tracks that meet nowhere at an end point: the second starts
  // beside the first one's end, inside its width, and the third crosses the second in the middle of both.
  const std::string text = R"({"layerCount": 2, "minTraceWidth": 0.2, "bounds": {"minX": -5, "maxX": 5, "minY": -5, "maxY": 5},
    "obstacles": [
      {"type": "rect", "layers": ["top"], "center": {"x": -4.0000000000000001, "y": 0.30000000000000004}, "width": 0.6, "height": 0.6, "connectedTo": ["n"]},
      {"type": "rect", "layers": ["top"], "center": {"x": 4, "y": 2}, "width": 0.6, "height": 0.6, "connectedTo": ["n"]}],
    "connections": [{"name": "n", "pointsToConnect": [{"x": -4.0000000000000001, "y": 0.30000000000000004, "layer": "top"}, {"x": 4, "y": 2, "layer": "top"}]}]})";
  auto lb = load(io::srj_to_kicad(io::read_srj(text)).pcb);
  const model::NetId n = lb.board.net_by_name("n");
  io::BoardEditor ed(lb);
  auto track = [&](double x0, double y0, double x1, double y1) {
    model::Track t;
    t.a = {mm_to_nm(x0), -mm_to_nm(y0)};
    t.b = {mm_to_nm(x1), -mm_to_nm(y1)};
    t.width = 200'000;
    t.layer = 0;
    t.net = n;
    ed.add_track(t);
  };
  track(-4, 0.3, 0, 0.3);
  track(0.05, 0.38, 2, 0.38);  // 0.094 mm from the end of the first track: the copper overlaps
  track(1, -1, 1, 2);          // crosses the second track
  track(1, 2, 4, 2);
  const model::Board routed = io::read_board(lb.doc);
  const auto res = io::srj_export(text, routed);
  CHECK(res.routed == 1);
  CHECK(res.unrouted.empty());
  const json out = json::parse(res.json);
  const json* route = nullptr;
  for (const auto& t : out["traces"])
    if (t["pcb_trace_id"] == "n") route = &t["route"];
  REQUIRE(route);
  // The trace starts with the problem's own digits and has no two points closer than a nanometre.
  CHECK((*route)[0]["x"] == -4.0000000000000001);
  CHECK((*route)[0]["y"] == 0.30000000000000004);
  CHECK(route->back()["x"] == 4.0);
  for (std::size_t i = 1; i < route->size(); ++i) {
    const double dx = (*route)[i]["x"].get<double>() - (*route)[i - 1]["x"].get<double>();
    const double dy = (*route)[i]["y"].get<double>() - (*route)[i - 1]["y"].get<double>();
    CHECK(std::hypot(dx, dy) >= 1e-6);
  }
  // It passes through the crossing point, which is the end of no track.
  CHECK(std::any_of(route->begin(), route->end(), [](const json& p) { return p["x"] == 1.0 && p["y"] == 0.38; }));
  // The parts beyond the joins are copper too: the tail of the second track and the foot of the third meet at the
  // crossing and leave as one more trace.
  CHECK(res.extra_traces == 1);
}

// Fan-out problems (doc 16 Track B): plane terminations become zones, breakout points leave on the layer their bus
// names, the far component's pads are keep-outs, closed layers get a rule area, the outline can grow.
TEST_CASE("SRJ fan-out extensions: planes, exit layers, unpointed pads, track layers, margin", "[srj]") {
  const json p = {
      {"bounds", {{"minX", -5}, {"maxX", 5}, {"minY", -5}, {"maxY", 5}}},
      {"layerCount", 4},
      {"minTraceWidth", 0.1},
      {"tracemakerTrackLayers", json::array({"top", "bottom"})},
      {"obstacles",
       json::array({
           // ball with a connection point, ball that is a plane drop, far-component pad with the same net as the ball
           {{"type", "rect"}, {"layers", {"top"}}, {"center", {{"x", 0}, {"y", 0}}}, {"width", 0.3}, {"height", 0.3}, {"connectedTo", {"n1"}}},
           {{"type", "rect"}, {"layers", {"top"}}, {"center", {{"x", 1}, {"y", 0}}}, {"width", 0.3}, {"height", 0.3}, {"connectedTo", {"gnd"}}},
           {{"type", "rect"}, {"layers", {"top"}}, {"center", {{"x", 3}, {"y", 3}}}, {"width", 0.3}, {"height", 0.3}, {"connectedTo", {"n1"}}},
       })},
      {"connections",
       json::array({
           {{"name", "n1"}, {"pointsToConnect", json::array({{{"x", 0}, {"y", 0}, {"layer", "top"}}, {{"x", -5}, {"y", 1}, {"layer", "top"}, {"pointId", "pcb_breakout_point_0"}}})}},
           {{"name", "gnd"}, {"pointsToConnect", json::array({{{"x", 1}, {"y", 0}, {"layer", "top"}}})}},
       })},
      {"buses",
       json::array({
           {{"busId", "A"}, {"connectionNames", {"n1"}}, {"connectionExitTargets", {{"n1", {{"x", -6}, {"y", 1}, {"layer", "inner1"}}}}}},
           {{"busId", "G"}, {"connectionNames", {"gnd"}}, {"termination", {{"type", "plane"}, {"layer", "inner2"}}}},
       })}};
  const auto problem = io::read_srj(p.dump());
  REQUIRE(problem.planes.size() == 1);
  CHECK(problem.planes[0].layer == 2);
  CHECK(problem.connections[0].points.back().layers == std::vector<int>{0});  // the breakout point keeps its own layer (the exit-layer override is an experiment, D98)
  CHECK(problem.track_layers == std::vector<int>{0, 3});

  io::SrjImportOptions opt;
  opt.outline_margin = mm_to_nm(0.5);
  opt.unpointed_keepout = true;
  const auto files = io::srj_to_kicad(problem, opt);
  const auto lb = load(files.pcb);
  // The far pad at (3,3) is not a pad of net n1: nothing lies on it, so it is a keep-out without a net.
  const auto* far = pad_at(lb.board, 3, -3, 0);
  REQUIRE(far != nullptr);
  CHECK(far->net == 0);
  const auto* ball = pad_at(lb.board, 0, 0, 0);
  REQUIRE(ball != nullptr);
  CHECK(ball->net != 0);
  // Zones: the plane of the gnd net on inner2 and the rule area closing inner1 and inner2 to tracks.
  CHECK(files.pcb.find("srj_plane_2") != std::string::npos);
  CHECK(files.pcb.find("srj_no_tracks") != std::string::npos);
  CHECK(files.pcb.find("(vias allowed)") != std::string::npos);
  // The outline grew by the margin.
  CHECK(files.pcb.find("(start -5.5 -5.5)") != std::string::npos);
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Generated pours and the copper layer count (doc 05 §53, M20 C3): the document edits, the pour planner, and a route that
// adds a pour and writes it filled.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "app/route_job.hpp"
#include "drc/drc.hpp"
#include "drc/refill.hpp"
#include "io/kicad/board_editor.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "passes/pour.hpp"

using namespace tmk;
namespace fs = std::filesystem;

namespace {

// 30 x 20 mm, two copper layers, KiCad 5 numbering and syntax. GND: two through-hole pads and an F.Cu pad; S: two F.Cu pads.
const std::string kTwoLayer = R"((kicad_pcb (version 20171130) (host pcbnew 5.1.6)
  (general (thickness 1.6))
  (layers
    (0 F.Cu signal)
    (31 B.Cu signal)
    (44 Edge.Cuts user)
  )
  (setup (pad_to_mask_clearance 0))
  (net 0 "")
  (net 1 GND)
  (net 2 S)
  (module T:H (layer F.Cu) (at 4 4)
    (pad 1 thru_hole circle (at 0 0) (size 1.6 1.6) (drill 0.8) (layers *.Cu *.Mask) (net 1 GND)))
  (module T:H (layer F.Cu) (at 26 16)
    (pad 1 thru_hole circle (at 0 0) (size 1.6 1.6) (drill 0.8) (layers *.Cu *.Mask) (net 1 GND)))
  (module T:P (layer F.Cu) (at 15 4)
    (pad 1 smd rect (at 0 0) (size 1 1) (layers F.Cu F.Paste F.Mask) (net 1 GND)))
  (module T:P (layer F.Cu) (at 5 10)
    (pad 1 smd rect (at 0 0) (size 1 1) (layers F.Cu F.Paste F.Mask) (net 2 S)))
  (module T:P (layer F.Cu) (at 25 10)
    (pad 1 smd rect (at 0 0) (size 1 1) (layers F.Cu F.Paste F.Mask) (net 2 S)))
  (gr_line (start 0 0) (end 30 0) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 30 0) (end 30 20) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 30 20) (end 0 20) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 0 20) (end 0 0) (layer Edge.Cuts) (width 0.1))
)
)";

// KiCad 9 numbering (B.Cu = 2), four layers, a stackup, and a zone on In1.Cu.
const std::string kFourLayer = R"((kicad_pcb (version 20241229) (generator "pcbnew")
	(general (thickness 1.6))
	(layers
		(0 "F.Cu" signal)
		(4 "In1.Cu" signal)
		(6 "In2.Cu" signal)
		(2 "B.Cu" signal)
		(25 "Edge.Cuts" user)
	)
	(setup
		(stackup
			(layer "F.Cu" (type "copper") (thickness 0.035))
			(layer "dielectric 1" (type "core") (thickness 1.5))
			(layer "B.Cu" (type "copper") (thickness 0.035))
		)
		(pad_to_mask_clearance 0)
	)
	(net 0 "")
	(net 1 "GND")
	(footprint "T:H" (layer "F.Cu") (at 4 4)
		(pad "1" thru_hole circle (at 0 0) (size 1.6 1.6) (drill 0.8) (layers "*.Cu") (net 1 "GND")))
	(zone (net 1) (net_name "GND") (layer "In1.Cu") (hatch edge 0.5) (connect_pads (clearance 0.2)) (min_thickness 0.25)
		(fill (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon (pts (xy 1 1) (xy 29 1) (xy 29 19) (xy 1 19))))
	(gr_rect (start 0 0) (end 30 20) (stroke (width 0.1) (type default)) (fill none) (layer "Edge.Cuts"))
)
)";

io::LoadedBoard load(const std::string& text) {
  io::LoadedBoard lb{sexpr::Document::parse(text), {}};
  lb.board = io::read_board(lb.doc);
  return lb;
}

io::LoadedBoard reparse(const io::LoadedBoard& lb) { return load(lb.doc.write()); }

struct Dir {
  fs::path dir;
  explicit Dir(const std::string& name) {
    dir = fs::temp_directory_path() / ("tmk_pour_" + name + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  ~Dir() { fs::remove_all(dir); }
};

}  // namespace

TEST_CASE("Copper layer count: 2 -> 4 -> 2 restores the file byte for byte (KiCad 5 numbering)", "[pour][layers]") {
  auto lb = load(kTwoLayer);
  {
    io::BoardEditor ed(lb);
    std::string why;
    REQUIRE(ed.set_copper_layer_count(4, &why));
  }
  auto four = reparse(lb);
  REQUIRE(four.board.copper_count() == 4);
  CHECK(four.board.copper_name(1) == "In1.Cu");
  CHECK(four.board.copper_name(2) == "In2.Cu");
  CHECK(four.board.layers[static_cast<std::size_t>(four.board.copper[1])].ordinal == 1);
  CHECK(four.board.layers[static_cast<std::size_t>(four.board.copper[2])].ordinal == 2);
  CHECK(four.board.pads[0].copper == 0b1111);  // plated holes reach every copper layer
  CHECK(four.board.pads[2].copper == 0b0001);
  CHECK(four.doc.write().find("    (1 In1.Cu signal)\n    (2 In2.Cu signal)\n    (31 B.Cu signal)") != std::string::npos);
  {
    io::BoardEditor ed(four);
    REQUIRE(ed.set_copper_layer_count(2));
  }
  CHECK(four.doc.write() == kTwoLayer);
}

TEST_CASE("Copper layer count: KiCad 9 numbering, stackup removed, layers with copper kept", "[pour][layers]") {
  auto lb = load(kFourLayer);
  {
    io::BoardEditor ed(lb);
    std::string why;
    REQUIRE_FALSE(ed.set_copper_layer_count(2, &why));  // the zone on In1.Cu
    CHECK(why.find("zone") != std::string::npos);
    CHECK_FALSE(lb.doc.modified());
    REQUIRE_FALSE(ed.set_copper_layer_count(5, &why));
  }
  {
    io::BoardEditor ed(lb);
    std::vector<std::string> notes;
    REQUIRE(ed.set_copper_layer_count(6, nullptr, &notes));
    CHECK(notes.size() == 1);
  }
  const auto six = reparse(lb);
  REQUIRE(six.board.copper_count() == 6);
  CHECK(six.board.copper_name(3) == "In3.Cu");
  CHECK(six.board.layers[static_cast<std::size_t>(six.board.copper[3])].ordinal == 8);
  CHECK(six.board.layers[static_cast<std::size_t>(six.board.copper[4])].ordinal == 10);
  CHECK_FALSE(six.board.stackup.present);
  CHECK(six.board.zones.size() == 1);
  CHECK(six.board.zones[0].copper == model::layer_bit(1));
}

TEST_CASE("A zone written by the editor reads back with its settings and fills", "[pour]") {
  auto lb = load(kTwoLayer);
  model::Zone z;
  z.net = 1;
  z.copper = model::layer_bit(1);
  z.name = "tracemaker pour";
  z.clearance = 300'000;
  z.min_thickness = 250'000;
  z.connect = model::ZoneConnect::Full;
  z.outline.push_back({{0, 0}, {30'000'000, 0}, {30'000'000, 20'000'000}, {0, 20'000'000}});
  {
    io::BoardEditor ed(lb);
    ed.add_zone(z);
  }
  auto a = reparse(lb);
  REQUIRE(a.board.zones.size() == 1);
  const auto& r = a.board.zones[0];
  CHECK(r.net == 1);
  CHECK(r.copper == model::layer_bit(1));
  CHECK(r.name == "tracemaker pour");
  CHECK(r.clearance == 300'000);
  CHECK(r.connect == model::ZoneConnect::Full);
  CHECK(r.thermal_gap == 500'000);
  CHECK(r.outline.front() == z.outline.front());
  CHECK(r.fills.empty());
  CHECK(a.doc.write().find("(net_name \"GND\")") != std::string::npos);  // numbered nets: KiCad 5 zones name their net too
  const std::vector<std::pair<int, std::vector<model::Point>>> fills = {{1, {{1'000'000, 1'000'000}, {29'000'000, 1'000'000}, {29'000'000, 19'000'000}}},
                                                                         {1, {{2'000'000, 2'000'000}, {3'000'000, 2'000'000}, {3'000'000, 3'000'000}}}};
  {
    io::BoardEditor ed(a);
    ed.set_zone_fills(0, fills);
  }
  auto b = reparse(a);
  CHECK(b.board.zones[0].fills == fills);
  CHECK(b.doc.write().find("(fill yes") != std::string::npos);
  {
    io::BoardEditor ed(b);
    ed.set_zone_fills(0, {fills[0]});
  }
  CHECK(reparse(b).board.zones[0].fills.size() == 1);  // replaced, not added to
}

TEST_CASE("Pour planner: auto picks the ground net and a free layer; NET@LAYER as given", "[pour]") {
  Dir d("plan");
  std::ofstream(d.dir / "b.kicad_pcb") << kTwoLayer;
  const auto two = load(kTwoLayer);
  const model::DesignRules rules = io::read_design_rules((d.dir / "b.kicad_pcb").string());  // no project: KiCad's defaults
  std::vector<std::string> notes;
  auto p = passes::plan_pours(two.board, rules, {"auto"}, &notes);
  REQUIRE(p.size() == 1);
  CHECK(p[0].zone.net == two.board.net_by_name("GND"));
  CHECK(p[0].zone.copper == model::layer_bit(1));  // two layers: the back
  CHECK(p[0].zone.clearance == 300'000);           // Default class 0.2 mm, at least 0.3 mm
  CHECK(p[0].zone.connect == model::ZoneConnect::Full);  // solid: no thermal spokes for routed copper to cut off
  p = passes::plan_pours(two.board, rules, {"S@F.Cu"});
  REQUIRE(p.size() == 1);
  CHECK(p[0].zone.net == two.board.net_by_name("S"));
  CHECK(p[0].zone.copper == model::layer_bit(0));
  CHECK_THROWS(passes::plan_pours(two.board, rules, {"X@F.Cu"}));
  CHECK_THROWS(passes::plan_pours(two.board, rules, {"GND@In1.Cu"}));
  CHECK_THROWS(passes::plan_pours(two.board, rules, {"GND"}));
  CHECK_THROWS(passes::plan_pours(two.board, rules, {"GND@F.Cu", "S@F.Cu"}));

  auto lb = load(kTwoLayer);
  {
    io::BoardEditor ed(lb);
    REQUIRE(ed.set_copper_layer_count(4));
  }
  p = passes::plan_pours(reparse(lb).board, rules, {"auto"});
  REQUIRE(p.size() == 1);
  CHECK(p[0].zone.copper == model::layer_bit(1));  // four layers: the first inner one

  notes.clear();
  CHECK(passes::plan_pours(load(kFourLayer).board, rules, {"auto"}, &notes).empty());  // it has a zone
  CHECK(notes.size() == 1);
}

TEST_CASE("route --pour auto writes a filled ground pour and leaves nothing unconnected", "[pour][route]") {
  Dir d("route");
  const fs::path in = d.dir / "b.kicad_pcb", out = d.dir / "out.kicad_pcb";
  std::ofstream(in) << kTwoLayer;
  app::RouteJob job;
  job.in = in.string();
  job.out = out.string();
  job.opt.work_budget = 2'000'000;
  job.variants = 1;
  job.threads = 1;
  job.pours = {"auto"};
  const auto r = app::run_route_job(job);
  CHECK(r.exit_code() == 0);
  CHECK(r.unconnected_after_refill == 0);
  REQUIRE(r.summary.contains("pours"));
  CHECK(r.summary["pours"][0]["net"] == "GND");
  CHECK(r.summary["pours"][0]["layer"] == "B.Cu");
  const auto written = io::read_board_file(out.string());
  REQUIRE(written.board.zones.size() == 1);
  CHECK_FALSE(written.board.zones[0].fills.empty());
  // The written fills are the engine's refill of the written board: judging them as they are or refilled gives the same.
  const auto rules = io::read_design_rules(out.string());
  drc::DrcOptions o;
  o.connectivity_only = true;
  o.dangling = false;
  CHECK(drc::run_drc(written.board, rules, o).unconnected.empty());
  const auto refilled = drc::refill_zones(written.board, rules).board;
  CHECK(refilled.zones[0].fills == written.board.zones[0].fills);
}

// A zone without a fill (doc 05 §54): filled in memory and routed with soft zones, so the GND pads use the plane, and written as it was.
TEST_CASE("route: a zone without a fill is filled for routing, so its net drops vias into the plane", "[pour][route]") {
  Dir d("planes");
  const fs::path in = d.dir / "b.kicad_pcb", out = d.dir / "out.kicad_pcb";
  std::string text = kTwoLayer;
  const std::string zone =
      "  (zone (net 1) (net_name GND) (layer B.Cu) (hatch edge 0.508) (connect_pads (clearance 0.3)) (min_thickness 0.25)\n"
      "    (fill yes (arc_segments 32) (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon (pts (xy 0 0) (xy 30 0) (xy 30 20) (xy 0 20))))\n";
  text.insert(text.find("  (gr_line"), zone);
  std::ofstream(in) << text;
  auto gnd_track_mm = [&](const route::RouteResult& r) {
    double mm = 0;
    for (const auto& t : r.tracks)
      if (t.net == 1) mm += std::hypot(static_cast<double>(t.b.x - t.a.x), static_cast<double>(t.b.y - t.a.y)) / 1e6;
    return mm;
  };
  app::RouteJob job;
  job.in = in.string();
  job.out = out.string();
  job.opt.work_budget = 2'000'000;
  job.variants = 1;
  job.threads = 1;
  const auto filled = app::run_route_job(job);
  CHECK(filled.exit_code() == 0);
  CHECK(filled.unconnected_after_refill == 0);  // soft zones: judged after a refill
  const auto written = io::read_board_file(out.string());
  REQUIRE(written.board.zones.size() == 1);
  CHECK(written.board.zones[0].fills.empty());  // the zone is written as it was: KiCad fills it
  job.fill_planes = false;
  const auto plain = app::run_route_job(job);
  CHECK(plain.unconnected_after_refill == -1);  // no soft zones
  CHECK(gnd_track_mm(filled.result) + 5 < gnd_track_mm(plain.result));  // tracks round the board without the plane
}

TEST_CASE("Layer advisor: counts the board cannot take are skipped, the fewest that completes is written", "[pour][layers][route]") {
  Dir d("advisor");
  const fs::path in = d.dir / "b.kicad_pcb", out = d.dir / "out.kicad_pcb";
  std::ofstream(in) << kTwoLayer;
  app::RouteJob job;
  job.in = in.string();
  job.out = out.string();
  job.opt.work_budget = 1'000'000;
  job.variants = 1;
  job.threads = 1;
  const auto r = app::run_layer_advisor(job, {4, 2});
  CHECK(r.exit_code() == 0);
  CHECK(r.summary["layers"]["chosen"] == 2);
  CHECK(r.summary["layers"]["tried"].size() == 1);  // 2 completes: 4 is not tried
  CHECK(io::read_board_file(out.string()).board.copper_count() == 2);
  CHECK_FALSE(fs::exists(out.string() + ".2-layers.tmp"));

  std::ofstream(in) << kFourLayer;
  const auto r4 = app::run_layer_advisor(job, {2, 4});
  CHECK(r4.summary["layers"]["tried"][0]["tried"] == false);  // the zone on In1.Cu
  CHECK(r4.summary["layers"]["chosen"] == 4);
}

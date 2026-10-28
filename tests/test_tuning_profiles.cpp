// SPDX-License-Identifier: GPL-3.0-or-later
// KiCad 10 tuning profiles (doc 05 §58, D145): read from the project, compiled into KiCad's implicit width rules, and routed with each
// layer's profile width.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "app/route_job.hpp"
#include "drc/drc.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"

using namespace tmk;
namespace fs = std::filesystem;

namespace {

// 30 x 20 mm, four copper layers (KiCad 9 numbering), no stackup. CLK joins an F.Cu pad and a B.Cu pad; a keepout across the middle of the
// outer layers makes it cross on an inner one.
const std::string kBoard = R"((kicad_pcb (version 20241229) (generator "pcbnew")
	(general (thickness 1.6))
	(layers
		(0 "F.Cu" signal)
		(4 "In1.Cu" signal)
		(6 "In2.Cu" signal)
		(2 "B.Cu" signal)
		(25 "Edge.Cuts" user)
	)
	(setup (pad_to_mask_clearance 0))
	(net 0 "")
	(net 1 "CLK")
	(footprint "T:P" (layer "F.Cu") (at 5 10)
		(pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "CLK")))
	(footprint "T:P" (layer "B.Cu") (at 25 10)
		(pad "1" smd rect (at 0 0) (size 1 1) (layers "B.Cu") (net 1 "CLK")))
	(zone (net 0) (net_name "") (layers "F.Cu" "B.Cu") (name "wall") (hatch edge 0.5) (connect_pads (clearance 0))
		(min_thickness 0.25) (keepout (tracks not_allowed) (vias allowed) (pads allowed) (copperpour allowed) (footprints allowed))
		(fill (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon (pts (xy 13 0) (xy 17 0) (xy 17 20) (xy 13 20))))
	(gr_rect (start 0 0) (end 30 20) (stroke (width 0.1) (type default)) (fill none) (layer "Edge.Cuts"))
)
)";

// The project: net class FAST (CLK) with profile MS, 0.15 mm on the outer layers and 0.1 mm inside (microstrip and stripline), in
// KiCad 10's layout (qa/data/pcbnew/time_calculations.kicad_pro). `severity` is that of tuning_profile_track_geometries.
std::string project(const std::string& severity) {
  return R"({
  "board": {"design_settings": {"rules": {"min_track_width": 0.1, "min_clearance": 0.15},
                                "rule_severities": {"tuning_profile_track_geometries": ")" + severity + R"("}}},
  "net_settings": {"classes": [{"name": "Default", "track_width": 0.25, "clearance": 0.2, "via_diameter": 0.6, "via_drill": 0.3, "tuning_profile": ""},
                               {"name": "FAST", "priority": 0, "tuning_profile": "MS"}],
                   "netclass_patterns": [{"pattern": "CLK", "netclass": "FAST"}]},
  "tuning_profiles": {"meta": {"version": 0}, "tuning_profiles_impedance_geometric": [
    {"profile_name": "MS", "type": 0, "target_impedance": 50.0, "enable_time_domain_tuning": false, "via_prop_delay": 0, "via_overrides": [],
     "layer_entries": [
       {"signal_layer": "F.Cu", "top_reference_layer": "UNDEFINED", "bottom_reference_layer": "In1.Cu", "width": 150000, "diff_pair_gap": 0, "delay": 0},
       {"signal_layer": "In1.Cu", "top_reference_layer": "F.Cu", "bottom_reference_layer": "In2.Cu", "width": 100000, "diff_pair_gap": 0, "delay": 0},
       {"signal_layer": "In2.Cu", "top_reference_layer": "In1.Cu", "bottom_reference_layer": "B.Cu", "width": 100000, "diff_pair_gap": 0, "delay": 0},
       {"signal_layer": "B.Cu", "top_reference_layer": "In2.Cu", "bottom_reference_layer": "UNDEFINED", "width": 150000, "diff_pair_gap": 0, "delay": 0}]}]}
})";
}

struct Dir {
  fs::path dir;
  explicit Dir(const std::string& name) : dir(fs::temp_directory_path() / ("tmk_tuning_" + name + "_" + std::to_string(::getpid()))) {
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  ~Dir() { fs::remove_all(dir); }
};

int track_width_errors(const model::Board& b, const model::DesignRules& r) {
  int n = 0;
  for (const auto& v : drc::run_drc(b, r).violations) n += v.type == "track_width" && v.severity == "error";
  return n;
}

}  // namespace

TEST_CASE("tuning profiles: read from the project and compiled into KiCad's width rules, with its severity", "[tuning]") {
  Dir d("read");
  const fs::path pcb = d.dir / "b.kicad_pcb";
  std::ofstream(pcb) << kBoard;
  std::ofstream(d.dir / "b.kicad_pro") << project("error");
  auto r = io::read_design_rules(pcb.string());
  REQUIRE(r.tuning_profiles.size() == 1);
  const auto& p = r.tuning_profiles[0];
  CHECK(p.name == "MS");
  CHECK_FALSE(p.differential);
  CHECK(p.width_on("F.Cu") == 150'000);
  CHECK(p.width_on("In1.Cu") == 100'000);
  REQUIRE(r.find_class("FAST"));
  CHECK(r.profile_of(*r.find_class("FAST")) == &p);
  CHECK(r.custom.size() == 4);  // one exact-width rule per layer
  // A 0.25 mm CLK track on F.Cu breaks the profile's width there; a 0.15 mm one does not.
  auto b = io::read_board_file(pcb.string()).board;
  b.tracks.push_back({{5'000'000, 10'000'000}, {10'000'000, 10'000'000}, 250'000, 0, 1, false, sexpr::kNoNode});
  CHECK(track_width_errors(b, r) == 1);
  b.tracks.back().width = 150'000;
  CHECK(track_width_errors(b, r) == 0);
  // KiCad's default severity: the rules are there but ignored, and they decide the width check, so a track below the board minimum on a
  // profiled layer is not reported either (rule_parity tuning_profile_ignored).
  std::ofstream(d.dir / "b.kicad_pro") << project("ignore");
  const auto ignored = io::read_design_rules(pcb.string());
  REQUIRE(ignored.custom.size() == 4);
  CHECK(ignored.custom[0].severity == "ignore");
  CHECK(ignored.custom[0].implicit);
  b.tracks.back().width = 250'000;
  CHECK(track_width_errors(b, ignored) == 0);
  b.tracks.back().width = 50'000;  // below the board's 0.1 mm
  CHECK(track_width_errors(b, ignored) == 0);
  // A user's rule with hasExactNetclass() never matches in KiCad 10.0.6, so it is not evaluated there.
  auto user = ignored;
  user.custom.clear();
  user.custom.push_back({"W", "A.hasExactNetclass('FAST')", "", "", {{"track_width", 1'000'000, std::nullopt, std::nullopt, {}}}});
  b.tracks.back().width = 500'000;  // above the board minimum, below the rule's
  CHECK(track_width_errors(b, user) == 0);
  user.custom[0].condition = "A.NetClass == 'FAST'";
  CHECK(track_width_errors(b, user) == 1);
}

TEST_CASE("route: a profiled net has its profile's width on each layer", "[tuning][route]") {
  Dir d("route");
  const fs::path pcb = d.dir / "b.kicad_pcb", out = d.dir / "out.kicad_pcb";
  std::ofstream(pcb) << kBoard;
  std::ofstream(d.dir / "b.kicad_pro") << project("error");
  app::RouteJob job;
  job.in = pcb.string();
  job.out = out.string();
  job.opt.work_budget = 1'000'000;
  job.variants = 1;
  job.threads = 1;
  const auto r = app::run_route_job(job);
  INFO(r.summary.dump(1));
  REQUIRE(r.result.routed == 1);
  REQUIRE(!r.result.tracks.empty());
  int inner = 0;
  for (const auto& t : r.result.tracks) {
    CHECK(t.width == (t.layer == 0 || t.layer == 3 ? 150'000 : 100'000));  // outer, inner
    inner += t.layer == 1 || t.layer == 2;
  }
  CHECK(inner > 0);
  CHECK(r.summary["tuning_profiles"][0]["tracks_narrowed"] == inner);
  CHECK(r.summary["tuning_profiles"][0]["route_width_mm"] == 0.15);
  CHECK(r.summary["tuning_profiles"][0]["layers_without_tracks"].empty());
  // The written board passes the project's own rules, the profile's included.
  const auto written = io::read_board_file(out.string()).board;
  CHECK(track_width_errors(written, io::read_design_rules(out.string())) == 0);
}

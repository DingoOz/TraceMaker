// SPDX-License-Identifier: GPL-3.0-or-later
// Coupled segments of a differential pair as KiCad 10.0.6 finds them (doc 05 §59), and the router keeping out of range couplings off
// the board. The DRC's verdicts on whole boards are in the rule parity corpus (tests/integration/rule_parity, cases diff_pair_*).
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "app/route_job.hpp"
#include "drc/coupling.hpp"
#include "drc/drc.hpp"
#include "drc/rule_engine.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"

using namespace tmk;
using model::Point;

TEST_CASE("coupling: parallel tracks couple over their common stretch, edge to edge", "[drc][coupling]") {
  // P from x 0 to 10 mm, N from 4 to 14 mm, 0.5 mm apart, 0.2 mm wide: coupled over x 4..10, gap 0.3 mm.
  const auto c = drc::couple({0, 0}, {10'000'000, 0}, 200'000, {4'000'000, 500'000}, {14'000'000, 500'000}, 200'000);
  REQUIRE(c);
  CHECK(c->gap == 300'000);
  CHECK(c->length == 6'000'000);
  CHECK(c->pa == Point{4'000'000, 0});
  CHECK(c->pb == Point{10'000'000, 0});
  // The direction of either track does not matter.
  const auto r = drc::couple({10'000'000, 0}, {0, 0}, 200'000, {14'000'000, 500'000}, {4'000'000, 500'000}, 200'000);
  REQUIRE(r);
  CHECK(r->gap == 300'000);
  CHECK(r->length == 6'000'000);
  // Diagonals: 45 degree tracks 0.5 mm apart measured square to them.
  const auto d = drc::couple({0, 0}, {5'000'000, 5'000'000}, 200'000, {500'000, 0}, {5'500'000, 5'000'000}, 200'000);
  REQUIRE(d);
  CHECK(d->gap == 353'553 - 200'000);
}

TEST_CASE("coupling: tracks that meet as lines, lie on one line or do not overlap are not coupled", "[drc][coupling]") {
  CHECK_FALSE(drc::couple({0, 0}, {10'000'000, 0}, 200'000, {0, 500'000}, {10'000'000, 600'000}, 200'000));  // not quite parallel
  CHECK_FALSE(drc::couple({0, 0}, {10'000'000, 0}, 200'000, {12'000'000, 0}, {20'000'000, 0}, 200'000));     // collinear
  CHECK_FALSE(drc::couple({0, 0}, {10'000'000, 0}, 200'000, {10'000'000, 500'000}, {20'000'000, 500'000}, 200'000));  // end to end
  CHECK_FALSE(drc::couple({0, 0}, {1, 1}, 200'000, {0, 500'000}, {10'000'000, 500'000}, 200'000));          // about 1 nm long
}

TEST_CASE("coupling: nearly parallel tracks whose lines meet beyond 32 bits are coupled, as KiCad's SEG::Intersect has it",
          "[drc][coupling]") {
  // vme-wren's DQS_P0 and DQS_N0 on In4.Cu: 45 degree stubs 10 nm off parallel. KiCad 10.0.6 reports their gap as 1.8573 mm.
  const auto c = drc::couple({167'098'800, 199'267'035}, {167'332'130, 199'033'705}, 100'000, {168'516'630, 200'617'185},
                             {168'716'060, 200'417'765}, 100'000);
  REQUIRE(c);
  CHECK(std::abs(c->gap - 1'857'300) < 50);
}

TEST_CASE("pair names: KiCad's MatchDpSuffix", "[drc][coupling]") {
  const auto d = drc::match_dp_suffix("USB_DP");
  CHECK(d.polarity == 1);
  CHECK(d.complement == "USB_DN");
  CHECK(d.base == "USB_D");
  // Digits and underscores after the polarity character are kept on the complement.
  const auto l = drc::match_dp_suffix("LVDS_N0_1");
  CHECK(l.polarity == -1);
  CHECK(l.complement == "LVDS_P0_1");
  CHECK(l.base == "LVDS_");
  CHECK(drc::match_dp_suffix("CLK+").complement == "CLK-");
  CHECK(drc::match_dp_suffix("CLK-").polarity == -1);
  CHECK(drc::match_dp_suffix("P").complement == "N");
  CHECK(drc::match_dp_suffix("GND").polarity == 0);   // D is not a polarity
  CHECK(drc::match_dp_suffix("+3V3").polarity == 0);  // the scan stops at V
  CHECK(drc::match_dp_suffix("12_3").polarity == 0);
}

namespace {

namespace fs = std::filesystem;

// 30 x 20 mm, two layers. D_P and D_N each join two through-hole pads 2 mm apart: straight on one layer they would run parallel with
// a 1.75 mm gap.
const std::string kPairBoard = R"((kicad_pcb (version 20241229) (generator "pcbnew")
	(general (thickness 1.6))
	(layers (0 "F.Cu" signal) (2 "B.Cu" signal) (25 "Edge.Cuts" user))
	(setup (pad_to_mask_clearance 0))
	(net 0 "")
	(net 1 "D_P")
	(net 2 "D_N")
	(footprint "T:J" (layer "F.Cu") (at 5 10)
		(pad "1" thru_hole circle (at 0 -1) (size 1 1) (drill 0.5) (layers "*.Cu") (net 1 "D_P"))
		(pad "2" thru_hole circle (at 0 1) (size 1 1) (drill 0.5) (layers "*.Cu") (net 2 "D_N")))
	(footprint "T:J" (layer "F.Cu") (at 25 10)
		(pad "1" thru_hole circle (at 0 -1) (size 1 1) (drill 0.5) (layers "*.Cu") (net 1 "D_P"))
		(pad "2" thru_hole circle (at 0 1) (size 1 1) (drill 0.5) (layers "*.Cu") (net 2 "D_N")))
	(gr_rect (start 0 0) (end 30 20) (stroke (width 0.1) (type default)) (fill none) (layer "Edge.Cuts"))
)
)";

std::string gap_rule(const std::string& severity) {
  return "(version 1)\n(rule dp_gap (severity " + severity +
         ") (condition \"A.inDiffPair('*')\") (constraint diff_pair_gap (min 0.15mm) (max 0.25mm)))\n";
}

int gap_errors(const model::Board& b, const model::DesignRules& r) {
  int n = 0;
  for (const auto& v : drc::run_drc(b, r).violations) n += v.type == "diff_pair_gap_out_of_range";
  return n;
}

}  // namespace

TEST_CASE("route: a pair under a gap rule is not left coupled out of range", "[coupling][route]") {
  const fs::path dir = fs::temp_directory_path() / ("tmk_coupling_" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path pcb = dir / "b.kicad_pcb", out = dir / "out.kicad_pcb";
  std::ofstream(pcb) << kPairBoard;
  std::ofstream(dir / "b.kicad_pro") << "{}";
  const auto route = [&](const std::string& severity) {
    std::ofstream(dir / "b.kicad_dru") << gap_rule(severity);
    app::RouteJob job;
    job.in = pcb.string();
    job.out = out.string();
    job.opt.work_budget = 1'000'000;
    job.variants = 1;
    job.threads = 1;
    const auto r = app::run_route_job(job);
    INFO(r.summary.dump(1));
    CHECK(r.result.routed == 2);
    return io::read_board_file(out.string()).board;
  };
  // Judged by the rule at severity error. Ignored, the rule does not steer the router, and the straight tracks break it; in force, the
  // router keeps the halves from coupling out of range.
  std::ofstream(dir / "b.kicad_dru") << gap_rule("error");
  const auto rules = io::read_design_rules(pcb.string());
  CHECK(gap_errors(route("ignore"), rules) > 0);
  CHECK(gap_errors(route("error"), rules) == 0);
  fs::remove_all(dir);
}

TEST_CASE("coupling: tracks of a tuning pattern are not coupled", "[drc][coupling]") {
  // D_P and D_N run parallel 1.75 mm apart, out of a 0.15-0.25 mm gap rule. Made members of a tuning pattern, KiCad leaves the tracks
  // out of the coupling test (isInTuningPattern); members of a plain group are still tested.
  const fs::path dir = fs::temp_directory_path() / ("tmk_tuning_" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path pcb = dir / "b.kicad_pcb";
  std::ofstream(dir / "b.kicad_dru") << gap_rule("error");
  const auto judge = [&](const std::string& group) {
    std::string text = kPairBoard.substr(0, kPairBoard.rfind(')'));
    text += "\t(segment (start 5 9) (end 25 9) (width 0.25) (layer \"F.Cu\") (net 1) (uuid \"p\"))\n"
            "\t(segment (start 5 11) (end 25 11) (width 0.25) (layer \"F.Cu\") (net 2) (uuid \"n\"))\n" +
            group + ")\n";
    std::ofstream(pcb) << text;
    const auto board = io::read_board_file(pcb.string()).board;
    return gap_errors(board, io::read_design_rules(pcb.string()));
  };
  CHECK(judge("") == 1);
  CHECK(judge("\t(group \"\" (uuid \"g\") (members \"p\" \"n\"))\n") == 1);
  CHECK(judge("\t(generated (uuid \"g\") (type tuning_pattern) (name \"Tuning Pattern\") (layer \"F.Cu\") (members \"n\"))\n") == 0);
  fs::remove_all(dir);
}

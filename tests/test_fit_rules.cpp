// SPDX-License-Identifier: GPL-3.0-or-later
// Rule fit (doc 20 §2.8, D194) and the chain measurement (doc 20 §2.9).
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "app/chains.hpp"
#include "drc/fit_rules.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "route/obstacles.hpp"
#include "route/router.hpp"

using namespace tmk;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path dir;
  TempDir() : dir(fs::temp_directory_path() / ("tm_fit_" + std::to_string(::getpid()))) { fs::create_directories(dir); }
  ~TempDir() { fs::remove_all(dir); }
};

// 30 x 20 mm, two layers, KiCad 5 format (no edge clearance in the file: KiCad 10's default of 0.5 mm holds). `fingers` connector pads
// sit 0.1 mm from the top edge, as a card-edge connector's do. With `thin`, five tracks of 0.15 mm join them to inner pads, under the
// file's 0.2 mm minimum and the class's 0.25 mm.
std::string board(int fingers, bool thin) {
  std::string s = R"((kicad_pcb (version 20171130) (host pcbnew 5.1.6)
  (general (thickness 1.6))
  (layers
    (0 F.Cu signal)
    (31 B.Cu signal)
    (44 Edge.Cuts user)
  )
  (setup (trace_min 0.2) (pad_to_mask_clearance 0))
  (net 0 "")
)";
  for (int i = 0; i < 5; ++i) s += "  (net " + std::to_string(i + 1) + " N" + std::to_string(i) + ")\n";
  s += "  (net_class Default \"\" (clearance 0.2) (trace_width 0.25) (via_dia 0.8) (via_drill 0.4)";
  for (int i = 0; i < 5; ++i) s += " (add_net N" + std::to_string(i) + ")";
  s += ")\n  (module T:J (layer F.Cu) (at 5 0.6)\n    (fp_text reference J1 (at 0 0) (layer F.SilkS))";
  for (int i = 0; i < 5; ++i)
    s += "\n    (pad " + std::to_string(i + 1) + " smd rect (at " + std::to_string(i * 2) + " " + (i < fingers ? "0" : "3") +
         ") (size 1 1) (layers F.Cu F.Paste F.Mask) (net " + std::to_string(i + 1) + " N" + std::to_string(i) + "))";
  s += ")\n  (module T:U (layer F.Cu) (at 5 12)\n    (fp_text reference U1 (at 0 0) (layer F.SilkS))";
  for (int i = 0; i < 5; ++i)
    s += "\n    (pad " + std::to_string(i + 1) + " smd rect (at " + std::to_string(i * 2) + " 0) (size 1 1) (layers F.Cu F.Paste F.Mask) (net " + std::to_string(i + 1) + " N" +
         std::to_string(i) + "))";
  s += ")\n";
  if (thin)
    for (int i = 0; i < 5; ++i)
      s += "  (segment (start " + std::to_string(5 + i * 2) + " " + (i < fingers ? "0.6" : "3.6") + ") (end " + std::to_string(5 + i * 2) + " 12) (width 0.15) (layer F.Cu) (net " +
           std::to_string(i + 1) + "))\n";
  s += R"(  (gr_line (start 0 0) (end 30 0) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 30 0) (end 30 20) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 30 20) (end 0 20) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 0 20) (end 0 0) (layer Edge.Cuts) (width 0.1))
)
)";
  return s;
}

const drc::FittedRule* change(const drc::FitReport& r, const std::string& rule) {
  for (const auto& c : r.changes)
    if (c.rule == rule) return &c;
  return nullptr;
}

}  // namespace

TEST_CASE("rule fit: pads inside the edge clearance bring it down to their gap, and only with enough evidence", "[fit]") {
  TempDir d;
  const fs::path p = d.dir / "fingers.kicad_pcb", q = d.dir / "two.kicad_pcb";
  std::ofstream(p) << board(4, false);
  std::ofstream(q) << board(2, false);
  {
    auto rules = io::read_design_rules(p.string());
    REQUIRE(rules.minimums.copper_edge_clearance == 500'000);
    const auto fit = drc::fit_rules(io::read_board_file(p.string()).board, rules);
    const auto* c = change(fit, "copper_edge_clearance");
    REQUIRE(c);
    CHECK(c->from == 500'000);
    CHECK(c->evidence == 4);
    // The pads' edge is 0.1 mm from the edge line's centre; the line is 0.1 mm wide, so its copper side is 0.05 mm nearer.
    CHECK(c->to >= 40'000);
    CHECK(c->to <= 100'000);
    CHECK(rules.minimums.copper_edge_clearance == c->to);
    CHECK(fit.changes.size() == 1);
    // A second fit finds nothing left to relax: the fit only ever lowers, to what the copper has.
    CHECK(drc::fit_rules(io::read_board_file(p.string()).board, rules).changes.empty());
  }
  {
    // Two violations are taken for slips (FitOptions::min_evidence).
    auto rules = io::read_design_rules(q.string());
    const auto fit = drc::fit_rules(io::read_board_file(q.string()).board, rules);
    CHECK(fit.changes.empty());
    CHECK(fit.violations == 2);
    CHECK(rules.minimums.copper_edge_clearance == 500'000);
  }
}

TEST_CASE("rule fit from an earlier layout: its tracks give the minimum and the class width", "[fit]") {
  TempDir d;
  const fs::path bare = d.dir / "bare.kicad_pcb", ref = d.dir / "ref.kicad_pcb";
  std::ofstream(bare) << board(0, false);
  std::ofstream(ref) << board(0, true);
  auto rules = io::read_design_rules(bare.string());
  REQUIRE(rules.minimums.track_width == 200'000);
  REQUIRE(rules.default_class().track_width == 250'000);
  // The board's own pads break nothing.
  CHECK(drc::fit_rules(io::read_board_file(bare.string()).board, rules).changes.empty());
  const auto fit = drc::fit_rules(io::read_board_file(ref.string()).board, rules);
  const auto* m = change(fit, "track_width");
  REQUIRE(m);
  CHECK(m->to == 150'000);
  CHECK(m->evidence == 5);
  const auto* c = change(fit, "class Default track_width");
  REQUIRE(c);
  CHECK(c->from == 250'000);
  CHECK(c->to == 150'000);
  CHECK(rules.default_class().track_width == 150'000);
  for (const auto& x : fit.changes) CHECK(x.to < x.from);
}

// The board of tests/test_causes.cpp: S joins U1.1 and R2.1, and a square of net T on both layers encloses U1.1.
TEST_CASE("chains: a connection closed by lifting one net is depth 1", "[chains]") {
  std::string ring;
  for (const char* layer : {"F.Cu", "B.Cu"})
    for (const char* seg : {"(start 4 9) (end 6 9)", "(start 6 9) (end 6 11)", "(start 6 11) (end 4 11)", "(start 4 11) (end 4 9)"})
      ring += std::string("  (segment ") + seg + " (width 0.25) (layer " + layer + ") (net 2))\n";
  const std::string text = R"((kicad_pcb (version 20171130) (host pcbnew 5.1.6)
  (general (thickness 1.6))
  (layers
    (0 F.Cu signal)
    (31 B.Cu signal)
    (44 Edge.Cuts user)
  )
  (setup (pad_to_mask_clearance 0))
  (net 0 "")
  (net 1 S)
  (net 2 T)
  (module T:U (layer F.Cu) (at 5 10)
    (fp_text reference U1 (at 0 0) (layer F.SilkS))
    (pad 1 smd rect (at 0 0) (size 1 1) (layers F.Cu F.Paste F.Mask) (net 1 S)))
  (module T:R (layer F.Cu) (at 25 10)
    (fp_text reference R2 (at 0 0) (layer F.SilkS))
    (pad 1 smd rect (at 0 0) (size 1 1) (layers F.Cu F.Paste F.Mask) (net 1 S)))
  (module T:X (layer F.Cu) (at 15 17)
    (fp_text reference X9 (at 0 0) (layer F.SilkS))
    (pad 1 smd rect (at 0 0) (size 1 1) (layers F.Cu F.Paste F.Mask) (net 2 T)))
)" + ring + R"(  (gr_line (start 0 0) (end 30 0) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 30 0) (end 30 20) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 30 20) (end 0 20) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 0 20) (end 0 0) (layer Edge.Cuts) (width 0.1))
)
)";
  TempDir d;
  const fs::path p = d.dir / "ring.kicad_pcb";
  std::ofstream(p) << text;
  app::ChainsOptions o;
  o.baseline_work = 300'000;
  o.work = 300'000;
  o.threads = 1;
  const auto j = app::measure_chains(p.string(), o, false);
  INFO(j.dump(1));
  REQUIRE(j["baseline"]["open"] == 1);
  const auto& c = j["connections"][0];
  CHECK(c["net"] == "S");
  CHECK(c["closed"] == true);
  CHECK(c["depth"] == 1);
  CHECK(c["first_blockers"] == 1);
  CHECK(c["first_nets"] == nlohmann::json::array({"T"}));
  CHECK(j["tally"]["closed_joint"] == 1);
  o.threads = 4;
  CHECK(app::measure_chains(p.string(), o, false) == j);
}

// A card-edge connector with fingers on both sides, each opening both masks (PCBench fifogfx_c64cart): the back finger of net B lies
// in the front finger's opening and the front finger of net A in the back finger's, so no track of either may leave its own pad.
TEST_CASE("rule fit: a mask opening that already exposes another net's pad does not keep tracks out", "[fit][route]") {
  const std::string text = R"((kicad_pcb (version 20171130) (host pcbnew 5.1.6)
  (general (thickness 1.6))
  (layers
    (0 F.Cu signal)
    (31 B.Cu signal)
    (44 Edge.Cuts user)
  )
  (setup (pad_to_mask_clearance 0.1))
  (net 0 "")
  (net 1 A)
  (net 2 B)
  (module T:EDGE (layer F.Cu) (at 10 10)
    (fp_text reference P1 (at 0 0) (layer F.SilkS))
    (pad 1 connect rect (at 0 0) (size 1.5 6) (layers *.Mask F.Cu) (net 1 A))
    (pad 2 connect rect (at 0 0) (size 1.5 6) (layers *.Mask B.Cu) (net 2 B)))
  (module T:R (layer F.Cu) (at 22 6)
    (fp_text reference R1 (at 0 0) (layer F.SilkS))
    (pad 1 smd rect (at 0 0) (size 1 1) (layers F.Cu F.Paste F.Mask) (net 1 A)))
  (module T:R (layer B.Cu) (at 22 14)
    (fp_text reference R2 (at 0 0) (layer B.SilkS))
    (pad 1 smd rect (at 0 0) (size 1 1) (layers B.Cu B.Paste B.Mask) (net 2 B)))
  (gr_line (start 0 0) (end 30 0) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 30 0) (end 30 20) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 30 20) (end 0 20) (layer Edge.Cuts) (width 0.1))
  (gr_line (start 0 20) (end 0 0) (layer Edge.Cuts) (width 0.1))
)
)";
  TempDir d;
  const fs::path p = d.dir / "edge.kicad_pcb";
  std::ofstream(p) << text;
  auto lb = io::read_board_file(p.string());
  const auto rules = io::read_design_rules(p.string());
  CHECK(route::Obstacles(lb.board, rules).bridged_mask_openings() == 2);
  CHECK(route::Obstacles(lb.board, rules).mask_graphics_over_pads() == 0);
  route::RouterOptions o;
  o.work_budget = 300'000;
  o.gpu_device = -1;
  const auto strict = route::Router(lb.board, rules, o).run();
  CHECK(strict.connections == 2);
  CHECK(strict.routed == 0);
  o.soft_bridged_masks = true;
  const auto fitted = route::Router(lb.board, rules, o).run();
  CHECK(fitted.routed == 2);
}

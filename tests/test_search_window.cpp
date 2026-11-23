// SPDX-License-Identifier: GPL-3.0-or-later
// The 31-bit state-index guard (hardening #12, #15): a search window's states are indexed through int32
// (SNode::parent, and the open list's tie-deep key masks the index to 31 bits), so a window past 2^31 states
// cannot be searched at full direction resolution. With no memory budget to hide behind, the guard must fall
// back to one direction state per point (fast bends) instead of letting the indices alias -- and before the
// fallback existed, instead of refusing a window that fits perfectly well at that resolution.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "app/route_job.hpp"
#include "io/kicad/board_reader.hpp"

using namespace tmk;
namespace fs = std::filesystem;

namespace {

// 30 x 30 mm on all 32 copper layers, one connection corner to corner. At a forced 10 um pitch the lattice is
// 3000 x 3000 per layer: 9M cells x 32 layers x 9 direction states = 2.59e9 states (past 2^31), while one
// direction state per point is 2.88e8 (well inside). Nothing else is on the board, so at that resolution the
// connection routes straight.
std::string board() {
  std::string layers;
  for (int i = 0; i <= 31; ++i) {
    const std::string name = i == 0 ? "F.Cu" : (i == 31 ? "B.Cu" : "In" + std::to_string(i) + ".Cu");
    layers += "    (" + std::to_string(i) + " " + name + " signal)\n";
  }
  return "(kicad_pcb (version 20171130) (host pcbnew 5.1.6)\n  (general (thickness 1.6))\n  (layers\n" + layers +
         "    (44 Edge.Cuts user)\n  )\n  (setup (pad_to_mask_clearance 0))\n  (net 0 \"\")\n  (net 1 S)\n"
         "  (module T:U (layer F.Cu) (at 2 2)\n    (fp_text reference U1 (at 0 0) (layer F.SilkS))\n"
         "    (pad 1 smd rect (at 0 0) (size 1 1) (layers F.Cu F.Paste F.Mask) (net 1 S)))\n"
         "  (module T:R (layer F.Cu) (at 28 28)\n    (fp_text reference R2 (at 0 0) (layer F.SilkS))\n"
         "    (pad 1 smd rect (at 0 0) (size 1 1) (layers F.Cu F.Paste F.Mask) (net 1 S)))\n"
         "  (gr_line (start 0 0) (end 30 0) (layer Edge.Cuts) (width 0.1))\n"
         "  (gr_line (start 30 0) (end 30 30) (layer Edge.Cuts) (width 0.1))\n"
         "  (gr_line (start 30 30) (end 0 30) (layer Edge.Cuts) (width 0.1))\n"
         "  (gr_line (start 0 30) (end 0 0) (layer Edge.Cuts) (width 0.1))\n)\n";
}

}  // namespace

TEST_CASE("search window past 2^31 states falls back to one direction state per point", "[router][paged]") {
  const fs::path dir = fs::temp_directory_path() / ("tm_guard_" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  const fs::path pcb = dir / "b.kicad_pcb", out = dir / "out.kicad_pcb";
  std::ofstream(pcb) << board();
  std::ofstream(dir / "b.kicad_pro") << "{}";
  app::RouteJob job;
  job.in = pcb.string();
  job.out = out.string();
  job.opt.pitch = 10'000;        // 10 um, forced
  job.opt.paged_nodes = true;    // the paged path never draws the window in: the guard alone decides
  job.opt.search_mem_mb = 0;     // no budget: the pre-#12 code returned true here and let the indices alias
  job.opt.work_budget = 20'000'000;
  job.variants = 1;
  job.threads = 1;
  const auto r = app::run_route_job(job);
  INFO(r.summary.dump(1));
  CHECK(r.result.connections == 1);
  CHECK(r.result.routed == 1);
  fs::remove_all(dir);
}

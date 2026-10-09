// SPDX-License-Identifier: GPL-3.0-or-later
// Plane targets with --soft-zones (doc 05 §26): teardrops are no planes, and a plane no pad of the net can reach is
// no target.
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "route/router.hpp"

using namespace tmk;
namespace fs = std::filesystem;

namespace {

// 30 x 20 mm, four copper layers. Net S joins two F.Cu pads 20 mm apart; `extra` adds copper of S.
std::string board_text(const std::string& extra) {
  return R"((kicad_pcb (version 20240108) (generator "pcbnew")
  (general (thickness 1.6))
  (layers (0 "F.Cu" signal) (1 "In1.Cu" signal) (2 "In2.Cu" signal) (31 "B.Cu" signal) (44 "Edge.Cuts" user))
  (setup (pad_to_mask_clearance 0))
  (net 0 "") (net 1 "S")
  (footprint "T:P" (layer "F.Cu") (at 5 10) (property "Reference" "R1")
    (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "S")))
  (footprint "T:P" (layer "F.Cu") (at 25 10) (property "Reference" "R2")
    (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "S")))
  (gr_rect (start 0 0) (end 30 20) (stroke (width 0.1) (type default)) (fill none) (layer "Edge.Cuts"))
)" + extra + ")\n";
}

std::string zone(const std::string& layer, const std::string& pts, bool teardrop) {
  return "  (zone (net 1) (net_name \"S\") (layer \"" + layer + "\") (hatch edge 0.5) (connect_pads (clearance 0.2))" +
         (teardrop ? " (attr (teardrop (type padvia)))" : "") +
         " (min_thickness 0.25)\n    (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon (pts " + pts +
         "))\n    (filled_polygon (layer \"" + layer + "\") (pts " + pts + ")))\n";
}

// An isolated 4 mm^2 teardrop of S between the pads (left behind by deleted routing), and a plane of S on In1.Cu.
const std::string kTeardrop = zone("F.Cu", "(xy 14 3) (xy 16 3) (xy 16 5) (xy 14 5)", true);
const std::string kPlane = zone("In1.Cu", "(xy 2 2) (xy 28 2) (xy 28 18) (xy 2 18)", false);

struct Files {
  fs::path dir, pcb;
  Files(const std::string& name, const std::string& board, const std::string& dru) {
    dir = fs::temp_directory_path() / ("tmk_soft_zones_" + name + "_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    pcb = dir / "b.kicad_pcb";
    std::ofstream(pcb) << board;
    if (!dru.empty()) std::ofstream(dir / "b.kicad_dru") << dru;
  }
  ~Files() { fs::remove_all(dir); }
};

route::RouteResult run(const Files& f, bool soft, bool vias = true) {
  const auto lb = io::read_board_file(f.pcb.string());
  route::RouterOptions o;
  o.work_budget = 3'000'000;
  o.time_limit_s = 600;
  o.gpu_device = -1;
  o.soft_zones = soft;
  o.allow_vias = vias;
  return route::Router(lb.board, io::read_design_rules(f.pcb.string()), o).run();
}

}  // namespace

TEST_CASE("soft zones: a teardrop is read as one and is never a plane target", "[route][planes]") {
  const Files f("teardrop", board_text(kTeardrop + kPlane), "");
  const auto b = io::read_board_file(f.pcb.string()).board;
  REQUIRE(b.zones.size() == 2);
  CHECK(b.zones[0].teardrop);
  CHECK_FALSE(b.zones[1].teardrop);

  // Teardrop alone: one pad-to-pad connection, not two connections into the teardrop.
  const Files t("teardrop_only", board_text(kTeardrop), "");
  const auto r = run(t, true);
  CHECK(r.connections == 1);
  CHECK(r.routed == 1);
}

TEST_CASE("soft zones: a plane no pad of the net can reach is no target", "[route][planes]") {
  // Reachable: both pads drop a via into the plane.
  const Files f("plane", board_text(kPlane), "");
  const auto reach = run(f, true);
  CHECK(reach.connections == 2);
  CHECK(reach.routed == 2);
  CHECK(!reach.vias.empty());

  // Without vias the F.Cu pads cannot reach In1.Cu: the pads join each other instead of two dead plane connections.
  const auto novias = run(f, true, false);
  CHECK(novias.connections == 1);
  CHECK(novias.routed == 1);
  CHECK(novias.vias.empty());

  // The same when a custom rule forbids S's vias.
  const Files g("plane_rule", board_text(kPlane), "(version 1)\n(rule \"no S vias\" (condition \"A.NetName == 'S'\") (constraint disallow via))\n");
  const auto rule = run(g, true);
  CHECK(rule.connections == 1);
  CHECK(rule.routed == 1);
  CHECK(rule.vias.empty());
}

TEST_CASE("soft zones: one through via joins every same-net plane it passes through", "[route][planes]") {
  // S planes on In1.Cu and In2.Cu, joined by nothing: two plane targets besides the two pads. A via from a pad
  // passes through both planes, so the second plane needs no connection of its own.
  const Files f("two_planes", board_text(kPlane + zone("In2.Cu", "(xy 2 2) (xy 28 2) (xy 28 18) (xy 2 18)", false)), "");
  const auto r = run(f, true);
  CHECK(r.connections == 3);
  CHECK(r.routed == 3);
  CHECK(r.vias.size() == 2);
}

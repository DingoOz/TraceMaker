// SPDX-License-Identifier: GPL-3.0-or-later
// Vias and pads that remove unconnected copper (doc 05 §60, D150): read from the file, flashed as KiCad 10.0.6 flashes them, and
// refilled around as its zone filler does. KiCad's verdicts on such vias are in the rule parity corpus (via_unused_layers).
#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "drc/copper.hpp"
#include "drc/drc.hpp"
#include "drc/refill.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"

using namespace tmk;
namespace fs = std::filesystem;

namespace {

// 30 x 20 mm, four layers, a GND plane on In1.Cu. `vias` adds items.
std::string board_text(const std::string& vias) {
  return R"((kicad_pcb (version 20241229) (generator "pcbnew")
  (general (thickness 1.6))
  (layers (0 "F.Cu" signal) (4 "In1.Cu" signal) (6 "In2.Cu" signal) (2 "B.Cu" signal) (25 "Edge.Cuts" user))
  (setup (pad_to_mask_clearance 0))
  (net 0 "") (net 1 "GND") (net 2 "S")
  (gr_rect (start 0 0) (end 30 20) (stroke (width 0.1) (type default)) (fill none) (layer "Edge.Cuts"))
  (zone (net 1) (net_name "GND") (layer "In1.Cu") (uuid "a") (hatch edge 0.5) (connect_pads (clearance 0.2))
    (min_thickness 0.25) (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5))
    (polygon (pts (xy 1 1) (xy 29 1) (xy 29 19) (xy 1 19))))
)" + vias + ")\n";
}

std::string via(double x, double y, int net, const std::string& flags) {
  return "  (via (at " + std::to_string(x) + " " + std::to_string(y) + ") (size 0.8) (drill 0.4) (layers \"F.Cu\" \"B.Cu\") " + flags +
         " (net " + std::to_string(net) + "))\n";
}

struct Loaded {
  model::Board board;
  model::DesignRules rules;
};

Loaded load(const std::string& name, const std::string& text) {
  const fs::path dir = fs::temp_directory_path() / ("tmk_flash_" + name + "_" + std::to_string(::getpid()));
  fs::create_directories(dir);
  const fs::path pcb = dir / "b.kicad_pcb";
  std::ofstream(pcb) << text;
  Loaded l{io::read_board_file(pcb.string()).board, io::read_design_rules(pcb.string())};
  fs::remove_all(dir);
  return l;
}

constexpr model::LayerMask kF = 1, kIn1 = 2, kIn2 = 4, kB = 8;

}  // namespace

TEST_CASE("unused layers: read in file order and flashed where something connects", "[refill][flash]") {
  const std::string keep = "(remove_unused_layers yes) (keep_end_layers yes)";
  auto l = load("read", board_text(via(5, 5, 2, keep) + via(10, 5, 2, keep + " (zone_layer_connections \"In2.Cu\")") +
                                   via(15, 5, 2, "(remove_unused_layers yes) (keep_end_layers no)") + via(20, 5, 2, "(start_end_only yes)") +
                                   via(25, 5, 2, "") +
                                   "  (segment (start 5 5) (end 5 8) (width 0.25) (layer \"In2.Cu\") (net 2))\n"
                                   "  (segment (start 15 5.35) (end 15 8) (width 0.25) (layer \"B.Cu\") (net 2))\n"));
  const auto& v = l.board.vias;
  REQUIRE(v.size() == 5);
  using U = model::UnconnectedLayers;
  CHECK(v[0].unconnected == U::RemoveExceptStartEnd);
  CHECK(v[1].zone_layers == kIn2);
  CHECK(v[2].unconnected == U::RemoveAll);
  CHECK(v[3].unconnected == U::StartEndOnly);
  CHECK(v[4].unconnected == U::KeepAll);
  const auto f = drc::flash_layers(l.board);
  CHECK(f.vias[0] == (kF | kIn2 | kB));  // the ends, and In2.Cu where a track reaches the hole
  CHECK(f.vias[1] == (kF | kIn2 | kB));  // the ends, and the zone connection
  CHECK(f.vias[2] == 0);                 // the track on B.Cu reaches the ring but not the hole
  CHECK(f.vias[3] == (kF | kB));
  CHECK(f.vias[4] == (kF | kIn1 | kIn2 | kB));
  // The copper model splits a via with bare layers: its copper, and its hole on the bare layers.
  const auto cm = drc::build_copper(l.board);
  int split = 0;
  for (const auto& it : cm.items)
    if (it.kind == drc::ItemKind::Via && it.bare_of >= 0) {
      ++split;
      CHECK(it.shapes.front().r == 200'000);
      CHECK((it.layers | cm.items[static_cast<std::size_t>(it.bare_of)].layers) == it.span);
    }
  CHECK(split == 4);
}

TEST_CASE("refill: a via that removes unconnected copper joins the plane of its net and is a bare hole in another's", "[refill][flash]") {
  const std::string keep = "(remove_unused_layers yes) (keep_end_layers yes)";
  auto l = load("refill", board_text(via(10, 10, 1, keep) + via(20, 10, 2, keep)));
  const auto rf = drc::refill_zones(l.board, l.rules);
  CHECK(rf.board.vias[0].zone_layers == kIn1);  // the GND plane is the highest-priority zone at its hole
  CHECK(rf.board.vias[1].zone_layers == 0);
  // The plane keeps the clearance from S's hole, not from a ring S does not have on In1.Cu: it comes within the ring's clearance
  // (0.4 + 0.2 mm) of the centre.
  double nearest = 1e18;
  for (const auto& [layer, pts] : rf.board.zones[0].fills)
    for (const auto& p : pts) nearest = std::min(nearest, std::hypot(static_cast<double>(p.x - 20'000'000), static_cast<double>(p.y - 10'000'000)));
  CHECK(nearest > 400'000);
  CHECK(nearest < 550'000);
  int errors = 0;
  for (const auto& v : drc::run_drc(rf.board, l.rules).violations) errors += v.severity == "error";
  CHECK(errors == 0);
}

// SPDX-License-Identifier: GPL-3.0-or-later
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>

#include "io/kicad/board_reader.hpp"
#include "io/kicad/netlist_reader.hpp"

namespace fs = std::filesystem;

TEST_CASE("schematic netlist agrees with the board's pad nets", "[netlist][kicad]") {
  const fs::path net = fs::path(TM_SOURCE_DIR) / "tests/truth/complex_hierarchy.net";
  const fs::path pcb = fs::path(TM_SOURCE_DIR) / "bench/data/kicad/demos/complex_hierarchy/complex_hierarchy.kicad_pcb";
  const auto nl = tmk::io::read_netlist_file(net.string());
  CHECK(nl.components.size() > 20);
  CHECK(nl.nets.size() > 20);
  if (!fs::exists(pcb)) SKIP("fixtures missing");
  const auto lb = tmk::io::read_board_file(pcb.string());
  std::map<std::pair<std::string, std::string>, std::string> pad_net;
  for (const auto& p : lb.board.pads)
    pad_net[{lb.board.footprints[static_cast<std::size_t>(p.footprint)].reference, p.number}] =
        lb.board.nets[static_cast<std::size_t>(p.net)].name;
  int checked = 0, mismatched = 0;
  for (const auto& n : nl.nets)
    for (const auto& node : n.nodes) {
      const auto it = pad_net.find({node.ref, node.pin});
      if (it == pad_net.end()) continue;
      ++checked;
      // Single-pin nets are named "unconnected-(...)" in the netlist and left unnamed on some boards.
      if (it->second != n.name && !n.name.starts_with("unconnected-")) {
        ++mismatched;
        if (mismatched < 5) WARN(node.ref << "." << node.pin << ": board " << it->second << " netlist " << n.name);
      }
    }
  CHECK(checked > 100);
  CHECK(mismatched == 0);
}

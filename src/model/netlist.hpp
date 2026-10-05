// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Schematic connectivity as exported by KiCad (kicad-cli sch export netlist --format kicadsexpr).
#include <string>
#include <vector>

namespace tmk::model {

struct NetlistComponent {
  std::string ref, value, footprint;
  bool dnp = false, exclude_from_board = false;
};

struct NetlistNode {
  std::string ref, pin, pinfunction, pintype;
};

struct NetlistNet {
  std::string name, netclass;
  std::vector<NetlistNode> nodes;
};

struct Netlist {
  std::vector<NetlistComponent> components;
  std::vector<NetlistNet> nets;
};

}  // namespace tmk::model

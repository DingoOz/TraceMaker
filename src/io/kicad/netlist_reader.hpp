#pragma once
#include <string>

#include "model/netlist.hpp"

namespace tmk::io {

// Parses a KiCad s-expression netlist (format "E", from kicad-cli sch export netlist --format kicadsexpr).
model::Netlist read_netlist_file(const std::string& path);

// Runs `kicad-cli sch export netlist` on a schematic and parses the result. Throws if kicad-cli fails.
model::Netlist netlist_from_schematic(const std::string& sch_path, const std::string& tmp_dir);

}  // namespace tmk::io

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Reads design rules from a KiCad project: <name>.kicad_pro (JSON) and <name>.kicad_dru (s-expressions).
#include <string>

#include "model/rules.hpp"

namespace tmk::io {

// `board_path` is the .kicad_pcb; the project and rule files are looked up next to it with the same stem.
// Missing files give KiCad's defaults and a warning, never an exception.
model::DesignRules read_design_rules(const std::string& board_path);

// Parses a KiCad length with an optional unit suffix ("0.2mm", "8mil", "0.01in", "150um", "0.2") to nm.
std::optional<Coord> parse_length(std::string_view s);

}  // namespace tmk::io

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::app {
// Prints a human-readable board summary.
void print_summary(const model::Board& b, const std::string& path);
// Writes the board in the same JSON schema as scripts/kicad_truth.py, for comparison with KiCad.
void write_truth_json(const model::Board& b, const std::string& out_path, const model::DesignRules* rules = nullptr);
}  // namespace tmk::app

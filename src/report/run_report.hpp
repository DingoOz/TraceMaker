// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The run report (doc 18 D3, D136): the sections of the route summary that a designer needs to audit a run and that the
// plain summary does not carry: which rules were read and which were not, and the DRC of the board before and after the
// route. The report file itself is rendered from the summary alone (report/render.hpp), so every number in it is a
// field of the JSON.
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::report {

// What the router took from the board's project (`board_path` names the .kicad_pcb; the .kicad_pro and .kicad_dru are
// looked for beside it as io::read_design_rules does): board minimums, net classes with their net counts, custom
// rules, and everything that could not be read (`rules.warnings`, the rule engine's `engine_warnings` and the board
// reader's `board.warnings`). Rule 6: what is in `not_read` the route may break.
nlohmann::json rules_json(const std::string& board_path, const model::Board& board, const model::DesignRules& rules,
                          const std::vector<std::string>& engine_warnings);

// TraceMaker's DRC (KiCad's violation names, doc 03 §6) of the board the router started from and of the board it
// wrote, by violation type, with what the route added. With `refill` the copper zones of both are refilled first, as
// `kicad-cli pcb drc --refill-zones` does (the plane tier is judged that way, doc 10 §4). Counts per type, not the
// instance matching of bench/drc_judge.py: a type whose count did not grow may still hold a new violation that replaced
// an old one.
// The errors of the board before the route are listed with where they are (`before/error_items`, the first of each), so a
// designer can tell the input's own errors from the route's. With `router_open` (names of the nets the router left a connection of open),
// each unconnected item after the route says whether its net is one of them (`open_by_router`) or was completed by the router and
// cut off by the zone refill, and `after` counts both.
nlohmann::json drc_json(const model::Board& before, const model::Board& after, const model::DesignRules& rules, bool refill,
                        const std::set<std::string>* router_open = nullptr);

// True for violation types that routing can cause (bench/drc_judge.py NOT_ROUTING lists the others; unconnected items
// are counted on their own).
bool routing_violation(const std::string& type);

}  // namespace tmk::report

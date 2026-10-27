// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Generated pours (doc 05 §53, M20 C3): a ground or power zone the route adds to a board that has none, routed with
// soft zones so other nets cut through it and the refill decides what it connects. The zone is written into the output
// with the engine's fill; the designer can move, delete or refill it in KiCad like any zone.
#include <string>
#include <vector>

#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::passes {

struct PourPlan {
  model::Zone zone;    // net, one copper layer, outline (the board's outline box; the refill clips it to the board), settings
  std::string reason;  // why this net and layer ("auto: ground net GND, back layer")
};

// Resolves `route --pour` entries against the board:
// - "auto": only when the board has no copper zone (rule areas and teardrops aside); its ground net (a ground name such
//   as GND, GNDD or VSS, with pads; the one with the most pads) on the first inner layer, else on the back layer. A board
//   without such a net gets no pour (a note says why).
// - "NET@LAYER": that net on that copper layer (KiCad or file layer name).
// Settings are KiCad's defaults for a new zone (minimum width 0.25 mm, islands removed) except: pads connect solid, not
// through thermal reliefs (spokes the routed copper cuts off are KiCad starved_thermal errors), and the clearance is the
// net class clearance but at least 0.3 mm, so the fill keeps out of fine-pitch gaps it could not use. Throws std::invalid_argument on a bad entry, an unknown net or a layer that is not copper.
std::vector<PourPlan> plan_pours(const model::Board& b, const model::DesignRules& rules, const std::vector<std::string>& specs,
                                 std::vector<std::string>* notes = nullptr);

}  // namespace tmk::passes

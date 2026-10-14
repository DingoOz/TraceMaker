// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Zone refill in the engine (doc 05 §36): the fills KiCad 10's zone filler would draw for the board's current
// copper, so connectivity can be judged as `kicad-cli pcb drc --refill-zones` judges it. The fills are not
// written to files.
#include <string>
#include <vector>

#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::drc {

struct RefillResult {
  model::Board board;            // the input with every copper zone refilled
  int zones = 0;                 // copper zones refilled
  int islands_removed = 0;       // fill islands no pad reaches, removed by the zones' island settings
  std::vector<std::string> warnings;
};

// Follows ZONE_FILLER::fillCopperZone: outline within the board, thermal gaps around same-net pads, every other
// item knocked out at its clearance, spokes that reach the fill, minimum-width pruning, then islands without a pad
// removed by each zone's island_removal_mode. Teardrop zones lose their fill: kicad-cli's refill first rebuilds
// teardrops (TEARDROP_MANAGER::UpdateTeardrops), and a rebuilt teardrop joins a pad to a track that already
// touches it. Hatched zones are not redrawn: their stored fill is kept where the new solid fill still is (warning).
RefillResult refill_zones(const model::Board& b, const model::DesignRules& r);

}  // namespace tmk::drc

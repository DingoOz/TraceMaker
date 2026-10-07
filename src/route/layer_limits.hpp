// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Which copper layers may carry new tracks, and what a track costs on each (doc 05 §29): the user's
// `--no-tracks-on` and `--layer-cost` lists turned into the numbers RouterOptions holds.
#include <cstdint>
#include <string>
#include <vector>

#include "model/board.hpp"

namespace tmk::route {

struct LayerLimits {
  model::LayerMask no_track_layers = 0;     // copper layers that get no new tracks
  std::vector<std::int32_t> layer_cost_pm;  // per copper index: track cost per mille (1000 = normal); empty = all normal
};

inline constexpr double kMaxLayerCost = 1000.0;

// `no_tracks_on`: copper layer names. `layer_costs`: "NAME=FACTOR" entries with 1 <= FACTOR <= kMaxLayerCost.
// Names are the board's canonical names ("In1.Cu"), the names written in the file, or the user's layer names.
// Throws std::invalid_argument on an unknown layer, a malformed entry, a factor out of range, or when no layer
// would be left for tracks: a limit the router cannot apply as written must stop the run, not be dropped.
LayerLimits layer_limits(const model::Board& b, const std::vector<std::string>& no_tracks_on, const std::vector<std::string>& layer_costs);

}  // namespace tmk::route

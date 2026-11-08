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

// Preferred track direction of a copper layer (issue #66, D174): orthogonal steps across it cost --layer-dir-pen.
enum LayerDir : std::int8_t { kLayerDirNone = 0, kLayerDirHorizontal = 1, kLayerDirVertical = 2 };

// Per-layer directions and diagonal penalties (`--layer-dir`, `--layer-diag-pen`; doc 05 §29, D174) resolved against a board.
struct LayerDirections {
  std::vector<std::int8_t> dir;        // per copper index (LayerDir); empty = not given (the layer parity of D98 applies)
  std::vector<double> diag_pen;        // per copper index: pitches per 45-degree step; empty = not given
  std::vector<std::string> warnings;   // entries naming a layer the board does not have (ignored)
};

inline constexpr double kMaxDiagPen = 100.0;

// Syntax check without a board, for the command line: `dir_specs` are "LAYER=h|v|none" entries, `diag_specs` are
// "LAYER=P" entries or one bare "P" for every copper layer (0 <= P <= kMaxDiagPen). Throws std::invalid_argument on a
// malformed entry, a bad value or a layer given twice.
void check_layer_dir_specs(const std::vector<std::string>& dir_specs, const std::vector<std::string>& diag_specs);

// The entries resolved against `b` (names as for layer_limits). A layer name the board does not have is a warning and
// the entry is ignored (a board-independent option file may name inner layers); everything check_layer_dir_specs refuses
// still throws. With dir_specs given, layers not named have no preferred direction; with diag_specs given, layers not
// named (and no bare value) have no diagonal penalty.
LayerDirections layer_directions(const model::Board& b, const std::vector<std::string>& dir_specs, const std::vector<std::string>& diag_specs);

}  // namespace tmk::route

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Net length and propagation delay as KiCad 10.0.6 measures them for its `length` and `skew` checks (design doc 05
// §59; KiCad's LENGTH_DELAY_CALCULATION with the DRC's options: via spans, merged tracks, tracks clipped in pads).
#include <cstdint>
#include <vector>

#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::drc {

// The length a via adds to its net in KiCad's length calculation (KiCad 10, BOARD_STACKUP::GetLayerDistance): the
// stackup between the two copper layers the via joins, with the copper of an outer layer in full and of an inner
// layer by half. A board without a stackup block gets KiCad's default one (35 um copper, 10 um per solder mask, the
// rest of the board thickness shared evenly between the dielectrics). Empty (every via 0) when the project turns
// "use height for length calculations" off.
struct ViaHeights {
  std::vector<Coord> from, to;  // per copper layer: where a via starting / ending on it begins / ends, nm from the top
  Coord between(int a, int b) const;  // copper indices, either order; 0 for a == b
};
ViaHeights via_heights(const model::Board& b, const model::DesignRules& rules);

// The items of one net that a length or skew rule matched: indices into the board's pads, tracks, arcs and vias.
struct LengthItems {
  std::vector<int> pads, tracks, arcs, vias;
};

// KiCad's LENGTH_DELAY_STATS. Lengths in nm, delays in attoseconds.
struct NetLength {
  std::int64_t track = 0;  // tracks and arcs, joined into chains and clipped in vias and pads
  Coord via = 0;           // via heights (0 when the project does not use heights)
  Coord die = 0;           // pad-to-die lengths
  int vias = 0, pads = 0;
  std::int64_t track_delay = 0, via_delay = 0, die_delay = 0;
  double total() const { return static_cast<double>(track) + static_cast<double>(via) + static_cast<double>(die); }
  double total_delay() const {
    return static_cast<double>(track_delay) + static_cast<double>(via_delay) + static_cast<double>(die_delay);
  }
};

// Length and delay of `items`, all of one net. `profile` is the tuning profile of the net's class (null: every
// delay is 0, as in KiCad).
NetLength net_length(const model::Board& b, const model::DesignRules& rules, const ViaHeights& vh, const LengthItems& items,
                     const model::TuningProfile* profile);

}  // namespace tmk::drc

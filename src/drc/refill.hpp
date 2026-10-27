// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Zone refill in the engine (doc 05 §36): the fills KiCad 10's zone filler would draw for the board's current
// copper, so connectivity can be judged as `kicad-cli pcb drc --refill-zones` judges it. The fills are not
// written to files.
#include <string>
#include <vector>

#include "geom/clip.hpp"
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

// A thermal relief KiCad's DRC reports as starved_thermal (DRC_TEST_PROVIDER_ZONE_CONNECTIONS::testZoneLayer): fewer
// spokes reach the zone's fill than the board's minimum (min_resolved_spokes, 2 by default), counting tracks that run from
// the pad into the fill; spokes into a fill island that touches nothing but this pad do not count unless the pad has no other.
struct StarvedThermal {
  int pad = -1, zone = -1, layer = -1;  // board indices; layer is a copper index
  int spokes = 0;                       // spokes into fill that connects to something else, plus track spokes
  int island_spokes = 0;                // spokes into an island only this pad touches
};
// On a board whose fills are current (refill_zones' board): every pad KiCad would report, in pad then zone order.
// Not covered: custom-pad spoke primitives and min_resolved_spokes constraints in custom rules (the board setup's applies).
std::vector<StarvedThermal> starved_thermals(const model::Board& filled, const model::DesignRules& r);

// The four spoke polygons of a pad's thermal relief to a zone, as the zone filler draws them (buildThermalSpokes).
std::vector<geom::Ring> thermal_spokes(const model::Pad& pad, const model::Zone& zone);

}  // namespace tmk::drc

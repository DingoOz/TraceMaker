#pragma once
// Escape (fanout) planning, version 1 (roadmap M9; design doc 05 §3).
//
// Dense packages (QFP/TSOP/SOIC rows, BGA/LGA arrays) fail in the router's strict first pass when other nets'
// routes take the channels a pin needs to leave its package: the pin is then "boxed in" until negotiation,
// which large boards often do not reach within the budget. The plan reserves, for every pin that has to be
// routed, a short escape corridor: straight out of the package for pins on its perimeter, and a dog-bone stub to
// the diagonal interstitial via site for the inner balls of an SMD array (the classic BGA fanout; Yan & Wong,
// "Recent research development in PCB layout", ICCAD 2010, §3; Kong, Yan & Wong, "Optimal simultaneous pin
// assignment and escape routing for dense PCBs", ASP-DAC 2010, for the escape model). The router treats
// another net's corridor as blocked in strict passes and as costly in negotiated passes, and releases a corridor
// once its pin is connected. Reservations only remove options: every commit is still checked exactly, so the
// plan cannot introduce violations.
#include <functional>
#include <vector>

#include "model/board.hpp"

namespace tmk::route {

using geom::Point;

struct EscapeCorridor {
  int pad = -1;                  // board pad index
  model::NetId net = 0;
  int layer = -1;                // copper index the stub runs on
  Point a, b;                    // centreline: pad centre -> escape point (or via site)
  Coord band = 0;                // half-width of the reserved strip around the centreline
  bool via = false;              // b is a dog-bone via site (reserved on every layer)
};

struct EscapeOptions {
  int min_pads = 8;              // smaller footprints escape easily
  Coord max_pitch = 1'300'000;   // pin pitch up to which a footprint counts as dense (1.27 mm BGAs included)
  Coord length = 1'000'000;      // corridor length past the pad edge for perimeter pins
};

struct EscapeStats {
  int parts = 0, perimeter = 0, dogbones = 0;
};

// Plans corridors for the pads with needs[pad] != 0. keep(net) is the distance another net's centreline must
// keep from this net's centreline (track width + clearance); the band is that, capped at a share of the pin
// pitch so the corridors of neighbouring pins never overlap. Deterministic: footprints and pads in board order.
std::vector<EscapeCorridor> plan_escapes(const model::Board& b, const std::vector<char>& needs, const std::function<Coord(model::NetId)>& keep,
                                         const EscapeOptions& o = {}, EscapeStats* stats = nullptr);

}  // namespace tmk::route

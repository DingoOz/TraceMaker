// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Fan-out heads (M22 item 8, D157; design doc 05 §65): the first millimetres of each perimeter pin's route out of a dense
// four-sided package, laid as the designer lays them before anything else is routed.
//
// On mini_mass_prog_bench_prog_rig the designer's own copper within 5 mm of the QFN-44 (only the parts that touch one pad),
// put into the unrouted board, let the router reach 990 of 995 connections where it otherwise stops at 986: the other nets
// route round the escapes instead of putting a ring of vias 1-2 mm out that boxes the pins in. Straight stubs from every pin
// did worse (983); what the designer does is to leave each side either straight or in staggered 45-degree bundles toward the
// pins' destinations, and to keep a side straight where the bundles would run into the parts beside the package.
//
// The heads are planar by construction (the ordered escape of Kong, Yan and Wong, "Optimal simultaneous pin assignment and
// escape routing for dense PCBs", ASP-DAC 2010, restricted to one bend per pin): along a side the pins split into a block that
// turns toward the low end, a straight block and a block that turns toward the high end, and each turning pin bends one step
// further out than its neighbour toward the corner, so parallel diagonals keep track width plus clearance apart.
// Every leg is cut at the first point the exact check refuses; a head that ran into something stops short of it, so its own
// route keeps room to leave.
#include <functional>
#include <vector>

#include "model/board.hpp"

namespace tmk::route {

struct FanoutHeadOptions {
  Coord reach = 5'000'000;     // heads run this far past the box of the package's signal pads
  Coord bend0 = 300'000;       // the first bend of a turning block, past the pad's outer end
  Coord step = 100'000;        // each further pin of the block bends this much further out
  Coord back = 500'000;        // a head cut by an obstacle ends this much short of it
  Coord min_length = 400'000;  // shorter heads are not laid
  double turn_deg = 30;        // a pin whose destination is more than this off its side's normal turns toward it
  int min_pins = 24;           // perimeter pins a package needs (with pins on all four sides)
};

struct FanoutHead {
  int pad = -1;
  model::NetId net = 0;
  int layer = -1;
  std::vector<geom::Point> pts;  // pad centre, pad's outer end, bend (if any), end
};

struct FanoutHeadHooks {
  std::function<bool(geom::Point, geom::Point, int, Coord, model::NetId)> free;  // exact check against fixed copper and the heads so far
  std::function<int(geom::Point, geom::Point, int, Coord, model::NetId)> add;    // a head segment: the copper the checks see; returns a handle
  std::function<void(int)> remove;                                               // takes the last added segment out again (LIFO)
  std::function<Coord(model::NetId)> width;
  std::function<Coord(model::NetId)> clearance;
};

// Plans and adds (through hooks.add) the heads of every qualifying package. Pins that get no head: nets with a zone, pins whose
// net has no pad off the package, and pins whose nearest such pad lies within `reach` of the package (local connections are the
// router's). A head never comes within clearance of another pad or other copper of its own net, so it joins nothing: the
// connection plan is unchanged, and removing the heads after routing cannot disconnect a route (each runs pad to pad).
// Deterministic: footprints in board order, sides in a fixed order, pins by position.
std::vector<FanoutHead> plan_fanout_heads(const model::Board& b, const FanoutHeadHooks& h, const FanoutHeadOptions& o = {});

}  // namespace tmk::route

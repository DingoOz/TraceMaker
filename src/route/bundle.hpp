// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Bundle (river) escapes (design doc 05 §50). The connections that leave one side of a dense package (QFN, QFP, SO) for one side of another part are
// routed as one block, the way a designer draws them: parallel straight tracks at the pin pitch, in the order of the pins, for as long as the board has
// room, and each track peels off toward its own pad at the last moment. Not a search per connection: ONE search for the spine of the block (a track as
// wide as the whole block, on an octilinear grid, turns spaced so that the inner tracks of a turn keep their length), then every lane is the spine offset
// by the lane's distance from the middle (rigid offsets: the order of the pins is the order of the lanes, so lanes cannot cross), and a last leg from the
// lane to its pad. A lane is placed only when every piece of it passes the router's own exact check; a connection that does not get a lane is left to
// the ordinary router. Related work: river routing (Hsu, "A new approach to channel routing"; Tsukiyama and Kuh, "Single-layer routing"); the
// generalisation of the router's coupled-pair legs (doc 05 §15) from two tracks to n.
#include <functional>
#include <vector>

#include "geom/point.hpp"
#include "model/board.hpp"

namespace tmk::route {

struct BundleConn {
  int ci = -1;              // the router's connection index
  model::NetId net = 0;
  int pad_a = -1, pad_b = -1;  // board pads; the one on a dense package's perimeter is the source
};

struct BundleHooks {
  // May a track of `net` and `width` run a-b on `layer`? The router's exact check against everything placed (fixed copper, routed copper, rules).
  std::function<bool(geom::Point, geom::Point, int layer, Coord width, model::NetId)> free;
  std::function<Coord(model::NetId)> width;      // track width of a net
  std::function<Coord(model::NetId)> clearance;  // clearance of a net
  std::function<bool(int layer, model::NetId)> layer_ok;
  // A via of `net` at p (the class via, not necked): may it go there? (exact check, strict)
  std::function<bool(geom::Point, model::NetId)> via_ok;
  std::function<Coord(model::NetId)> via_diameter;
  // Places the tunnel of a connection: tracks (layer, polyline) and the vias that join them. False: refused (nothing placed).
  std::function<bool(const BundleConn&, const std::vector<std::pair<int, std::vector<geom::Point>>>&, const std::vector<geom::Point>&)> place_tunnel;
  // Places the lane of a connection: the polyline from the centre of one pad to the centre of the other, on `layer`. False: refused (not placed).
  std::function<bool(const BundleConn&, int layer, const std::vector<geom::Point>&)> place;
};

struct BundleOptions {
  int min_lanes = 3;             // a group of fewer connections is left to the router
  Coord grid = 500'000;          // spine grid step
  Coord peel_step = 250'000;     // places along a lane where it may leave the block
  Coord extend = 30'000'000;     // how far a lane may run on straight past the end of the spine
  Coord max_pitch = 1'300'000;   // pin pitch up to which a footprint counts as a dense package (as the escape plan)
  int min_pads = 8;
};

struct BundleStats {
  int groups = 0;       // (package side, destination side) groups with enough connections
  int spines = 0;       // groups for which the search found a spine
  int lanes = 0;        // connections in those groups
  int placed = 0;       // lanes placed
};

// Pins on the perimeter of a dense package and the direction they leave in (index into the 8 octilinear directions: 0 +x, 2 +y, 4 -x, 6 -y).
// Corner pins (within the ring of two sides) and thermal pads are not perimeter pins.
std::vector<int> perimeter_directions(const model::Board& b, const BundleOptions& o);  // by pad index; -1 = not a perimeter pin

// Package tunnels (doc 05 §52): a connection between two perimeter pins of one dense package on the same net (the core rail's three pins, the supply pins
// of a package) goes where the designer puts it: a short stub and a via straight out of each pin, and the run between the two vias on the other layer,
// under the package. The run is one search (A*, octilinear, exact checks) between the two via sites; nothing is placed unless the whole tunnel passes.
struct TunnelStats {
  int tried = 0, placed = 0;
};
TunnelStats route_tunnels(const model::Board& b, const std::vector<BundleConn>& conns, const BundleHooks& h, const BundleOptions& o = {});

// Plans and places the bundles of `conns` (unrouted two-pad connections). Deterministic: groups, lanes and searches are in a fixed order.
BundleStats route_bundles(const model::Board& b, const std::vector<BundleConn>& conns, const BundleHooks& h, const BundleOptions& o = {});

}  // namespace tmk::route

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Style metric (doc 05 §39.2): how tidy a set of tracks is, per connection and per board. It only measures; no
// pass optimises it. Everything is counted in integers (nm, cross and dot products), so the numbers are the same
// on every machine and at every thread count.
//
// The patterns follow what layout reviewers flag by eye; tscircuit's pcb-trace-linter (segments off the 45 degree
// directions) and check-ugly-traces (removable jogs) were read for which patterns to count. No code of theirs is
// used.
#include <vector>

#include <nlohmann/json.hpp>

#include "model/board.hpp"

namespace tmk::passes {

struct StyleOptions {
  // A stair-step is a run of at least `stair_min_segments` consecutive segments on one layer, each shorter than
  // `stair_widths` track widths, whose bends turn alternately left and right.
  int stair_widths = 4;
  int stair_min_segments = 3;
  // A connection is a long detour when its copper is longer than this many per cent of the shortest octilinear
  // path between its ends.
  int long_detour_pc = 150;
};

// One connection: copper of one net from one end to the next. An end is a pad of the net, a junction (three or
// more track ends, or a track end on the middle of another track) or a loose end; a via with exactly two track
// ends is passed through.
struct ConnectionStyle {
  model::NetId net = 0;
  model::Point a, b;         // the two ends
  int layer_a = -1, layer_b = -1;
  int segments = 0;          // after joining collinear pieces
  int bends = 0;             // direction changes between consecutive segments on one layer
  int acute = 0;             // bends sharper than 90 degrees (the two segments enclose less than 90 degrees)
  int stair_steps = 0;       // runs, see StyleOptions
  int short_segments = 0;    // segments shorter than their own width
  int off_angle = 0;         // segments more than 0.1 degree off the eight octilinear directions
  int arcs = 0;              // arc tracks (not judged for bends or angles)
  int vias = 0;              // layer changes passed through
  Coord length = 0;          // nm, each piece rounded to the nanometre
  Coord octile = 0;          // shortest octilinear distance between the ends, nm (0 for a closed loop)
};

struct StyleReport {
  std::vector<ConnectionStyle> connections;  // sorted by net, then end points
  int segments = 0, bends = 0, acute = 0, stair_steps = 0, short_segments = 0, off_angle = 0, arcs = 0, vias = 0;
  int long_detours = 0;
  Coord length = 0;
  Coord detour_length = 0, detour_octile = 0;  // summed over the connections with distinct ends
  double bends_per_connection() const { return connections.empty() ? 0.0 : static_cast<double>(bends) / static_cast<double>(connections.size()); }
  double detour_ratio() const { return detour_octile > 0 ? static_cast<double>(detour_length) / static_cast<double>(detour_octile) : 1.0; }
};

// Measures `tracks`, `arcs` and `vias` (any subset of a board's copper, e.g. only what a run added). `b` supplies
// the pads that end connections. The result does not depend on the order of the three lists.
StyleReport measure_style(const model::Board& b, const std::vector<model::Track>& tracks, const std::vector<model::ArcTrack>& arcs,
                          const std::vector<model::Via>& vias, const StyleOptions& opt = {});

// The `style` object of the route summary and of `tracemaker style --json`. `per_connection` adds one row per
// connection.
nlohmann::json style_json(const model::Board& b, const StyleReport& r, bool per_connection = false);

}  // namespace tmk::passes

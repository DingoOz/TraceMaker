// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Power track widening (doc 05 §39.1): after routing, new tracks that are narrower than they should be are made
// wider where the exact check allows it. Only the width of new, unlocked tracks changes: nothing is moved, and no
// other copper is touched.
#include <map>
#include <string>
#include <vector>

#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::passes {

struct WidenOptions {
  // Tracks narrower than their net class's width (the router's neck-down) go back up to the class width.
  bool restore_necked = true;
  // Width targets by net (explicit options and component rules); a target below a track's width changes nothing.
  // The largest target of a net counts.
  struct Target {
    model::NetId net = 0;
    Coord width = 0;
    std::string source;  // "option", or the component rule that asked for it
  };
  std::vector<Target> targets;
  // Nets left alone: coupled differential pairs are routed at their pair width on purpose.
  std::vector<model::NetId> skip_nets;
  // Candidate widths per track: the target, then `steps` - 1 evenly spaced widths below it, down to (not including)
  // the track's own width, each rounded down to a micrometre. The widest that passes the exact check is kept.
  int steps = 8;
  // Work budget in exact checks (rule 5). When it runs out the remaining tracks keep their width.
  long work_limit = 2'000'000;
  bool soft_zones = false;  // as RouterOptions::soft_zones: stale fills do not block (the caller refills and judges)
};

struct WidenNet {
  model::NetId net = 0;
  Coord target = 0;
  std::string source;                   // "net class" (neck-down restored), "option", or a rule id
  std::map<Coord, Coord> before, after; // track width -> length at that width, nm
  int tracks = 0;                       // tracks below the target before the pass
  int widened = 0;                      // of them: made wider (to the target or part of the way)
  int at_target = 0;                    // of them: at the target (or at a pad's or via's size, see the doc) afterwards
  Coord widened_length = 0;             // length of the tracks made wider
};

struct WidenResult {
  std::vector<WidenNet> nets;           // nets with at least one track below its target, by net id
  long checks = 0;                      // exact checks done (the pass's work)
  bool out_of_work = false;
  int widened = 0;
};

// `board` is the board as the router saw it (without the new copper); `tracks` and `vias` are the run's new
// copper. On return the widths in `tracks` are the widened ones; order and end points are unchanged.
WidenResult widen_tracks(const model::Board& board, const model::DesignRules& rules, std::vector<model::Track>& tracks,
                         const std::vector<model::Via>& vias, const WidenOptions& opt);

// Pad necks for tracks routed wide (D184): a new track wider than a pad of its net that it ends in is split, and the piece at the pad gets
// the pad's smaller side as its width (never less than `floor` of its net, the width the router would have used), so the wide copper
// starts just outside the pad, as a designer draws it. The neck runs from the end point for half the pad's larger side plus 0.2 mm (the
// whole track if it is shorter). Only tracks at 0, 45 or 90 degrees are split, so the split point lies exactly on the track and the result
// is copper the track already covered: no clearance can get worse and nothing needs checking again. Returns the number of necks made.
int neck_at_pads(const model::Board& board, std::vector<model::Track>& tracks, const std::vector<Coord>& floor_by_net);

}  // namespace tmk::passes

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Octilinear clean-up (doc 05 §80, D187, M23 E3): a new track that runs off the eight directions (0, 45, 90, ... degrees) is replaced by
// a dogleg of two tracks that are on them, through one of the two points that split it into a diagonal and a straight leg. A dogleg is
// kept only if both legs are free in the router's exact check, against everything including the copper already changed, and if neither
// end makes an acute corner with the copper it joins. Ends do not move, so connectivity is unchanged.
#include <vector>

#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::passes {

struct OctilinearOptions {
  long work_limit = 2'000'000;  // exact checks (rule 5); when it runs out the remaining tracks stay as they are
  bool soft_zones = false;      // as RouterOptions::soft_zones
  // A leg shorter than this many track widths would be a jog of its own (the style metric's short segment): such a dogleg is not
  // used. 0 allows any length.
  double min_leg_widths = 0.5;
};

struct OctilinearResult {
  int off_angle = 0;    // new, unlocked tracks off the eight directions before the pass
  int replaced = 0;     // of them: replaced by a dogleg
  int no_room = 0;      // a dogleg that fits nowhere (both candidates failed the exact check)
  int short_leg = 0;    // skipped: a leg would be shorter than min_leg_widths
  int acute = 0;        // skipped: a dogleg would make an acute corner with its neighbours
  long checks = 0;
  bool out_of_work = false;
};

// `board` is the board as the router saw it (without the new copper); `tracks` and `vias` are the run's new copper. On return
// `tracks` holds the doglegs in place of the tracks they replace (each replaced track becomes two, in its place in the order).
OctilinearResult octilinear_tracks(const model::Board& board, const model::DesignRules& rules, std::vector<model::Track>& tracks,
                                   const std::vector<model::Via>& vias, const OctilinearOptions& opt);

// True when a→b is more than 0.1 degree off the eight directions (the style metric's test, passes/style.cpp).
bool off_octilinear_segment(model::Point a, model::Point b);

}  // namespace tmk::passes

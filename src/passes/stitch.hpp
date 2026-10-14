// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Stitching vias (doc 05 §39.3): after routing, vias of a plane's net are added where fills of that net lie over
// each other on two or more layers (plane stitching), and next to the vias of signals that need a reference plane
// (return-path stitching). Every via passes the router's exact check against the refilled zones, and is kept only
// if a second refill still joins it to its net's fill on two layers.
#include <string>
#include <vector>

#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::passes {

struct StitchOptions {
  bool planes = false;          // plane stitching on a grid
  // Grid pitch. The default is the catalogue's general high-speed value (TI SPRAAR7J: a stitching via within
  // 200 mil); app code passes the catalogue's number, this is its fallback.
  Coord pitch = 5'080'000;
  // Return-path stitching: every via of `signal` wants a via (or plated through-hole pad) of `plane` within
  // `distance`, centre to centre.
  struct ReturnNet {
    model::NetId signal = 0, plane = 0;
    Coord distance = 5'080'000;
    std::string source;         // "option" or the component rule (e.g. "USB2-09 J1")
  };
  std::vector<ReturnNet> returns;
  // Work budget (rule 5): sites examined plus exact checks. When it runs out no further site is tried.
  long work_limit = 2'000'000;
};

struct StitchReturn {
  model::NetId signal = 0, plane = 0;
  std::string source;
  Coord distance = 0;
  int signal_vias = 0;          // vias of the signal net on the board (old and new)
  int stitched_before = 0;      // of them: with a plane via or plated pad within the distance before the pass
  int stitched_after = 0;       // ... and after it
};

struct StitchResult {
  std::vector<model::Via> vias;            // the vias to add, plane stitches first; all marked free
  std::vector<std::uint8_t> return_path;   // per via: 1 = placed for a signal via, 0 = grid
  std::vector<StitchReturn> returns;       // one row per StitchOptions::returns entry
  int sites = 0;                // grid sites inside overlapping fills
  int site_served = 0;          // of them: already tied by a via or plated pad of the net within half the pitch
  int site_blocked = 0;         // of them: refused by the exact check
  int dropped = 0;              // vias placed and then removed because the second refill did not join them on two layers
  long checks = 0;              // the pass's work
  bool out_of_work = false;
  bool discarded = false;       // everything withdrawn: the refill with the vias left another net's zone in more pieces
  std::vector<std::string> warnings;
};

// `board` holds the routed copper (the run's new tracks and vias appended). Zone fills on it may be stale: the
// pass refills them in the engine (drc/refill.hpp) and places against that.
StitchResult stitch_vias(const model::Board& board, const model::DesignRules& rules, const StitchOptions& opt);

}  // namespace tmk::passes

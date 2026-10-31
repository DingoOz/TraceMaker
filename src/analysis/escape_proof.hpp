// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Why a connection stays open (doc 20 §2.4): can a track get out of a pin at all, past the board's fixed copper?
//
// A track of width w whose centre keeps w/2 + c from every item of another net (c the rule clearance between the track and
// that item) lives in the free space of the plane less those items, each grown by its own w/2 + c (the configuration space of
// a disc; Lozano-Pérez, "Spatial planning: a configuration space approach", 1983). Two items whose copper gap is less than
// w + c1 + c2 close the way between them. The pin is enclosed on a layer when the free space reachable from its copper
// neither reaches other copper of the net nor leaves a window around the pin. Over every layer, and with no via site
// (a via clear on all its layers) in the reachable space, no router can connect it as the parts are placed.
//
// The free space is sampled on a grid, and a cell counts as free when its centre is within half a cell diagonal of being
// free: the sampled space is a superset of the true one, so "enclosed" is a proof; "not enclosed" is not a promise.
// Only the board's own copper counts (pads, existing tracks and vias, zone fills unless soft, holes, the edge). Copper on the pin
// itself is no wall: it is the pin's own pour, or a short already there. Net-less copper graphics on it are walls to the router
// (without --soft-copper-graphics), and a pin walled in only by them or by zone fills is reported as such, not as enclosed: the routed
// copper of other nets is what a router may move, so a pin that is not enclosed here is congested, not blocked.
#include <string>
#include <vector>

#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::analysis {

struct EscapeOptions {
  Coord window = 1'500'000;  // how far beyond the pin's copper the search goes (a track that gets that far is out)
  Coord cell = 10'000;       // grid step
  bool soft = false;         // zone fills, and net-less copper graphics on the pin, are no walls (route --soft-zones, --soft-copper-graphics)
  bool via_in_pad = false;   // a via may sit in the pin's own pad (route --via-in-pad)
};

struct PinEscape {
  int pad = -1;                      // board pad index
  bool analysed = false;             // false: the pad has no copper or no net, nothing was decided
  bool enclosed = false;             // proved: no track of the board's minimum width gets out on any layer, no minimum via fits
  std::vector<int> layers_enclosed;  // copper layers on which the pin is enclosed (all of its layers when `enclosed`)
  Coord width = 0, clearance = 0;    // the net's track width and clearance used
  // Walled in for the class's track and via but not for the board's minimum ones (a class's sizes are defaults, not limits): not
  // `enclosed`. neck_width > 0: the widest track that gets out; small_via_fits: no track gets out, a smaller via fits.
  Coord neck_width = 0;
  bool small_via_fits = false;
  bool via_in_pad_fits = false;      // a via would fit in the pin's own pad (route --via-in-pad)
  bool soft_wall = false;            // zone fills, or net-less copper graphics on the pin, are among the walls
  bool soft_only = false;            // enclosed only while those block (route --soft-zones, --soft-copper-graphics would free it); not `enclosed`
  std::vector<std::string> walls;    // what bounds the space: pads ("U3.6"), "the GND zone fill", "the board edge" (nearest first, at most 6)
  std::string text;                  // one sentence for the user
};

// Builds the board's copper and rules once; `run` analyses one pin of a connection that stayed open.
class EscapeAnalyser {
 public:
  // Builds the copper model and rule engine of `board`; the board and rules must outlive the analyser.
  EscapeAnalyser(const model::Board& board, const model::DesignRules& rules, EscapeOptions opt = {});
  ~EscapeAnalyser();
  EscapeAnalyser(const EscapeAnalyser&) = delete;
  EscapeAnalyser& operator=(const EscapeAnalyser&) = delete;
  // Analyses board pad `pad` (an index into board.pads). Deterministic; const, but not safe to call from several threads at once.
  PinEscape run(int pad) const;

 private:
  struct Impl;
  Impl* impl_;
};

// "REF.NUM" of a board pad.
std::string pad_label(const model::Board& board, int pad);

}  // namespace tmk::analysis

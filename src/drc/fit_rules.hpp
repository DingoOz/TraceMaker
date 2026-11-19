// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Rule fit (route --fit-rules, doc 20 §2.8, D194): relax the rules a board's own copper already breaks, to what that copper has.
//
// A board whose pads or existing tracks break a rule cannot be routed under that rule the way it was laid out. The designer-oracle test
// (bench/records/oracle-2026-11) found this on boards that never complete: card-edge connector pads inside the copper-to-edge clearance,
// tracks closer to holes than the hole clearance, tracks narrower than the minimum width. All PCBench boards are in the KiCad 5 format,
// which stores no edge or hole clearance, so KiCad 10's defaults (0.5 mm and 0.25 mm) hold for boards designed without them.
//
// The fit runs the DRC on a reference board (the input, or an earlier layout of it), takes for every fitted rule the smallest value its
// violations measure (for the clearance, their low decile), and lowers the rule to that value, never below a floor. With tracks on the reference it also lowers each net
// class's track width and via size to what the reference's tracks of that class use. It only ever relaxes, and every change is reported.
// The output then breaks the board's stated rules where the fit relaxed them: KiCad's DRC reports those, as it does for the reference.
#include <string>
#include <vector>

#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::drc {

struct FitOptions {
  Coord clearance_floor = 90'000;  // no clearance is fitted below this (a common fab limit for multilayer boards)
  Coord width_floor = 90'000;      // nor a track width
  Coord edge_floor = 0;            // copper may reach the edge when the reference's does (card-edge fingers)
  Coord hole_floor = 0;
  Coord annular_floor = 50'000;
  Coord via_floor = 300'000;       // via diameter
  Coord drill_floor = 150'000;
  double clearance_quantile = 0.1;  // the clearance is fitted to this quantile of the measured gaps, the other rules to the smallest
  int min_evidence = 3;             // fewer violations of a kind are taken for slips and fit nothing
};

struct FittedRule {
  std::string rule;   // KiCad's name of the minimum, or "class NAME track_width" / "class NAME via"
  Coord from = 0, to = 0;
  int evidence = 0;   // violations on the reference (minimums), or its distinct track widths / its vias of the class
  std::string note;
};

struct FitReport {
  std::vector<FittedRule> changes;
  int violations = 0;  // the reference's DRC errors of the fitted kinds, before the fit
};

// Fits `rules` to `reference` (see above). Deterministic: the changes are in a fixed order (minimums, then classes in file order).
FitReport fit_rules(const model::Board& reference, model::DesignRules& rules, const FitOptions& opt = {});

}  // namespace tmk::drc

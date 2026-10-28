// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Coupled segments of a differential pair, as KiCad 10.0.6's diff-pair coupling test finds them
// (pcbnew/drc/drc_test_provider_diff_pair_coupling.cpp, doc 05 §59): a track of the P net and one of the N net on the same
// layer are coupled where they run on parallel, distinct lines and their projections overlap. The gap is measured between the
// overlapping parts, edge to edge. Integer arithmetic follows KiCad's SEG (rounded rescale, truncated square root), so the
// gap is KiCad's to the nanometre.

#include <optional>

#include "model/board.hpp"

namespace tmk::drc {

struct Coupling {
  model::Point pa, pb, na, nb;  // the overlapping parts of the P and N centrelines
  model::Point near_p, near_n;  // their nearest points
  Coord gap = 0;                // edge to edge: centreline distance minus the half widths
  Coord length = 0;             // length of the N part, rounded
};

// The two centrelines (a P track from p0 to p1 of width wp, an N track likewise) coupled as KiCad couples them, or nothing: both
// longer than about 1 nm, exactly parallel (a zero cross product) and not collinear, with overlapping projections.
std::optional<Coupling> couple(model::Point p0, model::Point p1, Coord wp, model::Point n0, model::Point n1, Coord wn);

}  // namespace tmk::drc

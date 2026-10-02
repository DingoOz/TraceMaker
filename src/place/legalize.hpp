#pragma once
// (D) Legalisation (design doc 04 §3 D): Tetris-style greedy, largest parts first, each part moved to the
// nearest legal position (minimum Euclidean displacement over a lattice searched in rings) from its target.
// Candidates are screened with the conservative occupancy raster and always confirmed by the exact test.
#include <string>

#include "place/legality.hpp"

namespace tmk::place {

struct LegaliseStats {
  int placed = 0;          // parts that were (re)placed
  int kept = 0;            // parts already legal and left untouched (only_illegal mode)
  int failed = 0;          // parts with no legal position found (left at their original position)
  double max_disp_mm = 0, mean_disp_mm = 0;
  long exact_checks = 0, raster_checks = 0;
  long evictions = 0;        // rip-up-and-re-place steps (full mode)
  long raster_disagree = 0;  // raster said free but the exact test failed (must stay 0: the raster is conservative)
  std::vector<std::string> failures;
};

// `only_illegal`: parts that are already legal (inside the board, conflict-free) stay where they are and only
// the others are re-placed (refine mode). `cell` is the raster cell (0 = automatic, 0.05–0.1 mm).
LegaliseStats legalise(const Problem& p, Placement& pl, bool only_illegal, Coord cell = 0);

}  // namespace tmk::place

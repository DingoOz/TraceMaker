// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Positional custom-rule predicates. Shapes are tested as rounded cores, not boxes.
#include <array>
#include <string>

#include "drc/copper.hpp"

namespace tmk::drc {

// Requires a common copper layer. Outline rings use even/odd filling (holes and
// disjoint contours); enclosed requires every copper shape to be wholly inside.
// The caller selects/caches zones; insideArea and intersectsArea both pass false.
bool area_matches(const CopperItem& item, const model::Zone& area, bool enclosed);
struct CourtyardRegion {
  std::vector<std::vector<geom::Point>> rings;
  geom::Box box;
};
struct CourtyardEntry {
  std::string reference;
  std::array<CourtyardRegion, 2> sides;  // physical front, back
};
struct CourtyardCache {
  std::vector<CourtyardEntry> entries;
};

// Build once per board; invalid/unclosed contours leave the corresponding side empty.
CourtyardCache build_courtyards(const model::Board& board);

// Function is intersectsCourtyard/intersectsFrontCourtyard/intersectsBackCourtyard.
// Selects physical F/B courtyard layers independently of the item's copper side.
// Prepared geometry and reference matching perform no per-query allocations.
bool courtyard_matches(const CopperItem& item, const CourtyardCache& courtyards,
                       const std::string& ref_pattern, const std::string& function);
}  // namespace tmk::drc

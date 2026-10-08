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
  std::string reference, lib_id;
  bool back = false;                     // footprint flipped to the bottom side
  std::array<CourtyardRegion, 2> sides;  // physical F.CrtYd, B.CrtYd
};
struct CourtyardCache {
  std::vector<CourtyardEntry> entries;
};

// Build once per board; invalid/unclosed contours leave the corresponding side empty.
CourtyardCache build_courtyards(const model::Board& board);

// KiCad's footprint selector (testFootprintSelector): a reference wildcard, or, when the selector contains ':',
// a library-id wildcard. Case-sensitive, like wxString::Matches.
bool footprint_selected(const std::string& selector, const std::string& reference, const std::string& lib_id);

// Function is intersectsCourtyard/intersectsFrontCourtyard/intersectsBackCourtyard. Front and back are the
// footprint's own sides (KiCad: GetCourtyard(fp->IsFlipped() ? B_Cu : F_Cu)): the front courtyard of a flipped
// footprint is its B.CrtYd outline. The item's copper side does not matter.
bool courtyard_matches(const CopperItem& item, const CourtyardCache& courtyards,
                       const std::string& selector, const std::string& function);
// The same test against entries resolved once by courtyard_entries (the rule engine binds each selector, so
// queries perform no allocations or name matching).
std::vector<int> courtyard_entries(const CourtyardCache& courtyards, const std::string& selector);
bool courtyard_matches(const CopperItem& item, const CourtyardCache& courtyards,
                       const std::vector<int>& entries, const std::string& function);
}  // namespace tmk::drc

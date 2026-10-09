// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Positional custom-rule predicates. Shapes are tested as rounded cores, not boxes.
#include <array>
#include <map>
#include <string>
#include <vector>

#include "drc/copper.hpp"

namespace tmk::drc {

// Requires a common copper layer. Outline rings use even/odd filling (holes and
// disjoint contours); enclosed requires every copper shape to be wholly inside.
// The caller selects/caches zones; insideArea and intersectsArea both pass false.
bool area_matches(const CopperItem& item, const model::Zone& area, bool enclosed);
struct CourtyardRegion {
  // The courtyard is the union of the parts. The rings of one part use even/odd filling (a ring inside another
  // is a hole); outlines that overlap or touch are parts of their own, as KiCad merges them.
  std::vector<std::vector<std::vector<geom::Point>>> parts;
  geom::Box box;
  // The side has courtyard graphics, but they are not closed, simple outlines (or use a Bezier curve): no
  // parts, and the rule engine warns that courtyard conditions cannot match this footprint.
  bool unreadable = false;
};
struct CourtyardEntry {
  std::string reference, lib_id;
  bool back = false;                     // footprint flipped to the bottom side
  std::array<CourtyardRegion, 2> sides;  // physical F.CrtYd, B.CrtYd
};
struct CourtyardCache {
  std::vector<CourtyardEntry> entries;
  // Index: the entries a footprint selector picks, in entry order. A selector that is not indexed is matched
  // against every footprint for each query (the reference path, with identical results).
  std::map<std::string, std::vector<std::size_t>> selected;
};

// Build once per board; invalid/unclosed contours leave the corresponding side empty and marked unreadable.
CourtyardCache build_courtyards(const model::Board& board);

// Resolves `selector` once: a rule is evaluated for every pair of items, and matching the selector against
// every footprint each time made a courtyard clearance rule dominate the DRC of a large board.
void index_courtyard_selector(CourtyardCache& courtyards, const std::string& selector);

// KiCad's footprint selector (testFootprintSelector): a reference wildcard, or, when the selector contains ':',
// a library-id wildcard. Case-sensitive, like wxString::Matches.
bool footprint_selected(const std::string& selector, const std::string& reference, const std::string& lib_id);

// Function is intersectsCourtyard/intersectsFrontCourtyard/intersectsBackCourtyard. Front and back are the
// footprint's own sides (KiCad: GetCourtyard(fp->IsFlipped() ? B_Cu : F_Cu)): the front courtyard of a flipped
// footprint is its B.CrtYd outline. The item's copper side does not matter.
// Prepared geometry and selector matching perform no per-query allocations.
bool courtyard_matches(const CopperItem& item, const CourtyardCache& courtyards,
                       const std::string& selector, const std::string& function);
}  // namespace tmk::drc

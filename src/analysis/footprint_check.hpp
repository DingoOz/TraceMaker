// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Footprint check against datasheet land patterns (issue #5, docs/05-routing.md §43, D115).
//
// For each footprint of a board the check finds the reference (footprint_ref.hpp) made for it: by manufacturer part number, library id, value or
// designator. A reference from a brand that is not on the trusted list gives no verdict (the footprint is reported as `untrusted`: nobody vouched for
// that datasheet). With a trusted reference the footprint's pads are compared with the datasheet's: the pattern is first aligned to the footprint
// (a rotation by a multiple of 90 degrees, a mirror, and a translation: the footprint's origin and orientation are the library's choice, not
// a property of the land pattern), then every pad is checked for number, position, size, drill and type. Whatever differs is an error.
//
// A mirror is not free: the pads of a footprint on the front must match the datasheet as drawn, those of a footprint on the back mirrored (that is
// what flipping a footprint does). A pattern that fits only the other way round is reported as mirrored: the pin numbers run the wrong way.
//
// The check is read-only and geometric. It verifies the footprint against the record somebody made from the datasheet; it does not read the datasheet.
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "analysis/footprint_ref.hpp"
#include "geom/point.hpp"
#include "model/board.hpp"

namespace tmk::analysis {

enum class Severity { Info, Warning, Error };

struct FootprintIssue {
  Severity severity = Severity::Error;
  std::string kind;       // pad_count, missing_pad, extra_pad, position, size, drill, pad_type, orientation, mirrored, pattern, no_reference, untrusted_source
  std::string pad;        // the pad number, "" = the footprint as a whole
  std::string message;
  std::string label;      // a few words for a mark on the board ("pad 3: 0.15 mm off")
  geom::Point at;         // where to look on the board (nm)
  double actual_mm = -1;  // the measured value where there is one (the deviation of a position, a size ...)
  double expected_mm = -1;
};

enum class FootprintStatus { Ok, Mismatch, Unverified, Untrusted, Skipped };

struct FootprintResult {
  int footprint = -1;
  std::string ref, lib_id, value;
  std::vector<std::string> mpn;
  geom::Point at;
  FootprintStatus status = FootprintStatus::Unverified;
  // The reference that was used (Ok, Mismatch) or found but not trusted (Untrusted).
  std::string ref_id, brand, part, document, revision, figure, url;
  int page = 0;
  int other_matches = 0;      // further references that also matched and were not used
  int rotation = 0;           // the alignment found: degrees the datasheet pattern is turned to meet the footprint
  bool mirrored = false;
  int pads_board = 0, pads_ref = 0, pads_matched = 0;
  double max_position_mm = 0; // the largest deviation of a matched pad after alignment
  std::vector<FootprintIssue> issues;
  int errors() const;
  int warnings() const;
};

struct FootprintCheckOptions {
  double position_tol_mm = 0.05;  // a reference may set its own
  double size_tol_mm = 0.05;
  double drill_tol_mm = 0.05;
  std::vector<std::string> only;     // designator patterns to check (empty = all)
  std::vector<std::string> require;  // designator patterns that must have a trusted reference: without one it is an error
  std::vector<std::string> mpn_fields;  // property names (besides the usual MPN ones) that hold the manufacturer part number
};

struct FootprintCheckReport {
  std::vector<FootprintResult> results;  // board order
  int checked = 0, ok = 0, mismatched = 0, unverified = 0, untrusted = 0, skipped = 0;
  int errors = 0, warnings = 0;
  std::vector<std::string> notes;        // what the run could not do or chose to do (trust everything, ambiguous matches ...)
  FootprintCheckOptions options;
  int references = 0;
  std::string trusted_source;            // the trusted list's file, "" = none
  bool trust_all = false;
};

// Deterministic: board order, reference order, no hash iteration.
FootprintCheckReport check_footprints(const model::Board& b, const RefLibrary& lib, const FootprintCheckOptions& opt);

// The report as JSON (millimetres), one `footprint_check` viewer message (docs/13-viewer-protocol.md), and the text lines (problems first; `all` adds
// the footprints that are fine or have no reference).
nlohmann::json footprint_check_json(const model::Board& b, const FootprintCheckReport& r);
std::string footprint_check_message(const FootprintCheckReport& r);
std::vector<std::string> footprint_check_lines(const FootprintCheckReport& r, bool all);

}  // namespace tmk::analysis

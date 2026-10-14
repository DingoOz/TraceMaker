// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Datasheet land-pattern references and the list of brands whose datasheets are trusted (issue #5, docs/05-routing.md §43, D115).
//
// TraceMaker does not read datasheets: a PDF drawing is not data. A reference is a record somebody made from a datasheet (typed in from its
// recommended land pattern, or the manufacturer's own KiCad footprint) together with where it came from: the brand, the document, the page. The check
// (footprint_check.hpp) compares a board's footprints with these records, and only with the ones whose brand is on the trusted list: a record
// from a source nobody vouched for gives no verdict, it is reported as unverified.
//
// A reference library is a directory (or one file) of JSON:
//   { "format": "tracemaker-footprint-refs/1",
//     "references": [ { "brand": "Example Semiconductor", "part": "EX1234", "package": "SOIC-8",
//                       "datasheet": { "document": "EX1234 datasheet", "revision": "C", "page": 21, "figure": "Land pattern", "url": "..." },
//                       "match": { "mpn": ["EX1234*"], "lib_id": ["*SOIC-8*"], "value": ["EX1234"], "ref": ["U*"] },
//                       "pads": [ { "number": "1", "type": "smd", "shape": "rect", "at": [-2.7, -1.905], "size": [1.55, 0.6], "angle": 0 }, ... ]
//                       or "footprint": "path/to/RGT0016C.kicad_mod" (relative to the JSON file),
//                       "tolerance_mm": { "position": 0.05, "size": 0.05, "drill": 0.05 } } ] }
// Millimetres, KiCad's y-down footprint frame. Unknown keys are errors (a misspelt tolerance must not be ignored by a verification tool).
#include <string>
#include <vector>

#include "model/board.hpp"

namespace tmk::analysis {

struct RefPad {
  std::string number;
  model::PadType type = model::PadType::Smd;
  model::PadShape shape = model::PadShape::Rect;
  double x = 0, y = 0;          // footprint-local centre, nm (the datasheet's own frame)
  double angle = 0;             // degrees
  double w = 0, h = 0;          // nm
  double drill_w = 0, drill_h = 0;  // nm, 0 = no hole
};

struct FootprintRef {
  std::string id;               // "file#index" or "file#part": what messages call it
  std::string brand, part, package;
  std::string document, revision, url, figure;
  int page = 0;
  // Which board footprints the record is for: every non-empty list must be satisfied by one of its patterns (KiCad wildcards; mpn and value
  // ignore case, lib_id and the designator do not).
  std::vector<std::string> mpn, lib_id, value, ref;
  std::vector<RefPad> pads;
  double position_tol_nm = -1, size_tol_nm = -1, drill_tol_nm = -1;  // -1 = the run's default
  std::string source;           // the file it came from
  int specificity() const { return (mpn.empty() ? 0 : 1) + (lib_id.empty() ? 0 : 1) + (value.empty() ? 0 : 1) + (ref.empty() ? 0 : 1); }
};

// Brands whose datasheets are trusted. One line per brand in the file: `Canonical name | alias | alias`, `#` starts a comment. Names are compared
// without case, punctuation or corporate suffixes ("Texas Instruments Inc." = "texas instruments").
struct TrustedBrands {
  bool trust_all = false;       // --trust-all: no list, everything counts (a deliberate choice, said so in the report)
  std::vector<std::vector<std::string>> groups;  // normalised names, canonical first
  std::string source;           // the file, "" = none given
  bool trusts(const std::string& brand) const;
};

struct RefLibrary {
  std::vector<FootprintRef> refs;  // files in name order, entries in file order: the order is part of the result
  TrustedBrands trusted;
  std::vector<std::string> files;  // what was read
};

// `path` is a directory (every *.json below it, in path order; a trusted_brands.txt beside the references is the trusted list unless one is given) or one
// JSON file. Throws std::runtime_error with the file and the key on anything it cannot take.
RefLibrary load_references(const std::string& path, const std::string& trusted_brands_file = "");
TrustedBrands load_trusted_brands(const std::string& file);
TrustedBrands parse_trusted_brands(const std::string& text, const std::string& source);
// The pads of a footprint file (.kicad_mod, or the text of one) in its own frame.
std::vector<RefPad> pads_from_kicad_mod(const std::string& text);
// "Texas Instruments Inc." -> "texas instruments"
std::string normalise_brand(const std::string& brand);

}  // namespace tmk::analysis

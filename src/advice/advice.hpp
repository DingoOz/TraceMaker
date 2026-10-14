// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The advice file (design doc 17, issue #26): typed hints for the router, written by a person, a script or a language
// model, and read by the engine like any other input. The engine never talks to a model; a model is one possible writer
// of this file.
//
//   {
//     "version": 1,
//     "note": "free text, logged and never acted on",
//     "variants": 4,                                       // advised variants added to the portfolio (default 4)
//     "first_nets": ["USB_DP", "USB_DM"],                  // routed first; other nets do not rip them
//     "priority": [["U1.A1", "J2.3"]],                     // connections (pad to pad, either order) routed first
//     "layers": {"no_tracks": ["In2.Cu"],                  // no new tracks on these copper layers (vias still pass)
//                "cost": {"F.Cu": 1.5}},                   // track cost factor per layer (1 to 1000)
//     "regions": [{"name": "north channel",                // a region of the board (millimetres, the board file's axes)
//                  "rect": [10, 10, 20, 14],               //   or "polygon": [[x, y], ...]
//                  "layers": ["F.Cu"],                     //   default: every copper layer
//                  "reserve_for": ["NET1", "NET2"],        //   only these nets may put copper there ...
//                  "avoid_nets": ["NET3"],                 //   ... or: these nets may not (exactly one of the two)
//                  "tracks": true, "vias": true}]          //   which copper is kept out (both by default)
//   }
//
// Advice never contains copper and never changes a rule's value: it changes the order of the connections, the cost of
// a layer, and where some nets may not go, all inside the router's search. Every route is still judged by the exact check,
// and the advised attempts run as extra portfolio variants next to the unadvised ones, so the winner can only be better.
//
// Reading is strict, as for the rule overrides (doc 15 §6.3): an unknown key, a wrong type or a value out of range is an
// error naming the file and the entry, never ignored (a typo must not silently drop a hint). Names are checked against the
// board by check_advice_refs; entries that have no effect are warnings.
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "geom/point.hpp"
#include "model/board.hpp"
#include "model/rules.hpp"
#include "route/router.hpp"

namespace tmk::advice {

// Upper bounds on what one file may ask for (an advisor is not trusted to be sensible, doc 17 §6).
inline constexpr std::size_t kMaxFirstNets = 64;
inline constexpr std::size_t kMaxPriority = 512;
inline constexpr std::size_t kMaxRegions = 24;
inline constexpr std::size_t kMaxPolygon = 64;
inline constexpr std::size_t kMaxRegionNets = 64;
inline constexpr std::size_t kMaxNote = 2000;
inline constexpr int kMaxVariants = 8;
inline constexpr int kDefaultVariants = 4;

struct Region {
  std::string name;                         // "" = numbered
  std::vector<std::string> layers;          // copper layer names; empty = every copper layer
  std::vector<geom::Point> polygon;         // nm, the board file's axes
  bool tracks = true, vias = true;
  std::vector<std::string> reserve_for;     // only these nets may enter
  std::vector<std::string> avoid_nets;      // these nets may not enter
  std::string where;                        // "regions[2]" in messages
};

struct Advice {
  std::string source;                       // file name, for messages
  int version = 1;
  std::string note;
  int variants = kDefaultVariants;
  std::vector<std::string> first_nets;
  std::vector<std::pair<std::string, std::string>> priority;  // "REF.NUM" pairs
  std::vector<std::string> no_tracks;       // copper layer names
  std::vector<std::pair<std::string, double>> layer_cost;
  std::vector<Region> regions;
  bool empty() const { return first_nets.empty() && priority.empty() && no_tracks.empty() && layer_cost.empty() && regions.empty(); }
};

// Parses and validates the text of an advice file. Throws std::runtime_error naming `source` and the entry ("regions[1].rect").
Advice parse_advice(std::string_view json_text, const std::string& source = "advice");
// Reads a file (size-limited); throws on a missing file or a syntax or content error.
Advice load_advice_file(const std::string& path);
// Checks every name against the board: nets, pads (REF.NUM, on one net), copper layers. Throws std::runtime_error listing
// the unknown ones with a "did you mean" where one is close. Warnings (entries without effect) go to `warnings`.
void check_advice_refs(const Advice& a, const model::Board& b, std::vector<std::string>* warnings = nullptr);

// What the router is given for an advice file, and what each hint became (for the log, the summary and the viewer).
struct Applied {
  route::RouterOptions::AdviceSet set;
  nlohmann::json report;                    // {"source", "note", "hints": [{"kind", "text", "applied"}], "variants"}
  std::vector<std::string> warnings;
};
// check_advice_refs, then the RouterOptions::AdviceSet: layer masks and costs, and for each region a rule area plus
// disallow rules (doc 05 §34) that name its nets. `a` is not changed.
Applied apply_advice(const Advice& a, const model::Board& b);

// The advice as JSON, with the board's real names: what is saved beside the output so that `--advice FILE` repeats a run.
nlohmann::json advice_to_json(const Advice& a);

// The hint types an advisor may use, as JSON (goes into the stuck report so an advisor adapts to the engine it talks to).
nlohmann::json advice_schema();

// Aliases for net names and part references (a redacted stuck report): the advice an advisor writes uses the aliases, and
// unalias puts the board's names back before the references are checked. Unknown aliases stay as they are (and fail the check).
struct AliasMap {
  std::map<std::string, std::string> net;   // alias -> net name
  std::map<std::string, std::string> part;  // alias -> reference
  bool empty() const { return net.empty() && part.empty(); }
};
void unalias(Advice& a, const AliasMap& m);

}  // namespace tmk::advice

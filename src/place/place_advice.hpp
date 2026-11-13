// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Placement advice (design doc 17 §9 stage 4, issue #26): proposed placements and moves for `tracemaker-place`, written by a person, a
// script or a language model. The placer never talks to a model; the file is one more input, read strictly.
//
//   {
//     "version": 1,
//     "note": "free text, logged and never acted on",
//     "seeds": [{"name": "U3 to the west",                  // a whole proposal: these parts moved together
//                "moves": [{"ref": "U3", "at": [42.5, 18.0], "rotate": 90}]}],
//     "eco": [{"ref": "C12", "at": [40.0, 21.5]}]           // single-part moves, tried first among the ECO moves
//   }
//
// "at" is the footprint origin in millimetres on the board file's axes; "rotate" is the footprint's absolute orientation in degrees
// as KiCad shows it (default: unchanged) and must be the part's own orientation plus a multiple of 90 (the placer's rotation states).
// A seed becomes a candidate of the routability loop and an "eco" move a candidate of the ECO step: each is routed with the same
// budget as the placer's own candidates and kept only when it leaves fewer connections unrouted (CLAUDE.md rule 4). A proposal that
// adds an overlap or puts a part outside the board is skipped with a note; one that names a locked or otherwise fixed part, an
// unknown reference, an unknown key or an orientation the part cannot take is refused with an error naming the entry (rule 6: locked
// parts never move; a typo must not silently drop a proposal).
#include <string>
#include <string_view>
#include <vector>

#include "place/legality.hpp"
#include "place/problem.hpp"

namespace tmk::place {

inline constexpr std::size_t kMaxAdviceSeeds = 8;
inline constexpr std::size_t kMaxSeedMoves = 64;
inline constexpr std::size_t kMaxEcoMoves = 32;

// One proposed part position, resolved against the problem.
struct AdvisedMove {
  int part = -1;
  Point pos;              // footprint origin (nm)
  std::uint8_t rot = 0;   // rotation state (angle0 + 90° * rot)
  std::string what;       // "U3 to (42.50, 18.00) at 90°" for the log
};

struct AdvisedSeed {
  std::string name;
  std::vector<AdvisedMove> moves;
};

struct PlaceAdvice {
  std::string source;
  std::string note;
  std::vector<AdvisedSeed> seeds;
  std::vector<AdvisedMove> eco;
  bool empty() const { return seeds.empty() && eco.empty(); }
};

// Parses the text of a placement advice file and resolves its references against `p`. Throws std::runtime_error naming `source`
// and the entry ("seeds[1].moves[0].ref").
PlaceAdvice parse_place_advice(std::string_view json_text, const Problem& p, const std::string& source = "placement advice");
// Reads and parses a file (as above).
PlaceAdvice read_place_advice(const std::string& path, const Problem& p);

// `base` with the seed's moves applied.
Placement apply_seed(const Placement& base, const AdvisedSeed& s);

}  // namespace tmk::place

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// How deep a rip-up chain each open connection needs (doc 20 §2.9, `tracemaker chains`): a measurement, not a router.
//
// The designer-oracle test (bench/records/oracle-2026-11) showed that on many boards every open connection can be drawn once the
// other routes are arranged differently. This measures how much has to be rearranged, one open connection at a time, with the rest of
// the routed board fixed:
//   depth 0  the connection routes on the board as it is (left by the order or the budget);
//   joint    it and its blocking nets, lifted near its path, all route when the router is given them together (tried first);
//   depth 1  it routes once the blocking nets are lifted near its path, and every lifted net routes again around it;
//   depth d  some lifted net needs its own blockers lifted, and so on (an ejection chain; Shin and Sangiovanni-Vincentelli's "weak
//            modification", IEEE TCAD 6(6), 1987, is the ancestor).
// The blockers are the nets the router's last rejected route would have ripped (RouteResult::Unrouted::blockers), or failing that the
// nets whose copper the connection's route on the bare board touches; the set is then shrunk greedily (a net is put back if the
// connection still routes), so it is minimal, not minimum. A connection the chain has put in is held: deeper levels route around it
// (else two nets swap places for ever). There is no backtracking: a chain that fails here might close in another
// order. Every routing is the product router's, so a chain that closes is a legal board with one more connection routed.
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace tmk::app {

struct ChainsOptions {
  std::string rules_from;           // read the rules beside this board instead
  long baseline_work = 10'000'000;  // work of the run on the board as it is
  long work = 2'000'000;            // work of each run on a changed copy
  int max_depth = 4;                // chains deeper than this are given up
  int max_nets = 32;                // and chains that lift more nets than this in all
  int max_shrink = 40;              // blocker sets larger than this are not shrunk (one run per member)
  double rip_mm = 1.0;              // a blocker's copper is lifted within this distance of the new route (0 = the whole net)
  int threads = 8;                  // open connections measured at once (the result does not depend on it)
  std::uint64_t seed = 1;
  bool fill_planes = true;          // as `causes`
  bool soft_zones = false;
  // Not only measured: the chains that close are put on the board, one open connection after the other (each on the board the ones
  // before left), and the board is written here (doc 20 §2.16, D200). It is written only when fewer connections are open than on the
  // input and every net that was lifted is joined as it was; copper left with no pad on it is taken off (D198).
  std::string apply_out;
  // Applied mode only: how many passes over the still-open connections. A chain that fails on the board as it was may close on
  // the board the chains before it left (and one that closed may have been blocking a later one), so a second sweep is the
  // bounded form of backtracking over chain order. 1 = the D200 behaviour.
  int apply_sweeps = 1;
};

// Measures the chain of every open connection of `board` and returns the report (also printed as a table when `print`).
nlohmann::json measure_chains(const std::string& board, const ChainsOptions& opt, bool print);

}  // namespace tmk::app

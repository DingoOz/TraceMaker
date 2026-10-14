// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Critical nets, from what the board's parts are (issue #24, docs/05-routing.md §44, D117).
//
// JLCPCB's first rule of routing is to route the critical signals first. Which signals are critical is not in the netlist, but the component rules
// (doc 15) know the roles the nets play: the data lines of a USB port, the pairs of an Ethernet PHY, a crystal's two pins, the clock of a memory
// interface. This turns the roles into tiers: 1 clocks, crystals and RF lines (the signals that radiate and that nothing may disturb), 2 high-speed
// differential links, 3 memory and other fast parallel buses. A tier is the order of routing; the user's own list (`--critical-nets`) is tier 1.
#include <string>
#include <utility>
#include <vector>

#include "crules/catalogue.hpp"
#include "crules/detect.hpp"
#include "model/board.hpp"

namespace tmk::crules {

struct CriticalNet {
  model::NetId net = 0;
  int tier = 0;       // 1 = most critical
  std::string why;    // "USB 2.0 data (usb2 dp)", "differential pair"
};

// The nets playing a critical role in a detected category instance, and `diff_pairs` (named pairs, tier 2) that no role claimed. A net found
// twice keeps the lower tier. Deterministic: tier, then net id.
std::vector<CriticalNet> critical_nets(const model::Board& b, const Catalogue& cat, const Detection& det,
                                       const std::vector<std::pair<model::NetId, model::NetId>>& diff_pairs);

// The table itself, for the docs and the tests: (category id, role, tier).
struct CriticalRole {
  const char* category;
  const char* role;
  int tier;
};
const std::vector<CriticalRole>& critical_roles();

}  // namespace tmk::crules

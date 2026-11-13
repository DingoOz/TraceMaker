// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Helpers shared by the ECO step, the routability loop (routable.cpp) and placement closure (closure.cpp).
#include <functional>
#include <string>
#include <vector>

#include "place/routable.hpp"

namespace tmk::place::detail {

inline constexpr Coord kLattice = 10'000;  // ECO targets are snapped to 10 µm
Coord snap(Coord v);
Point snap(Point q);
// Centre of a part's body box in state r, relative to its origin.
Point centre(const Part& pt, int r);
std::string fmt_mm(Coord v);
// Problem copy with the failed nets' weights boosted (ranking and re-placement only; reports use the original).
Problem boosted(const Problem& p, const std::vector<std::string>& nets, int boost);
// Movable parts at the ends of failed connections or with bodies near their boxes (sorted, unique).
std::vector<int> parts_near(const Problem& p, const Placement& pl, const std::vector<RouteEval::Failure>& failed, Coord corridor);
// Distance from a point to a part's body box (0 inside).
Coord box_dist(const Problem& p, int part, const Placement& pl, Point q);
void say(const std::function<void(const std::string&)>& log, const std::string& s);
// "N unrouted (routed/connections)".
std::string describe(const RouteEval& e);

}  // namespace tmk::place::detail

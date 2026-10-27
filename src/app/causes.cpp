// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/causes.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "route/escape.hpp"
#include "route/obstacles.hpp"
#include "route/router.hpp"

namespace tmk::app {
using nlohmann::json;

namespace {

struct Box {
  Coord x0, y0, x1, y1;
};

// Does the segment a-b, thickened by `half`, touch the box? (The box is grown by `half` and the segment clipped against it, Liang-Barsky.)
bool segment_touches(geom::Point a, geom::Point b, Coord half, const Box& r) {
  const double x0 = static_cast<double>(r.x0 - half), y0 = static_cast<double>(r.y0 - half);
  const double x1 = static_cast<double>(r.x1 + half), y1 = static_cast<double>(r.y1 + half);
  const double dx = static_cast<double>(b.x - a.x), dy = static_cast<double>(b.y - a.y);
  double t0 = 0, t1 = 1;
  const double p[4] = {-dx, dx, -dy, dy};
  const double q[4] = {static_cast<double>(a.x) - x0, x1 - static_cast<double>(a.x), static_cast<double>(a.y) - y0, y1 - static_cast<double>(a.y)};
  for (int i = 0; i < 4; ++i) {
    if (p[i] == 0) {
      if (q[i] < 0) return false;
      continue;
    }
    const double t = q[i] / p[i];
    if (p[i] < 0) t0 = std::max(t0, t);
    else t1 = std::min(t1, t);
    if (t0 > t1) return false;
  }
  return true;
}

bool touches_any(geom::Point a, geom::Point b, Coord half, const std::vector<Box>& boxes) {
  for (const auto& r : boxes)
    if (segment_touches(a, b, half, r)) return true;
  return false;
}

// A copy of the board (copper locked) keeping the copper for which keep(net, near) holds; `net`'s own copper always stays.
model::Board copy_with(const model::Board& b, model::NetId net, const std::function<bool(model::NetId, bool)>& keep, const std::vector<Box>& near) {
  model::Board c = b;
  auto kept = [&](model::NetId n, bool is_near) { return n == net || keep(n, is_near); };
  std::erase_if(c.tracks, [&](const model::Track& t) { return !kept(t.net, touches_any(t.a, t.b, t.width / 2, near)); });
  std::erase_if(c.arcs, [&](const model::ArcTrack& t) {
    return !kept(t.net, touches_any(t.a, t.mid, t.width / 2, near) || touches_any(t.mid, t.b, t.width / 2, near));
  });
  std::erase_if(c.vias, [&](const model::Via& v) { return !kept(v.net, touches_any(v.pos, v.pos, v.size / 2, near)); });
  return c;
}

std::string pad_name(const model::Board& b, int pad) {
  if (pad < 0) return "zone";
  const auto& p = b.pads[static_cast<std::size_t>(pad)];
  return b.footprints[static_cast<std::size_t>(p.footprint)].reference + "." + p.number;
}

// Open connections of one net on a routed copy, as end pads (a connection still open touches one of them).
std::set<int> open_pads(const route::RouteResult& r) {
  std::set<int> s;
  for (const auto& u : r.unrouted) {
    if (u.pad_a >= 0) s.insert(u.pad_a);
    if (u.pad_b >= 0) s.insert(u.pad_b);
  }
  return s;
}

}  // namespace

json classify_open_connections(const std::string& path, const CausesOptions& opt, bool print) {
  const auto lb = io::read_board_file(path);
  const auto rules = io::read_design_rules(opt.rules_from.empty() ? path : opt.rules_from);
  model::Board board = lb.board;
  for (auto& t : board.tracks) t.locked = true;  // the copper is the run's result: nothing in it moves (rule 6)
  for (auto& t : board.arcs) t.locked = true;
  for (auto& v : board.vias) v.locked = true;

  route::RouterOptions base;
  base.seed = opt.seed;
  base.gpu_device = -1;
  base.time_limit_s = 3600;  // a safety net: the budgets are the stop (rule 5)
  auto run = [&](const model::Board& b, long work, const std::string& only) {
    route::RouterOptions o = base;
    o.work_budget = work;
    if (!only.empty())
      for (const auto& n : b.nets)
        if (n.id > 0 && !n.name.empty() && n.name != only) o.skip_nets.push_back(n.name);
    return route::Router(b, rules, o).run();
  };

  // The board as it is: what the router still routes here was left by the budget or the order, not by the board.
  const auto first = run(board, opt.baseline_work, "");
  json out;
  out["board"] = path;
  out["copper_layers"] = board.copper_count();
  out["baseline"] = {{"connections", first.connections}, {"routed", first.routed}, {"open", first.unrouted.size()}, {"work", opt.baseline_work}};
  if (print)
    std::printf("%s: %d copper layers; on the board as it is the router routes %d more and leaves %zu open (%ld work)\n", path.c_str(), board.copper_count(),
                first.routed, first.unrouted.size(), opt.baseline_work);
  if (opt.baseline_only) return out;

  // Pins that cannot leave their package on the bare board, and the dense packages (route/escape.hpp).
  model::Board bare = board;
  bare.tracks.clear();
  bare.arcs.clear();
  bare.vias.clear();
  route::Obstacles obs(bare, rules);
  const auto parts = route::analyse_escapes(bare, rules, obs);
  std::map<int, const route::PartEscape*> dense;  // footprint -> its analysis
  std::map<int, std::string> dead;                // pad -> why it cannot escape
  for (const auto& pe : parts) {
    dense[pe.footprint] = &pe;
    for (const auto& d : pe.dead) dead[d.pad] = d.reason;
  }

  // The open connections, by net (in the order of the first run's list, which is deterministic).
  std::vector<std::string> nets;
  std::map<std::string, std::vector<std::size_t>> of_net;
  for (std::size_t i = 0; i < first.unrouted.size(); ++i) {
    const auto& n = first.unrouted[i].net;
    if (!of_net.count(n)) nets.push_back(n);
    of_net[n].push_back(i);
  }
  std::vector<std::string> cause(first.unrouted.size());
  std::vector<std::string> tested(first.unrouted.size());  // the copies tried, in order
  const Coord reach = static_cast<Coord>(std::llround(opt.radius_mm * 1e6));
  auto box_of = [&](int pad) {
    const auto& p = board.pads[static_cast<std::size_t>(pad)];
    const Coord h = std::max(p.size_x, p.size_y) / 2 + reach;
    return Box{p.pos.x - h, p.pos.y - h, p.pos.x + h, p.pos.y + h};
  };

  auto classify_net = [&](std::size_t k) {
    const auto& name = nets[k];
    const auto net = board.net_by_name(name);
    std::vector<std::size_t> todo;
    std::vector<Box> near;
    for (const auto i : of_net[name]) {
      const auto& u = first.unrouted[i];
      const auto da = dead.find(u.pad_a), db = u.pad_b >= 0 ? dead.find(u.pad_b) : dead.end();
      if (da != dead.end() || db != dead.end()) {
        cause[i] = "pin_cannot_escape";
        continue;
      }
      todo.push_back(i);
      if (u.pad_a >= 0) near.push_back(box_of(u.pad_a));
      if (u.pad_b >= 0) near.push_back(box_of(u.pad_b));
    }
    // The nets with a pad on a part at an end: their copper near the ends is the parts' own fan-out; any other net's copper there passes through.
    std::set<model::NetId> own;
    for (const auto i : todo)
      for (const int pad : {first.unrouted[i].pad_a, first.unrouted[i].pad_b}) {
        if (pad < 0) continue;
        for (const int q : board.footprints[static_cast<std::size_t>(board.pads[static_cast<std::size_t>(pad)].footprint)].pads)
          own.insert(board.pads[static_cast<std::size_t>(q)].net);
      }
    // Copies, smallest change first; a connection takes the cause of the first copy on which it routes.
    using Keep = std::function<bool(model::NetId, bool)>;
    const std::pair<Keep, const char*> steps[] = {
        {[&](model::NetId n, bool is_near) { return !is_near || own.count(n) > 0; }, "through_traffic"},
        {[](model::NetId, bool is_near) { return !is_near; }, "package_fanout"},
        {[](model::NetId, bool is_near) { return is_near; }, "channel"},
        {[](model::NetId, bool) { return false; }, "near_and_far"}};
    for (const auto& [keep, label] : steps) {
      if (todo.empty()) break;
      const auto r = run(copy_with(board, net, keep, near), opt.work, name);
      const auto still = open_pads(r);
      std::vector<std::size_t> left;
      for (const auto i : todo) {
        const auto& u = first.unrouted[i];
        tested[i] += tested[i].empty() ? label : std::string(",") + label;
        if (still.count(u.pad_a) || (u.pad_b >= 0 && still.count(u.pad_b))) left.push_back(i);
        else cause[i] = label;
      }
      todo = std::move(left);
    }
    for (const auto i : todo) cause[i] = "no_path_bare";
  };
  std::atomic<std::size_t> next{0};
  std::vector<std::thread> pool;
  for (int t = 0; t < std::max(1, opt.threads); ++t)
    pool.emplace_back([&] {
      for (std::size_t k; (k = next++) < nets.size();) classify_net(k);
    });
  for (auto& t : pool) t.join();

  // The report.
  std::map<std::string, int> by_cause, by_cause_dense;
  std::map<std::string, std::map<std::string, int>> by_part;  // dense package -> cause -> open ends
  json conns = json::array();
  for (std::size_t i = 0; i < first.unrouted.size(); ++i) {
    const auto& u = first.unrouted[i];
    json ends = json::array();
    bool at_dense = false;
    for (const int pad : {u.pad_a, u.pad_b}) {
      if (pad < 0) continue;
      const auto& p = board.pads[static_cast<std::size_t>(pad)];
      const auto& f = board.footprints[static_cast<std::size_t>(p.footprint)];
      json e{{"pad", pad_name(board, pad)}, {"part_pads", f.pads.size()}};
      if (const auto it = dense.find(p.footprint); it != dense.end()) {
        at_dense = true;
        e["dense"] = true;
        e["pitch_mm"] = nm_to_mm(it->second->pitch);
        ++by_part[f.reference][cause[i]];
      }
      if (const auto it = dead.find(pad); it != dead.end()) e["cannot_escape"] = it->second;
      ends.push_back(std::move(e));
    }
    ++by_cause[cause[i]];
    if (at_dense) ++by_cause_dense[cause[i]];
    json c{{"net", u.net}, {"a", u.a}, {"b", u.b}, {"reason", u.reason}, {"cause", cause[i]}, {"at_dense_package", at_dense}, {"ends", ends}};
    if (!tested[i].empty()) c["copies"] = tested[i];
    if (!u.why.empty()) c["why"] = u.why;
    conns.push_back(std::move(c));
  }
  out["radius_mm"] = opt.radius_mm;
  out["work"] = opt.work;
  out["dense_packages"] = parts.size();
  out["by_cause"] = by_cause;
  out["by_cause_at_dense_packages"] = by_cause_dense;
  out["by_dense_package"] = by_part;
  out["connections"] = conns;
  if (print) {
    std::printf("  %-18s %6s %6s\n", "cause", "open", "dense");
    for (const auto& [c, n] : by_cause) std::printf("  %-18s %6d %6d\n", c.c_str(), n, by_cause_dense.count(c) ? by_cause_dense.at(c) : 0);
  }
  return out;
}

}  // namespace tmk::app

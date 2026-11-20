// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/chains.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include "drc/refill.hpp"
#include "io/kicad/board_editor.hpp"
#include "io/kicad/board_reader.hpp"
#include "io/kicad/project_reader.hpp"
#include "passes/repair.hpp"
#include "route/router.hpp"

namespace tmk::app {
using nlohmann::json;

namespace {

// A piece of copper as a thick segment on a span of copper layers (a via is a point from its top to its bottom layer).
struct Piece {
  geom::Point a, b;
  Coord half = 0;
  int top = 0, bottom = 0;
  model::NetId net = 0;
};

double seg_point(double px, double py, double ax, double ay, double bx, double by) {
  const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
  double t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
  t = std::clamp(t, 0.0, 1.0);
  return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

bool seg_cross(const geom::Point& p, const geom::Point& q, const geom::Point& r, const geom::Point& s) {
  auto o = [](const geom::Point& a, const geom::Point& b, const geom::Point& c) {
    const double v = static_cast<double>(b.x - a.x) * static_cast<double>(c.y - a.y) - static_cast<double>(b.y - a.y) * static_cast<double>(c.x - a.x);
    return v > 0 ? 1 : v < 0 ? -1 : 0;
  };
  return o(p, q, r) * o(p, q, s) < 0 && o(r, s, p) * o(r, s, q) < 0;
}

// Centre-line distance of two segments.
double seg_seg(const Piece& x, const Piece& y) {
  if (seg_cross(x.a, x.b, y.a, y.b)) return 0;
  const auto d = [](const geom::Point& p, const Piece& s) {
    return seg_point(static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(s.a.x), static_cast<double>(s.a.y), static_cast<double>(s.b.x), static_cast<double>(s.b.y));
  };
  return std::min({d(x.a, y), d(x.b, y), d(y.a, x), d(y.b, x)});
}

// Do the two pieces share a layer and come within `extra` of each other, edge to edge?
bool near(const Piece& x, const Piece& y, Coord extra) {
  if (x.top > y.bottom || y.top > x.bottom) return false;
  return seg_seg(x, y) < static_cast<double>(x.half + y.half + extra);
}

Piece piece(const model::Track& t) { return {t.a, t.b, t.width / 2, t.layer, t.layer, t.net}; }
Piece piece(const model::Via& v) { return {v.pos, v.pos, v.size / 2, v.layer_top, v.layer_bottom, v.net}; }

std::vector<Piece> pieces(const std::vector<model::Track>& ts, const std::vector<model::Via>& vs) {
  std::vector<Piece> out;
  for (const auto& t : ts) out.push_back(piece(t));
  for (const auto& v : vs) out.push_back(piece(v));
  return out;
}

bool near_any(const Piece& p, const std::vector<Piece>& path, Coord extra) {
  for (const auto& q : path)
    if (near(p, q, extra)) return true;
  return false;
}

// Removes the unlocked copper of the nets in `lift` for which `where` holds (arcs by their two chords).
template <class Where>
void lift_copper(model::Board& b, const std::set<model::NetId>& lift, Where where) {
  std::erase_if(b.tracks, [&](const model::Track& t) { return !t.locked && lift.count(t.net) && where(piece(t)); });
  std::erase_if(b.arcs, [&](const model::ArcTrack& t) {
    return !t.locked && lift.count(t.net) && (where(Piece{t.a, t.mid, t.width / 2, t.layer, t.layer, t.net}) || where(Piece{t.mid, t.b, t.width / 2, t.layer, t.layer, t.net}));
  });
  std::erase_if(b.vias, [&](const model::Via& v) { return !v.locked && lift.count(v.net) && where(piece(v)); });
}

struct Conn {
  std::string net, a, b;
  int pad_a = -1, pad_b = -1;
  std::vector<std::string> blockers;
};

Conn conn_of(const route::RouteResult::Unrouted& u) { return {u.net, u.a, u.b, u.pad_a, u.pad_b, u.blockers}; }

bool is_open(const route::RouteResult& r, const Conn& c) {
  for (const auto& u : r.unrouted)
    if (u.net == c.net && (u.pad_a == c.pad_a || u.pad_a == c.pad_b || (c.pad_b >= 0 && (u.pad_b == c.pad_a || u.pad_b == c.pad_b)))) return true;
  return false;
}

}  // namespace

json measure_chains(const std::string& path, const ChainsOptions& opt, bool print) {
  auto lb = io::read_board_file(path);
  const auto rules = io::read_design_rules(opt.rules_from.empty() ? path : opt.rules_from);
  model::Board board = lb.board;

  // Zones without a fill, as `route` and `causes` see them (doc 05 §54).
  bool soft = opt.soft_zones;
  if (opt.fill_planes) {
    std::vector<std::size_t> empty;
    for (std::size_t i = 0; i < board.zones.size(); ++i) {
      const auto& z = board.zones[i];
      if (!z.rule_area && !z.teardrop && z.net > 0 && z.copper != 0 && z.fills.empty()) empty.push_back(i);
    }
    if (!empty.empty()) {
      const auto filled = drc::refill_zones(board, rules);
      for (const std::size_t i : empty) board.zones[i].fills = filled.board.zones[i].fills;
      soft = true;
    }
  }
  Coord clearance = 0;  // the largest class clearance: what "touches" means when the blockers are found from a bare-board route
  for (const auto& c : rules.classes) clearance = std::max(clearance, c.clearance);
  const Coord rip = static_cast<Coord>(std::llround(opt.rip_mm * 1e6));

  route::RouterOptions base;
  base.seed = opt.seed;
  base.soft_zones = soft;
  base.gpu_device = -1;
  base.time_limit_s = 3600;  // a safety net: the budgets are the stop (rule 5)
  // Routes only `only` (every net when empty) on a copy of `b` whose copper is all locked: nothing but the lifted copper ever moves.
  auto run = [&](const model::Board& b, long work, const std::set<std::string>& only) {
    model::Board c = b;
    for (auto& t : c.tracks) t.locked = true;
    for (auto& t : c.arcs) t.locked = true;
    for (auto& v : c.vias) v.locked = true;
    route::RouterOptions o = base;
    o.work_budget = work;
    if (!only.empty())
      for (const auto& n : c.nets)
        if (n.id > 0 && !n.name.empty() && !only.count(n.name)) o.skip_nets.push_back(n.name);
    return route::Router(c, rules, o).run();
  };
  // `hold`: the copper is locked on the board, so no deeper level of the chain lifts it again. Without it two nets that want the same
  // channel swap places level after level, and every such chain ends "too deep".
  auto add_copper = [](model::Board& b, const route::RouteResult& r, bool hold = false) {
    for (auto t : r.tracks) t.locked = hold || t.locked, b.tracks.push_back(t);
    for (auto v : r.vias) v.locked = hold || v.locked, b.vias.push_back(v);
  };

  const auto first = run(board, opt.baseline_work, {});
  json out;
  out["board"] = path;
  out["copper_layers"] = board.copper_count();
  out["baseline"] = {{"connections", first.connections}, {"routed", first.routed}, {"open", first.unrouted.size()}, {"work", opt.baseline_work}};
  // What the baseline run routed stays: the chains are measured on the board with it.
  add_copper(board, first);

  // Connections open before any chain: a lifted net is back when it has no more open than it had (the target, one fewer).
  std::map<std::string, int> base_open;
  std::set<std::pair<int, int>> base_pairs;
  for (const auto& u : first.unrouted) ++base_open[u.net], base_pairs.insert(std::minmax(u.pad_a, u.pad_b));
  auto open_count = [](const route::RouteResult& r, const std::string& net) {
    int n = 0;
    for (const auto& u : r.unrouted) n += u.net == net ? 1 : 0;
    return n;
  };

  struct Row {
    bool joint = false;  // closed by routing the connection and its lifted blockers together
    bool closed = false;
    int depth = 0, lifted = 0, first_blockers = -1, runs = 0;
    std::string fail, blockers_from;
    std::vector<std::string> first_nets;
  };
  std::vector<Row> rows(first.unrouted.size());

  // Closes `c` on `cur` (changed only on success) at chain depth `depth` (1 = the open connection itself).
  std::function<bool(model::Board&, const Conn&, int, Row&)> close = [&](model::Board& cur, const Conn& c, int depth, Row& row) -> bool {
    const model::NetId self = cur.net_by_name(c.net);
    std::set<model::NetId> lift;
    route::RouteResult kept;
    bool have = false;
    auto without = [&](const std::set<model::NetId>& nets) {
      model::Board t = cur;
      lift_copper(t, nets, [](const Piece&) { return true; });
      return t;
    };
    auto try_route = [&](const std::set<model::NetId>& nets) {
      ++row.runs;
      auto r = run(without(nets), opt.work, {c.net});
      const bool ok = !is_open(r, c);
      if (ok) kept = std::move(r), have = true;
      return ok;
    };
    // The nets the router's rejected route would have ripped.
    for (const auto& n : c.blockers)
      if (const auto id = cur.net_by_name(n); id > 0 && id != self) lift.insert(id);
    std::string from = "router";
    if (lift.empty() || !try_route(lift)) {
      // The nets its route on the board without other routed copper touches.
      std::set<model::NetId> all;
      for (const auto& t : cur.tracks)
        if (!t.locked && t.net != self) all.insert(t.net);
      for (const auto& v : cur.vias)
        if (!v.locked && v.net != self) all.insert(v.net);
      have = false;
      if (!try_route(all)) {
        row.fail = depth == 1 ? "no_path_bare" : "lifted_net_no_path_bare";
        return false;
      }
      const auto mine = pieces(kept.tracks, kept.vias);
      lift.clear();
      for (const auto& p : pieces(cur.tracks, cur.vias))
        if (p.net != self && all.count(p.net) && near_any(p, mine, clearance)) lift.insert(p.net);
      from = "bare";
      have = false;
      if (!lift.empty() && !try_route(lift)) {
        row.fail = "blockers_not_found";  // its bare route's neighbours lifted, it still does not route (the route moved)
        return false;
      }
      if (lift.empty()) {  // nothing in the way of the bare route, yet it did not route with everything there
        row.fail = "blockers_not_found";
        return false;
      }
    }
    // Shrink: put each net back if the connection still routes without the others.
    if (static_cast<int>(lift.size()) <= opt.max_shrink) {
      const std::vector<model::NetId> order(lift.begin(), lift.end());
      for (const auto n : order) {
        if (lift.size() == 1) break;
        auto fewer = lift;
        fewer.erase(n);
        if (try_route(fewer)) lift = std::move(fewer);
      }
    }
    if (depth == 1) {
      row.first_blockers = static_cast<int>(lift.size());
      row.blockers_from = from;
      for (const auto n : lift) row.first_nets.push_back(cur.nets[static_cast<std::size_t>(n)].name);
    }
    row.lifted += static_cast<int>(lift.size());
    if (row.lifted > opt.max_nets) {
      row.fail = "too_many_nets";
      return false;
    }
    const auto mine = pieces(kept.tracks, kept.vias);
    // First the connection and its blockers together, the blockers lifted near its route and the rest of the board fixed: the router's
    // own negotiation settles who takes which way. Putting the connection in first and holding it (below) fixes its route before the
    // blockers have had a say, and a blocker left with no way at all is the commonest failure of that order.
    if (depth == 1) {
      model::Board joint = cur;
      if (rip > 0) lift_copper(joint, lift, [&](const Piece& p) { return near_any(p, mine, rip); });
      else lift_copper(joint, lift, [](const Piece&) { return true; });
      std::set<std::string> together{c.net};
      for (const auto n : lift) together.insert(joint.nets[static_cast<std::size_t>(n)].name);
      ++row.runs;
      const auto r = run(joint, 2 * opt.work, together);
      bool all = !is_open(r, c);
      for (const auto& n : together) all = all && open_count(r, n) <= base_open[n] - (n == c.net ? 1 : 0);
      if (all) {
        add_copper(joint, r);
        cur = std::move(joint);
        row.depth = 1;
        row.joint = true;
        return true;
      }
    }
    // The board with the connection in and the blockers lifted near it.
    model::Board next = cur;
    if (rip > 0) lift_copper(next, lift, [&](const Piece& p) { return near_any(p, mine, rip); });
    else lift_copper(next, lift, [](const Piece&) { return true; });
    add_copper(next, kept, true);
    // The lifted nets route again, together; the ones that do not are closed one by one, a level down.
    std::set<std::string> names;
    for (const auto n : lift) names.insert(next.nets[static_cast<std::size_t>(n)].name);
    ++row.runs;
    const auto again = run(next, opt.work, names);
    add_copper(next, again);
    row.depth = std::max(row.depth, depth);
    for (const auto& u : again.unrouted) {
      if (!names.count(u.net) || base_pairs.count(std::minmax(u.pad_a, u.pad_b))) continue;  // not lifted, or open before the chain
      if (depth + 1 > opt.max_depth) {
        row.fail = "too_deep";
        return false;
      }
      // An earlier chain of this level may already have closed it.
      ++row.runs;
      const auto check = run(next, opt.work, {u.net});
      const Conn cu = conn_of(u);
      if (!is_open(check, cu)) {
        add_copper(next, check);
        continue;
      }
      Conn cc = cu;
      for (const auto& v : check.unrouted)
        if (is_open(check, cu) && v.net == cu.net && v.pad_a == cu.pad_a && v.pad_b == cu.pad_b) cc.blockers = v.blockers;
      if (!close(next, cc, depth + 1, row)) return false;
    }
    cur = std::move(next);
    return true;
  };

  const bool apply = !opt.apply_out.empty();
  json applied;
  if (apply) {
    // One after the other on one board: a chain that closes changes the board the next one is closed on. What a chain held (so its
    // own deeper levels route round it) is free again for the next chain; only the file's locked copper stays locked.
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto& u = first.unrouted[i];
      model::Board cur = board;
      rows[i].closed = close(cur, conn_of(u), 1, rows[i]);
      if (!rows[i].closed) continue;
      for (auto& t : cur.tracks)
        if (t.node == sexpr::kNoNode) t.locked = false;
      for (auto& v : cur.vias)
        if (v.node == sexpr::kNoNode) v.locked = false;
      board = std::move(cur);
      --base_open[u.net];
      base_pairs.erase(std::minmax(u.pad_a, u.pad_b));
    }
    // The board as written: the file's copper that is still there keeps its place in the document, the rest is new.
    std::set<sexpr::NodeId> left;
    std::vector<model::Track> new_tracks;
    std::vector<model::Via> new_vias;
    for (const auto& t : board.tracks) {
      if (t.node != sexpr::kNoNode) left.insert(t.node);
      else new_tracks.push_back(t);
    }
    for (const auto& v : board.vias) {
      if (v.node != sexpr::kNoNode) left.insert(v.node);
      else new_vias.push_back(v);
    }
    model::Board now = lb.board;
    std::erase_if(now.tracks, [&](const model::Track& t) { return !left.count(t.node); });
    std::erase_if(now.vias, [&](const model::Via& v) { return !left.count(v.node); });
    const auto cut = passes::padless_islands(lb.board, now, new_tracks, new_vias);
    std::set<sexpr::NodeId> cut_nodes;
    for (const int k : cut.base_tracks) cut_nodes.insert(now.tracks[static_cast<std::size_t>(k)].node);
    for (const int k : cut.base_vias) cut_nodes.insert(now.vias[static_cast<std::size_t>(k)].node);
    model::Board fin = now;
    std::erase_if(fin.tracks, [&](const model::Track& t) { return cut_nodes.count(t.node) > 0; });
    std::erase_if(fin.vias, [&](const model::Via& v) { return cut_nodes.count(v.node) > 0; });
    std::vector<model::Track> add_t;
    std::vector<model::Via> add_v;
    for (std::size_t k = 0; k < new_tracks.size(); ++k)
      if (!cut.new_track_cut[k]) add_t.push_back(new_tracks[k]);
    for (std::size_t k = 0; k < new_vias.size(); ++k)
      if (!cut.new_via_cut[k]) add_v.push_back(new_vias[k]);
    model::Board check = fin;
    check.tracks.insert(check.tracks.end(), add_t.begin(), add_t.end());
    check.vias.insert(check.vias.end(), add_v.begin(), add_v.end());
    const int open_before = passes::open_connections(lb.board, rules), open_after = passes::open_connections(check, rules);
    int closed = 0;
    for (const auto& r : rows) closed += r.closed ? 1 : 0;
    const bool write = open_after < open_before;
    if (write) {
      io::BoardEditor ed(lb, opt.seed);
      for (std::size_t k = lb.board.tracks.size(); k-- > 0;)
        if (!left.count(lb.board.tracks[k].node) || cut_nodes.count(lb.board.tracks[k].node)) ed.remove_track(k);
      for (std::size_t k = lb.board.vias.size(); k-- > 0;)
        if (!left.count(lb.board.vias[k].node) || cut_nodes.count(lb.board.vias[k].node)) ed.remove_via(k);
      for (const auto& t : add_t) ed.add_track(t);
      for (const auto& v : add_v) ed.add_via(v);
      ed.save(opt.apply_out);
    }
    applied = {{"chains_closed", closed}, {"open_before", open_before}, {"open_after", open_after}, {"written", write},
               {"islands_cut", cut.islands}, {"output", write ? opt.apply_out : std::string()}};
  } else {
    std::atomic<std::size_t> next_i{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < std::max(1, opt.threads); ++t)
      pool.emplace_back([&] {
        for (std::size_t i; (i = next_i++) < rows.size();) {
          model::Board cur = board;
          rows[i].closed = close(cur, conn_of(first.unrouted[i]), 1, rows[i]);
        }
      });
    for (auto& t : pool) t.join();
  }

  std::map<std::string, int> tally;
  json conns = json::array();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto& u = first.unrouted[i];
    const auto& r = rows[i];
    const std::string key = r.closed ? (r.joint ? "closed_joint" : "closed_depth_" + std::to_string(r.depth)) : r.fail;
    ++tally[key];
    json c{{"net", u.net}, {"a", u.a}, {"b", u.b}, {"reason", u.reason}, {"closed", r.closed}, {"depth", r.depth}, {"nets_lifted", r.lifted},
           {"first_blockers", r.first_blockers}, {"joint", r.joint}, {"first_nets", r.first_nets}, {"blockers_from", r.blockers_from}, {"runs", r.runs}};
    if (!r.closed) c["fail"] = r.fail;
    conns.push_back(std::move(c));
  }
  out["options"] = {{"work", opt.work}, {"max_depth", opt.max_depth}, {"max_nets", opt.max_nets}, {"rip_mm", opt.rip_mm}};
  out["tally"] = tally;
  out["connections"] = conns;
  if (apply) out["applied"] = applied;
  if (print) {
    std::printf("%s: the router routes %d more on the board as it is and leaves %zu open (%ld work)\n", path.c_str(), first.routed, first.unrouted.size(), opt.baseline_work);
    for (const auto& [k, n] : tally) std::printf("  %-26s %5d\n", k.c_str(), n);
    if (apply)
      std::printf("applied: %d chains closed, open %d -> %d; %s\n", applied["chains_closed"].get<int>(), applied["open_before"].get<int>(), applied["open_after"].get<int>(),
                  applied["written"].get<bool>() ? ("written to " + opt.apply_out).c_str() : "not written (no fewer open than the input)");
  }
  return out;
}

}  // namespace tmk::app

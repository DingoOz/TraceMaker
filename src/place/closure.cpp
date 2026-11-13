// SPDX-License-Identifier: GPL-3.0-or-later
#include "place/closure.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "place/anneal.hpp"
#include "place/eco_util.hpp"
#include "place/wirelength.hpp"

namespace tmk::place {

using namespace detail;

namespace {

constexpr const char* kNames[kArms] = {"shift", "rotate", "swap", "flip", "room", "far", "window", "replace"};

struct Pose {
  int part;
  Point pos;
  std::uint8_t rot;
};

// One candidate change of the incumbent: a set of part poses (ECO arms), or a whole placement (window, replace).
struct Move {
  std::vector<Pose> poses;
  std::string what;
  std::int64_t score = 0;
  std::size_t order = 0;
  std::vector<std::int64_t> key() const {
    std::vector<std::int64_t> k;
    for (const auto& q : poses) k.insert(k.end(), {q.part, q.pos.x, q.pos.y, q.rot});
    return k;
  }
};

Placement applied(const Placement& pl, const std::vector<Pose>& poses) {
  Placement t = pl;
  for (const auto& q : poses) {
    t.pos[z(q.part)] = q.pos;
    t.rot[z(q.part)] = q.rot;
  }
  return t;
}

// Ranked single- and multi-part moves of one ECO arm around the incumbent's failures (the ranking of doc 04 §4,
// as eco_place: annealing cost with boosted failed nets over a capacity map shrunk at the failures, a displacement
// charge, and credit for room around failed pads). Already-tried moves are left out.
class Ranker {
 public:
  Ranker(const Problem& p, const Placement& pl, const RouteEval& e, const ClosureOptions& o)
      : p_(p), pl_(pl), e_(e), o_(o), L_(p), cm_(make_congestion_map(p)) {
    std::vector<std::string> nets;
    std::vector<Point> fp;
    for (const auto& f : e.failed) {
      nets.push_back(f.net);
      fp.push_back(f.a);
      fp.push_back(f.b);
    }
    std::sort(nets.begin(), nets.end());
    nets.erase(std::unique(nets.begin(), nets.end()), nets.end());
    q_ = boosted(p, nets, o.weight_boost);
    scale_bins(cm_, fp, cm_.cell, 0.5);
    base_ = anneal_cost(q_, pl, o.alpha_cross_mm, o.beta, &cm_, o.via_mm);
    involved_ = parts_near(p, pl, e.failed, o.corridor);
    L_.reset(pl);
  }

  const std::vector<int>& involved() const { return involved_; }

  // A part may take (np, r) only if it has no conflict there at all: a moved part that still overlaps something is
  // a new violation (placer.cpp counts it so, and KiCad reports a changed overlap as a new error), even when the
  // input had an overlap between the same parts.
  bool ok(const Legality& L, int a, Point np, int r) const { return L.legal(a, np, r); }

  std::vector<Move> moves(int arm, const std::set<std::vector<std::int64_t>>& tried) {
    std::vector<Move> out;
    auto add = [&](Move m) {
      if (tried.count(m.key())) return;
      m.score = score(m.poses);
      m.order = out.size();
      out.push_back(std::move(m));
    };
    static constexpr int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    static constexpr const char* dname[4] = {"+x", "-x", "+y", "-y"};
    if (arm == kShift || arm == kFar) {
      static constexpr int near_steps[] = {1, 2}, far_steps[] = {3, 4, 6, 8};
      const auto steps = arm == kShift ? std::vector<int>(std::begin(near_steps), std::end(near_steps))
                                       : std::vector<int>(std::begin(far_steps), std::end(far_steps));
      for (int a : involved_) {
        const Point pos = pl_.pos[z(a)];
        const int r = pl_.rot[z(a)];
        for (int d = 0; d < 4; ++d)
          for (int s : steps) {
            const Point np = snap(pos + Point{dirs[d][0] * s * o_.pitch, dirs[d][1] * s * o_.pitch});
            if (ok(L_, a, np, r)) add(Move{{{a, np, static_cast<std::uint8_t>(r)}}, p_.parts[z(a)].ref + " shift " + dname[d] + " " + fmt_mm(s * o_.pitch) + " mm"});
          }
      }
    } else if (arm == kRotate || arm == kFlip) {
      for (int a : involved_) {
        const Part& pt = p_.parts[z(a)];
        if (arm == kFlip && !pt.may_flip()) continue;
        const int r = pl_.rot[z(a)];
        const Point c = pl_.pos[z(a)] + centre(pt, r);
        for (int dr = arm == kRotate ? 1 : 0; dr <= 3; ++dr) {
          const int r2 = arm == kRotate ? with_turn(r, r + dr) : ((r ^ kFlipBit) & kFlipBit) | ((r + dr) & 3);
          const Point np = snap(c - centre(pt, r2));
          if (!ok(L_, a, np, r2)) continue;
          add(Move{{{a, np, static_cast<std::uint8_t>(r2)}},
                   pt.ref + (arm == kRotate ? " rotate " + std::to_string(90 * dr) : std::string(" flip to the other side") + (dr ? ", turn " + std::to_string(90 * dr) : ""))});
        }
      }
    } else if (arm == kSwap) {
      for (int a : involved_) {
        const Part& pt = p_.parts[z(a)];
        for (std::size_t bi = 0; bi < p_.parts.size(); ++bi) {
          const int b = static_cast<int>(bi);
          const Part& pb = p_.parts[bi];
          if (b == a || !pb.movable || pb.shape_key != pt.shape_key) continue;
          if (b < a && std::binary_search(involved_.begin(), involved_.end(), b)) continue;  // each pair once
          const double dk_f = geom::norm_deg(pb.angle0 - pt.angle0) / 90.0;
          if (std::fabs(dk_f - std::round(dk_f)) > 1e-6) continue;
          const int dk = static_cast<int>(std::lround(dk_f)) & 3;
          const int r = pl_.rot[z(a)];
          const Point na = pl_.pos[bi], nb = pl_.pos[z(a)];
          const auto ra = swap_state(pl_.rot[bi], dk), rb = swap_state(r, -dk);
          if (flipped(ra) != flipped(r) && !(pt.may_flip() && pb.may_flip())) continue;
          if (!L_.inside_ok(a, na, ra) || !L_.inside_ok(b, nb, rb)) continue;
          if (L_.find_conflict(a, na, ra, b) >= 0 || L_.find_conflict(b, nb, rb, a) >= 0 || L_.pair_conflict(a, na, ra, b, nb, rb)) continue;
          add(Move{{{a, na, ra}, {b, nb, rb}}, pt.ref + " swap " + pb.ref});
        }
      }
    } else if (arm == kRoom) {
      room_moves(add);
    }
    std::sort(out.begin(), out.end(), [](const Move& x, const Move& y) { return x.score != y.score ? x.score < y.score : x.order < y.order; });
    return out;
  }

 private:
  // Every movable neighbour of a failed pad (body within o.room, not the pad's own part) pushed straight away from
  // the pad by 1 or 2 pitches, all together; pushed parts that would collide are tried with a smaller push, then
  // left in place. Pads the router reports as boxed in come first.
  template <class Add>
  void room_moves(Add& add) {
    std::vector<std::pair<Point, int>> pads;  // failed pad, its own part
    for (int pass = 0; pass < 2; ++pass)
      for (const auto& f : e_.failed) {
        if ((f.reason == "boxed_in") != (pass == 0)) continue;
        pads.emplace_back(f.a, f.part_a);
        if (f.part_b >= 0 && f.b != f.a) pads.emplace_back(f.b, f.part_b);
      }
    std::set<std::pair<Coord, Coord>> seen;
    for (const auto& [pad, own] : pads) {
      if (!seen.insert({pad.x, pad.y}).second) continue;
      std::vector<int> nb;
      for (std::size_t i = 0; i < p_.parts.size(); ++i)
        if (p_.parts[i].movable && static_cast<int>(i) != own && box_dist(p_, static_cast<int>(i), pl_, pad) < o_.room) nb.push_back(static_cast<int>(i));
      if (nb.size() < 2) continue;  // one neighbour: the shift arm has that move
      for (int s = 1; s <= 2; ++s) {
        Legality L = L_;
        for (int i : nb) L.remove(i);
        Move m;
        std::string who;
        for (int i : nb) {
          const Point c = pl_.pos[z(i)] + centre(p_.parts[z(i)], pl_.rot[z(i)]);
          const Coord dx = c.x - pad.x, dy = c.y - pad.y;
          const Point dir = std::llabs(dx) >= std::llabs(dy) ? Point{dx >= 0 ? 1 : -1, 0} : Point{0, dy >= 0 ? 1 : -1};
          const int r = pl_.rot[z(i)];
          Point at = pl_.pos[z(i)];
          for (int k = s; k >= 1; --k) {
            const Point np = snap(pl_.pos[z(i)] + Point{dir.x * k * o_.pitch, dir.y * k * o_.pitch});
            if (ok(L, i, np, r)) {
              at = np;
              break;
            }
          }
          if (!ok(L, i, at, r)) {  // even its own spot is taken by an earlier push
            m.poses.clear();
            break;
          }
          L.insert(i, at, r);
          if (at != pl_.pos[z(i)]) {
            m.poses.push_back({i, at, static_cast<std::uint8_t>(r)});
            who += (who.empty() ? "" : ",") + p_.parts[z(i)].ref;
          }
        }
        if (m.poses.size() < 2) continue;
        m.what = "room around a failed pad at (" + fmt_mm(pad.x) + ", " + fmt_mm(pad.y) + "): " + who + " pushed " + fmt_mm(s * o_.pitch) + " mm";
        add(std::move(m));
      }
    }
  }

  std::int64_t score(const std::vector<Pose>& poses) const {
    const Placement t = applied(pl_, poses);
    const std::int64_t disp_units = std::llround(o_.disp_cost * kSignalWeight), room_units = std::llround(o_.room_gain * kSignalWeight);
    std::int64_t s = anneal_cost(q_, t, o_.alpha_cross_mm, o_.beta, &cm_, o_.via_mm) - base_;
    for (const auto& q : poses) {
      s += disp_units * (std::llabs(q.pos.x - pl_.pos[z(q.part)].x) + std::llabs(q.pos.y - pl_.pos[z(q.part)].y));
      for (const auto& f : e_.failed)
        for (int e = 0; e < 2; ++e) {
          if ((e == 0 ? f.part_a : f.part_b) == q.part) continue;  // its own pad moves with it
          const Point pt = e == 0 ? f.a : f.b;
          const Coord d0 = std::min(o_.room, box_dist(p_, q.part, pl_, pt)), d1 = std::min(o_.room, box_dist(p_, q.part, t, pt));
          s -= room_units * (d1 - d0);
        }
    }
    return s;
  }

  const Problem& p_;
  const Placement& pl_;
  const RouteEval& e_;
  const ClosureOptions& o_;
  Problem q_;
  Legality L_;
  CongestionMap cm_;
  std::int64_t base_ = 0;
  std::vector<int> involved_;
};

// UCB value in parts per million: prior-smoothed success rate per route of cost, plus the exploration bonus.
std::int64_t ucb_ppm(const ArmStats& s, int prior_ppm, int prior_n, int cost, int total, double explore) {
  const double n = s.pulls + prior_n;
  const double mean = (s.wins * 1e6 + static_cast<double>(prior_ppm) * prior_n) / std::max(1.0, n);
  const double bonus = explore * 1e6 * std::sqrt(std::log(1.0 + total) / std::max(1.0, n));
  return std::llround(mean / cost + bonus);
}

}  // namespace

const char* arm_name(int arm) { return arm >= 0 && arm < kArms ? kNames[arm] : "?"; }
int arm_from_name(const std::string& name) {
  for (int a = 0; a < kArms; ++a)
    if (name == kNames[a]) return a;
  return -1;
}
std::array<int, kArms> default_prior_ppm() { return {350'000, 300'000, 200'000, 200'000, 300'000, 200'000, 300'000, 250'000}; }
std::array<int, kArms> arm_cost() { return {1, 1, 1, 1, 1, 1, 2, 3}; }

ClosureResult placement_closure(const Problem& p, const Placement& start, const RouteEval& start_eval, const ClosureOptions& o,
                                const RouteFn& route) {
  ClosureResult res;
  res.pl = start;
  res.eval = start_eval;
  if (!start_eval.ok || start_eval.unrouted() == 0) {
    res.stop = start_eval.ok ? "complete" : "input not routed";
    return res;
  }
  // Prior: the built-in rates, or a learned table capped at prior_cap pseudo-trials per arm.
  std::array<int, kArms> prior_ppm = default_prior_ppm(), prior_n{};
  prior_n.fill(1);
  if (o.prior)
    for (int a = 0; a < kArms; ++a) {
      const ArmStats& s = (*o.prior)[z(a)];
      if (s.pulls <= 0) continue;
      prior_n[z(a)] = std::min(s.pulls, o.prior_cap);
      prior_ppm[z(a)] = static_cast<int>(std::llround(1e6 * s.wins / s.pulls));
    }
  const auto cost = arm_cost();
  bool any_flip = false;
  for (const auto& pt : p.parts) any_flip |= pt.may_flip();

  std::set<std::vector<std::int64_t>> tried;  // ECO moves already routed (absolute poses: valid across incumbents)
  std::optional<Ranker> ranker;
  std::array<std::optional<std::vector<Move>>, kArms> lists;  // ranked moves for the current incumbent, per arm
  std::array<std::size_t, kArms> next{};
  int window_k = 0, window_here = 0, replace_k = 0, since_win = 0, total = 0;
  bool replace_done = !o.radical;
  auto reset_incumbent = [&] {
    ranker.reset();
    for (auto& l : lists) l.reset();
    next.fill(0);
    window_here = 0;
  };
  // Has the arm a candidate left for this incumbent? (Builds its ranked list on first use.)
  auto available = [&](int a) -> bool {
    if (a == kFlip && !any_flip) return false;
    if (a == kWindow) return window_here < o.window_per_incumbent;
    if (a == kReplace) return !replace_done;
    if (!ranker) ranker.emplace(p, res.pl, res.eval, o);
    if (!lists[z(a)]) lists[z(a)] = ranker->moves(a, tried);
    auto& l = *lists[z(a)];
    while (next[z(a)] < l.size() && tried.count(l[next[z(a)]].key())) ++next[z(a)];
    return next[z(a)] < l.size();
  };

  while (res.eval.unrouted() > 0) {
    if (res.routes >= o.max_routes) {
      res.stop = "route budget";
      break;
    }
    if (o.out_of_time && o.out_of_time()) {
      res.stop = "time limit";
      break;
    }
    // Choose the arm: the stall rule forces a radical one; otherwise the best UCB value.
    const bool force = since_win >= o.stall;
    int arm = -1;
    std::int64_t best = 0;
    for (int a = 0; a < kArms; ++a) {
      if (force && a != kWindow && a != kReplace) continue;
      if (!available(a)) continue;
      const std::int64_t v = ucb_ppm(res.stats[z(a)], prior_ppm[z(a)], prior_n[z(a)], cost[z(a)], total, o.explore);
      if (arm < 0 || v > best) {
        arm = a;
        best = v;
      }
    }
    if (arm < 0 && force) {  // no radical arm left: carry on with the small ones
      since_win = 0;
      continue;
    }
    if (arm < 0) {
      res.stop = "no candidates";
      break;
    }
    // Build the trial placement.
    Placement t;
    std::string what;
    if (arm == kWindow) {
      std::vector<Point> pts;
      std::vector<std::string> nets;
      for (const auto& f : res.eval.failed) {
        pts.push_back(f.a);
        pts.push_back(f.b);
        nets.push_back(f.net);
      }
      std::sort(nets.begin(), nets.end());
      nets.erase(std::unique(nets.begin(), nets.end()), nets.end());
      CongestionMap cm = make_congestion_map(p);
      scale_bins(cm, pts, cm.cell, o.penalty);
      Problem q = boosted(p, nets, o.weight_boost);
      PlaceOptions po = o.place;
      po.mode = "refine";
      po.beta_congestion = o.beta;
      po.congestion = &cm;
      po.focus = parts_near(p, res.pl, res.eval.failed, o.corridor);
      // Only the window moves: every other part is held where it is (refine annealing would otherwise move them all).
      for (std::size_t i = 0; i < q.parts.size(); ++i)
        if (!std::binary_search(po.focus.begin(), po.focus.end(), static_cast<int>(i))) q.parts[i].movable = false;
      po.lns_rate = std::max(po.lns_rate, 0.02);
      po.seed = o.place.seed + static_cast<std::uint64_t>(++window_k) * 1'000'003u;
      po.trace = nullptr;
      t = res.pl;
      ++window_here;
      const PlaceReport pr = place(q, t, po);
      what = "re-place " + std::to_string(po.focus.size()) + " parts around the failures (seed " + std::to_string(window_k) + ")";
      const Metrics m = measure(p, t, &res.pl);
      if (!pr.legal || m.new_overlaps > 0 || m.new_outside > 0) {
        say(o.log, "  window: " + what + ": not legal, skipped");
        ++res.stats[z(arm)].pulls;  // a wasted pull still counts against the arm, without a route
        ++total;
        continue;
      }
    } else if (arm == kReplace) {
      const int k = replace_k++;
      std::optional<Placement> x = o.radical(k);
      if (!x) {
        replace_done = true;
        continue;
      }
      what = "new placement of the whole board (" + std::to_string(k + 1) + ")";
      if (x->pos.empty()) {  // the caller could not make a legal one
        ++res.stats[z(arm)].pulls;
        ++total;
        continue;
      }
      t = std::move(*x);
    } else {
      const Move& m = (*lists[z(arm)])[next[z(arm)]++];
      tried.insert(m.key());
      t = applied(res.pl, m.poses);
      what = m.what;
    }
    const RouteEval e = route(t);
    ++res.routes;
    ++total;
    ClosureTrial tr{arm, what, res.eval.unrouted(), e.unrouted(), false, force};
    ArmStats& st = res.stats[z(arm)];
    ++st.pulls;
    if (e.ok && e.unrouted() < res.eval.unrouted()) {
      ++st.wins;
      tr.accepted = true;
      ++res.accepted;
      say(o.log, std::string("  ") + arm_name(arm) + (force ? " (escalated)" : "") + ": " + what + ": " + describe(e) + " -> accepted");
      res.pl = std::move(t);
      res.eval = e;
      since_win = 0;
      reset_incumbent();
      if (o.on_accept) o.on_accept(std::string(arm_name(arm)) + ": " + what, res.pl);
    } else {
      say(o.log, std::string("  ") + arm_name(arm) + (force ? " (escalated)" : "") + ": " + what + ": " + describe(e));
      ++since_win;
      if (force) since_win = o.stall / 2;  // after a failed radical trial, give the small arms some trials again
    }
    res.trials.push_back(std::move(tr));
  }
  if (res.eval.unrouted() == 0) res.stop = "complete";
  return res;
}

}  // namespace tmk::place

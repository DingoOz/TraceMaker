// Large-neighbourhood search and exact windows for detailed placement (design doc 04 §3 E "LNS", F).
//
// LNS after Shaw (CP 1998): destroy a window of k nearby movable parts and repair it. Small windows are repaired
// exactly over a candidate set by branch and bound (Land & Doig, Econometrica 1960) with the bound
//   Σ_affected w·HPWL(pins of fixed parts and already assigned window parts) − (largest possible fall of the
//   crossing and overflow terms),
// valid because a net's HPWL can only grow when pins are added and the crossing and overflow terms are ≥ 0.
// Larger windows are repaired greedily (largest part first, each to its cheapest legal candidate). Either way
// the window is kept only if the board cost strictly falls, so LNS never makes a placement worse.
#include <algorithm>
#include <climits>

#include "place/anneal_state.hpp"
#include "place/global.hpp"

namespace tmk::place {

namespace detail {

namespace {
constexpr long kInlineLeaves = 4000;              // exact-window leaf cap for windows inside annealing
}  // namespace

std::vector<int> Annealer::window_around(int seed, int k) const {
  const Point c = pl_.pos[z(seed)] + body_centre(p_.parts[z(seed)], pl_.rot[z(seed)]);
  std::vector<std::pair<geom::i128, int>> d;
  d.reserve(movable_.size());
  for (int i : movable_) {
    const Point q = pl_.pos[z(i)] + body_centre(p_.parts[z(i)], pl_.rot[z(i)]);
    const geom::i128 dx = q.x - c.x, dy = q.y - c.y;
    d.emplace_back(dx * dx + dy * dy, i);
  }
  const std::size_t n = std::min(d.size(), static_cast<std::size_t>(std::clamp(k, 1, kMaxMove)));
  std::partial_sort(d.begin(), d.begin() + static_cast<long>(n), d.end());
  std::vector<int> w;
  for (std::size_t i = 0; i < n; ++i) w.push_back(d[i].second);
  return w;
}

bool Annealer::lns_step() {
  if (movable_.empty()) return false;
  const int seed = focus_.empty() ? pick_movable() : focus_[z(static_cast<int>(next_u64() % focus_.size()))];
  const int k = 2 + static_cast<int>(next_u64() % static_cast<std::uint64_t>(std::max(1, std::min(o_.lns_window, kMaxMove) - 1)));
  return repair(window_around(seed, k));
}

void Annealer::candidates(int a, const std::vector<int>& window, const std::vector<Point>& old_pos, const std::vector<std::uint8_t>& old_rot,
                          std::vector<std::pair<Point, std::uint8_t>>& out) const {
  out.clear();
  const Part& pt = p_.parts[z(a)];
  std::size_t self = 0;
  while (window[self] != a) ++self;
  out.emplace_back(old_pos[self], old_rot[self]);
  for (int r = 0; r < 4; ++r) {
    // Every window part's spot (body centres), including its own spot at the other rotations.
    for (std::size_t j = 0; j < window.size(); ++j) {
      const Point c = old_pos[j] + body_centre(p_.parts[z(window[j])], old_rot[j]);
      out.emplace_back(snap(c - body_centre(pt, r)), static_cast<std::uint8_t>(r));
    }
    Point t;
    if (median_target(a, r, t)) out.emplace_back(snap(t), static_cast<std::uint8_t>(r));
  }
  // Drop duplicates, keeping the first occurrence (the own spot stays first).
  std::vector<std::pair<Point, std::uint8_t>> u;
  for (const auto& c : out)
    if (std::find(u.begin(), u.end(), c) == u.end()) u.push_back(c);
  out.swap(u);
}

void Annealer::restore_window(const std::vector<int>& window, const std::vector<Point>& old_pos, const std::vector<std::uint8_t>& old_rot) {
  for (int a : window) L_.remove(a);
  Move m;
  m.n = static_cast<int>(window.size());
  for (std::size_t i = 0; i < window.size(); ++i) {
    m.part[i] = window[i];
    m.old_pos[i] = pl_.pos[z(window[i])];
    m.old_rot[i] = pl_.rot[z(window[i])];
    m.pos[i] = old_pos[i];
    m.rot[i] = old_rot[i];
  }
  evaluate(m);
  commit(m);
}

bool Annealer::repair(std::vector<int> window) {
  if (window.empty() || window.size() > static_cast<std::size_t>(kMaxMove)) return false;
  ++lns_tried_;
  if (static_cast<int>(window.size()) <= o_.exact_window) {
    const WindowStats st = solve_window(window, true, kInlineLeaves);
    if (st.improved) ++lns_improved_;
    return st.improved;
  }
  std::sort(window.begin(), window.end());
  const std::int64_t c0 = cost();
  std::vector<Point> old_pos;
  std::vector<std::uint8_t> old_rot;
  for (int a : window) {
    old_pos.push_back(pl_.pos[z(a)]);
    old_rot.push_back(pl_.rot[z(a)]);
  }
  for (int a : window) L_.remove(a);
  std::vector<int> order = window;
  std::sort(order.begin(), order.end(), [&](int x, int y) {
    const Coord ax = p_.parts[z(x)].area, ay = p_.parts[z(y)].area;
    return ax != ay ? ax > ay : x < y;
  });
  std::vector<std::pair<Point, std::uint8_t>> cand;
  std::vector<std::pair<std::int64_t, std::size_t>> ev;
  for (int a : order) {
    candidates(a, window, old_pos, old_rot, cand);
    // Also a small lattice around the HPWL-optimal spot at the current rotation.
    Point t;
    if (median_target(a, pl_.rot[z(a)], t))
      for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
          if (dx || dy) cand.emplace_back(snap(t + Point{dx * 500'000, dy * 500'000}), pl_.rot[z(a)]);
    ev.clear();
    for (std::size_t i = 0; i < cand.size(); ++i) {
      Move m;
      single(m, a, cand[i].first, cand[i].second);
      ev.emplace_back(evaluate(m), i);
      revert(m);
      ++moves_;
    }
    std::sort(ev.begin(), ev.end());
    bool placed = false;
    for (const auto& [d, i] : ev) {
      if (!L_.legal(a, cand[i].first, cand[i].second)) continue;
      Move m;
      single(m, a, cand[i].first, cand[i].second);
      evaluate(m);
      commit(m);
      placed = true;
      break;
    }
    if (!placed) {
      restore_window(window, old_pos, old_rot);
      return false;
    }
  }
  if (cost() < c0) {
    ++lns_improved_;
    return true;
  }
  restore_window(window, old_pos, old_rot);
  return false;
}

WindowStats Annealer::solve_window(std::vector<int> window, bool prune, long max_leaves) {
  WindowStats st;
  if (window.empty() || window.size() > static_cast<std::size_t>(kMaxMove)) return st;
  std::sort(window.begin(), window.end());
  window.erase(std::unique(window.begin(), window.end()), window.end());
  const std::size_t k = window.size();
  std::vector<Point> old_pos;
  std::vector<std::uint8_t> old_rot;
  for (int a : window) {
    old_pos.push_back(pl_.pos[z(a)]);
    old_rot.push_back(pl_.rot[z(a)]);
  }
  // Identity move: the affected nets and their current crossings.
  Move id;
  id.n = static_cast<int>(k);
  for (std::size_t i = 0; i < k; ++i) {
    id.part[i] = window[i];
    id.pos[i] = id.old_pos[i] = old_pos[i];
    id.rot[i] = id.old_rot[i] = old_rot[i];
  }
  evaluate(id);
  const std::vector<int> affected = nets_;
  const std::int64_t old_c = last_old_c_;
  revert(id);
  std::int64_t hp_old = 0;
  for (int n : affected) hp_old += p_.nets[z(n)].weight * hp_[z(n)];
  // The crossing and overflow terms can fall by at most their current values.
  const std::int64_t slack = alpha_ * old_c + beta_ * ovf_;

  for (int a : window) L_.remove(a);
  std::vector<std::vector<std::pair<Point, std::uint8_t>>> cands(k);
  std::vector<std::pair<Point, std::uint8_t>> tmp;
  for (std::size_t i = 0; i < k; ++i) {
    candidates(window[i], window, old_pos, old_rot, tmp);
    for (std::size_t c = 0; c < tmp.size(); ++c)
      if (c == 0 || L_.legal(window[i], tmp[c].first, tmp[c].second)) cands[i].push_back(tmp[c]);  // own spot: legal by invariant
  }
  std::vector<int> widx(p_.parts.size(), -1);
  for (std::size_t i = 0; i < k; ++i) widx[z(window[i])] = static_cast<int>(i);
  std::vector<Point> cur_pos(k);
  std::vector<std::uint8_t> cur_rot(k);
  std::vector<std::size_t> best_choice(k, 0);
  std::vector<std::size_t> choice(k, 0);
  std::int64_t best_d = 0;
  bool aborted = false;

  auto hp_bound = [&](std::size_t assigned) {
    std::int64_t s = 0;
    for (int n : affected) {
      Coord x0 = LLONG_MAX, x1 = LLONG_MIN, y0 = LLONG_MAX, y1 = LLONG_MIN;
      for (int pi : p_.nets[z(n)].pins) {
        const Pin& q = p_.pins[z(pi)];
        const int w = widx[z(q.part)];
        Point at;
        if (w < 0) at = pl_.pin(p_, pi);
        else if (static_cast<std::size_t>(w) < assigned) at = cur_pos[z(w)] + q.off[cur_rot[z(w)]];
        else continue;
        x0 = std::min(x0, at.x);
        x1 = std::max(x1, at.x);
        y0 = std::min(y0, at.y);
        y1 = std::max(y1, at.y);
      }
      if (x0 <= x1) s += p_.nets[z(n)].weight * ((x1 - x0) + (y1 - y0));
    }
    return s;
  };

  auto dfs = [&](auto&& self, std::size_t i) -> void {
    ++st.nodes;
    if (prune && i > 0 && hp_bound(i) - hp_old - slack >= best_d) return;
    if (i == k) {
      if (++st.leaves > max_leaves) {
        aborted = true;
        return;
      }
      Move m = id;
      for (std::size_t j = 0; j < k; ++j) {
        m.pos[j] = cur_pos[j];
        m.rot[j] = cur_rot[j];
      }
      const std::int64_t d = evaluate(m);
      revert(m);
      if (d < best_d) {
        best_d = d;
        best_choice = choice;
      }
      return;
    }
    for (std::size_t c = 0; c < cands[i].size() && !aborted; ++c) {
      const auto& [q, r] = cands[i][c];
      bool ok = true;
      for (std::size_t j = 0; j < i && ok; ++j) ok = !L_.pair_conflict(window[i], q, r, window[j], cur_pos[j], cur_rot[j]);
      if (!ok) continue;
      cur_pos[i] = q;
      cur_rot[i] = r;
      choice[i] = c;
      self(self, i + 1);
    }
  };
  dfs(dfs, 0);
  moves_ += static_cast<std::uint64_t>(std::max<long>(1, st.leaves));
  st.proven = !aborted;
  st.delta = best_d;
  if (best_d < 0) {
    Move m = id;
    for (std::size_t j = 0; j < k; ++j) {
      m.pos[j] = cands[j][best_choice[j]].first;
      m.rot[j] = cands[j][best_choice[j]].second;
    }
    evaluate(m);
    commit(m);
    st.improved = true;
  } else {
    for (std::size_t i = 0; i < k; ++i) L_.insert(window[i], old_pos[i], old_rot[i]);
  }
  return st;
}

}  // namespace detail

namespace {
constexpr int kLnsStream = 0x4C4E53;  // "LNS": the annealer stream used by lns_improve
}  // namespace

LnsResult lns_improve(const Problem& p, const Placement& start, const AnnealOptions& o, int windows) {
  const auto pn = part_nets(p);
  AnnealOptions oo = o;
  detail::Annealer a(p, pn, start, oo, kLnsStream);
  LnsResult r;
  r.cost_before = a.cost();
  for (int w = 0; w < windows && !a.movable_empty(); ++w) a.lns_step();
  r.pl = a.current();
  r.cost_after = a.cost();
  r.tried = a.lns_tried();
  r.improved = a.lns_improved();
  return r;
}

WindowResult solve_window(const Problem& p, Placement& pl, const std::vector<int>& parts, const AnnealOptions& o, bool prune, long max_leaves) {
  const auto pn = part_nets(p);
  detail::Annealer a(p, pn, pl, o, 0);
  WindowResult r;
  r.cost_before = a.cost();
  const detail::WindowStats st = a.solve_window(parts, prune, max_leaves);
  r.improved = st.improved;
  r.proven = st.proven;
  r.nodes = st.nodes;
  r.leaves = st.leaves;
  r.cost_after = a.cost();
  pl = a.current();
  return r;
}

}  // namespace tmk::place

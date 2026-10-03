#include "place/anneal.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <memory>
#include <thread>

#include "place/anneal_state.hpp"
#include "place/global.hpp"

namespace tmk::place {

namespace detail {

namespace {
constexpr std::uint32_t kStageAnneal = 0x504C4145u;  // "PLAE"
}  // namespace

Annealer::Annealer(const Problem& p, const std::vector<std::vector<int>>& pn, const Placement& start, const AnnealOptions& o, int stream)
    : p_(p), pn_(pn), o_(o), pl_(start), L_(p), rng_(o.seed, kStageAnneal, static_cast<std::uint64_t>(stream)) {
  alpha_ = alpha_units(o.alpha_cross_mm);
  beta_ = beta_units(o.beta_congestion);
  L_.reset(pl_);
  for (std::size_t i = 0; i < p.parts.size(); ++i)
    if (p.parts[i].movable) movable_.push_back(static_cast<int>(i));
  for (int f : o.focus)
    if (f >= 0 && z(f) < p.parts.size() && p.parts[z(f)].movable) focus_.push_back(f);
  std::sort(focus_.begin(), focus_.end());
  focus_.erase(std::unique(focus_.begin(), focus_.end()), focus_.end());
  // Swap groups.
  std::map<std::pair<std::uint64_t, long>, int> gid;
  group_.assign(p.parts.size(), -1);
  for (int i : movable_) {
    const Part& pt = p.parts[z(i)];
    const long ang = std::lround(std::fmod(geom::norm_deg(pt.angle0), 90.0) * 1000);
    auto [it, ins] = gid.emplace(std::make_pair(pt.shape_key, ang), static_cast<int>(groups_.size()));
    if (ins) groups_.emplace_back();
    group_[z(i)] = it->second;
    groups_[z(it->second)].push_back(i);
  }
  hp_.assign(p.nets.size(), 0);
  for (std::size_t n = 0; n < p.nets.size(); ++n) {
    hp_[n] = net_hpwl(p, pl_, static_cast<int>(n));
    whp_ += p.nets[n].weight * hp_[n];
  }
  // Airwire segment grid.
  const Box r = p.region.inflated(30'000'000);
  gcell_ = std::max<Coord>(1'000'000, std::max(r.x1 - r.x0, r.y1 - r.y0) / 40);
  gox_ = r.x0;
  goy_ = r.y0;
  gnx_ = static_cast<int>((r.x1 - r.x0) / gcell_) + 1;
  gny_ = static_cast<int>((r.y1 - r.y0) / gcell_) + 1;
  cells_.assign(z(gnx_ * gny_), {});
  net_ids_.assign(p.nets.size(), {});
  in_c_.assign(p.nets.size(), 0);
  if (alpha_ > 0) {
    std::vector<Seg> tmp;
    for (std::size_t n = 0; n < p.nets.size(); ++n)
      if (p.nets[n].signal) {
        mst_of(static_cast<int>(n), tmp);
        insert_segs(static_cast<int>(n), tmp);
      }
    cross_ = count_crossings(p, pl_);
  }
  if (beta_ > 0) {
    cmap_ = o.congestion ? *o.congestion : make_congestion_map(p);
    demand_ = rudy_demand(p, pl_, cmap_);
    ovf_ = total_overflow(cmap_, demand_);
    nbox_.assign(p.nets.size(), Box{});
    for (std::size_t n = 0; n < p.nets.size(); ++n)
      if (rudy_net(p, static_cast<int>(n))) nbox_[n] = net_box(p, pl_, static_cast<int>(n));
    bstamp_.assign(cmap_.size(), 0);
    saved_.assign(cmap_.size(), 0);
  }
  const Coord w = p.region.x1 - p.region.x0, h = p.region.y1 - p.region.y0;
  r0_ = o.refine ? 5'000'000 : std::min<Coord>(std::max(w, h) / 4, 20'000'000);
  r0_ = std::max<Coord>(r0_, 500'000);
  best_pl_ = pl_;
  best_cost_ = start_cost_ = cost();
}

double Annealer::estimate_t0(double p0) {
  if (movable_.empty()) return 1.0;
  double up = 0;
  int nup = 0;
  for (int k = 0; k < 200; ++k) {
    Move m;
    if (!propose_shift(m, r0_)) continue;
    const std::int64_t d = evaluate(m);
    revert(m);
    if (d > 0) {
      up += static_cast<double>(d);
      ++nup;
    }
  }
  return nup ? (up / nup) / -std::log(p0) : 1.0;
}

void Annealer::note_best() {
  if (cost() < best_cost_) {
    best_cost_ = cost();
    best_pl_.pos = pl_.pos;
    best_pl_.rot = pl_.rot;
  }
}

void Annealer::step(Slot& s) {
  if (o_.trace_every > 0 && steps_++ % o_.trace_every == 0) trace_.push_back(pl_);
  // Shift radius adapts to keep the acceptance ratio of shift moves near 0.44 (Lam & Delosme, "Performance
  // of a new annealing schedule", DAC 1988).
  if (s.tried >= 500) {
    const double acc = static_cast<double>(s.acc) / static_cast<double>(s.tried);
    s.radius = std::clamp(s.radius * (acc > 0.44 ? 1.15 : 0.87), 50'000.0, static_cast<double>(r0_));
    s.tried = s.acc = 0;
  }
  if (lns_on_ && o_.lns_rate > 0 && next() < o_.lns_rate) {
    lns_step();
    note_best();
    return;
  }
  const Coord rad = static_cast<Coord>(s.radius);
  Move m;
  const double u = next();
  bool ok;
  const bool shift = u < 0.45;
  if (shift) ok = propose_shift(m, rad);
  else if (u < 0.60) ok = propose_median(m, rad);
  else if (u < 0.72) ok = propose_rotate(m);
  else if (u < 0.84) ok = propose_swap(m);
  else ok = propose_swap_near(m, rad);
  ++moves_;
  if (!ok) return;
  if (shift) ++s.tried;
  const std::int64_t d = evaluate(m);
  const double v = next();
  const bool accept = d <= 0 || v < std::exp(-static_cast<double>(d) / s.t);
  if (!accept || !legal(m)) {
    if (accept) ++illegal_;
    revert(m);
    return;
  }
  if (shift) ++s.acc;
  commit(m);
  ++accepted_;
  note_best();
}

void Annealer::run(std::uint64_t moves, std::chrono::steady_clock::time_point deadline, bool has_deadline) {
  if (movable_.empty()) return;
  const double t0 = estimate_t0(o_.refine ? 0.05 : 0.3);
  constexpr double tend_ratio = 1e-4;
  const double ln_ratio = std::log(tend_ratio);
  Slot s;
  s.radius = static_cast<double>(r0_);
  const std::uint64_t start = moves_;
  for (std::uint64_t k = 0; moves_ - start < moves; ++k) {
    if (has_deadline && (k & 1023) == 0 && std::chrono::steady_clock::now() > deadline) {
      time_limited_ = true;
      break;
    }
    const double frac = static_cast<double>(moves_ - start) / static_cast<double>(moves);
    s.t = t0 * std::exp(ln_ratio * frac);
    lns_on_ = frac >= 1.0 - kLnsPhase;
    step(s);
  }
}

void Annealer::sweep(Slot& s, std::uint64_t n) {
  if (movable_.empty()) return;
  const std::uint64_t start = moves_;
  while (moves_ - start < n) step(s);
}

bool Annealer::propose_shift(Move& m, Coord radius) {
  const int a = pick_movable();
  const Coord dx = static_cast<Coord>((next() * 2 - 1) * static_cast<double>(radius));
  const Coord dy = static_cast<Coord>((next() * 2 - 1) * static_cast<double>(radius));
  const Point q{snap(pl_.pos[z(a)].x + dx), snap(pl_.pos[z(a)].y + dy)};
  if (q == pl_.pos[z(a)]) return false;
  return single(m, a, q, pl_.rot[z(a)]);
}

bool Annealer::median_target(int a, int r, Point& out) const {
  std::vector<std::pair<Coord, int>> xs, ys;
  for (int n : pn_[z(a)]) {
    Coord x0 = LLONG_MAX, x1 = LLONG_MIN, y0 = LLONG_MAX, y1 = LLONG_MIN;
    for (int pi : p_.nets[z(n)].pins) {
      if (p_.pins[z(pi)].part == a) continue;
      const Point q = pl_.pin(p_, pi);
      x0 = std::min(x0, q.x);
      x1 = std::max(x1, q.x);
      y0 = std::min(y0, q.y);
      y1 = std::max(y1, q.y);
    }
    if (x0 > x1) continue;
    const int w = p_.nets[z(n)].weight;
    xs.emplace_back(x0, w);
    xs.emplace_back(x1, w);
    ys.emplace_back(y0, w);
    ys.emplace_back(y1, w);
  }
  if (xs.empty()) return false;
  auto wmedian = [](std::vector<std::pair<Coord, int>>& v) {
    std::sort(v.begin(), v.end());
    long tot = 0;
    for (const auto& e : v) tot += e.second;
    long acc = 0;
    for (const auto& e : v) {
      acc += e.second;
      if (2 * acc >= tot) return e.first;
    }
    return v.back().first;
  };
  // The part's pin centroid sits at the median.
  Point mean{};
  const auto& pins = p_.parts[z(a)].pins;
  for (int pi : pins) mean = mean + p_.pins[z(pi)].off[z(r)];
  if (!pins.empty()) mean = Point{mean.x / static_cast<Coord>(pins.size()), mean.y / static_cast<Coord>(pins.size())};
  out = Point{wmedian(xs) - mean.x, wmedian(ys) - mean.y};
  return true;
}

// Weighted median of the bounding intervals of the part's nets (other pins only): the exact HPWL optimum
// for a point-like part in each axis.
bool Annealer::propose_median(Move& m, Coord radius) {
  const int a = pick_movable();
  Point t;
  if (!median_target(a, pl_.rot[z(a)], t)) return false;
  const Coord j = std::max<Coord>(radius / 8, 20'000);
  const Coord jx = static_cast<Coord>((next() * 2 - 1) * static_cast<double>(j));
  const Coord jy = static_cast<Coord>((next() * 2 - 1) * static_cast<double>(j));
  const Point q{snap(t.x + jx), snap(t.y + jy)};
  if (q == pl_.pos[z(a)]) return false;
  return single(m, a, q, pl_.rot[z(a)]);
}

bool Annealer::propose_rotate(Move& m) {
  const int a = pick_movable();
  const int r0 = pl_.rot[z(a)];
  const int r = (r0 + 1 + static_cast<int>(next_u64() % 3)) & 3;
  const Point c = pl_.pos[z(a)] + body_centre(p_.parts[z(a)], r0);
  const Point q = c - body_centre(p_.parts[z(a)], r);
  return single(m, a, snap(q), static_cast<std::uint8_t>(r));
}

bool Annealer::propose_swap(Move& m) {
  const int a = pick_movable();
  const auto& g = groups_[z(group_[z(a)])];
  if (g.size() < 2) return false;
  int b = g[z(static_cast<int>(next_u64() % (g.size() - 1)))];
  if (b == a) b = g.back();
  const Part& pa = p_.parts[z(a)];
  const Part& pb = p_.parts[z(b)];
  // Same orientation class: angle0 differs by a multiple of 90°, so the absolute angles can be exchanged.
  const int dk = static_cast<int>(std::lround(geom::norm_deg(pb.angle0 - pa.angle0) / 90.0)) & 3;
  m.n = 2;
  m.part[0] = a;
  m.part[1] = b;
  m.old_pos[0] = pl_.pos[z(a)];
  m.old_pos[1] = pl_.pos[z(b)];
  m.old_rot[0] = pl_.rot[z(a)];
  m.old_rot[1] = pl_.rot[z(b)];
  m.pos[0] = pl_.pos[z(b)];
  m.pos[1] = pl_.pos[z(a)];
  m.rot[0] = static_cast<std::uint8_t>((pl_.rot[z(b)] + dk) & 3);
  m.rot[1] = static_cast<std::uint8_t>((pl_.rot[z(a)] - dk + 4) & 3);
  return true;
}

// Exchange the body centres of two movable parts within `radius` of each other (any footprints; each keeps
// its rotation).
bool Annealer::propose_swap_near(Move& m, Coord radius) {
  const int a = pick_movable();
  const Point ca = pl_.pos[z(a)] + body_centre(p_.parts[z(a)], pl_.rot[z(a)]);
  near_.clear();
  L_.neighbours(Box{ca.x - radius, ca.y - radius, ca.x + radius, ca.y + radius}, near_);
  std::sort(near_.begin(), near_.end());
  near_.erase(std::remove_if(near_.begin(), near_.end(), [&](int b) { return b == a || !p_.parts[z(b)].movable; }), near_.end());
  if (near_.empty()) return false;
  const int b = near_[z(static_cast<int>(next_u64() % near_.size()))];
  const Point cb = pl_.pos[z(b)] + body_centre(p_.parts[z(b)], pl_.rot[z(b)]);
  m.n = 2;
  m.part[0] = a;
  m.part[1] = b;
  m.old_pos[0] = pl_.pos[z(a)];
  m.old_pos[1] = pl_.pos[z(b)];
  m.old_rot[0] = m.rot[0] = pl_.rot[z(a)];
  m.old_rot[1] = m.rot[1] = pl_.rot[z(b)];
  m.pos[0] = snap(cb - body_centre(p_.parts[z(a)], m.rot[0]));
  m.pos[1] = snap(ca - body_centre(p_.parts[z(b)], m.rot[1]));
  return true;
}

bool Annealer::single(Move& m, int a, Point q, std::uint8_t r) {
  m.n = 1;
  m.part[0] = a;
  m.old_pos[0] = pl_.pos[z(a)];
  m.old_rot[0] = pl_.rot[z(a)];
  m.pos[0] = q;
  m.rot[0] = r;
  return true;
}

void Annealer::touch_bin(std::size_t k) {
  if (bstamp_[k] == bepoch_) return;
  bstamp_[k] = bepoch_;
  saved_[k] = demand_[k];
  touched_.push_back(k);
}

// Applies the move to pl_ and returns the cost delta (pl_ stays modified; call revert or commit).
std::int64_t Annealer::evaluate(const Move& m) {
  for (int k = 0; k < m.n; ++k) {
    pl_.pos[z(m.part[z(k)])] = m.pos[z(k)];
    pl_.rot[z(m.part[z(k)])] = m.rot[z(k)];
  }
  nets_.clear();
  for (int k = 0; k < m.n; ++k) nets_.insert(nets_.end(), pn_[z(m.part[z(k)])].begin(), pn_[z(m.part[z(k)])].end());
  if (m.n > 1) {
    std::sort(nets_.begin(), nets_.end());
    nets_.erase(std::unique(nets_.begin(), nets_.end()), nets_.end());
  }
  new_hp_.resize(nets_.size());
  std::int64_t d = 0;
  for (std::size_t k = 0; k < nets_.size(); ++k) {
    const int n = nets_[k];
    new_hp_[k] = net_hpwl(p_, pl_, n);
    d += p_.nets[z(n)].weight * (new_hp_[k] - hp_[z(n)]);
  }
  dcross_ = 0;
  last_old_c_ = 0;
  if (alpha_ > 0) {
    cnets_.clear();
    for (int n : nets_)
      if (p_.nets[z(n)].signal) cnets_.push_back(n);
    if (!cnets_.empty()) {
      for (int n : cnets_) in_c_[z(n)] = 1;
      std::int64_t old_c = 0, new_c = 0;
      old_segs_.assign(cnets_.size(), {});
      new_segs_.assign(cnets_.size(), {});
      for (std::size_t k = 0; k < cnets_.size(); ++k) {
        for (int id : net_ids_[z(cnets_[k])]) old_segs_[k].push_back(pool_[z(id)]);
        mst_of(cnets_[k], new_segs_[k]);
      }
      for (std::size_t k = 0; k < cnets_.size(); ++k) {
        for (const Seg& s : old_segs_[k]) old_c += grid_cross(s);
        for (const Seg& s : new_segs_[k]) new_c += grid_cross(s);
      }
      old_c += within(old_segs_);
      new_c += within(new_segs_);
      for (int n : cnets_) in_c_[z(n)] = 0;
      dcross_ = new_c - old_c;
      last_old_c_ = old_c;
    }
  }
  dovf_ = 0;
  if (beta_ > 0) {
    if (++bepoch_ == 0) {
      std::fill(bstamp_.begin(), bstamp_.end(), 0);
      bepoch_ = 1;
    }
    touched_.clear();
    auto touch = [this](std::size_t k) { touch_bin(k); };
    new_box_.resize(nets_.size());
    for (std::size_t k = 0; k < nets_.size(); ++k) {
      const int n = nets_[k];
      if (!rudy_net(p_, n)) continue;
      new_box_[k] = net_box(p_, pl_, n);
      add_box_demand(cmap_, nbox_[z(n)], -1, demand_, touch);
      add_box_demand(cmap_, new_box_[k], +1, demand_, touch);
    }
    for (int k = 0; k < m.n; ++k) {
      const Part& pt = p_.parts[z(m.part[z(k)])];
      for (int pi : pt.pins) {
        const auto& off = p_.pins[z(pi)].off;
        const std::size_t b0 = z(cmap_.bin(m.old_pos[z(k)] + off[m.old_rot[z(k)]]));
        const std::size_t b1 = z(cmap_.bin(m.pos[z(k)] + off[m.rot[z(k)]]));
        if (b0 == b1) continue;
        touch_bin(b0);
        touch_bin(b1);
        demand_[b0] -= cmap_.pin_demand;
        demand_[b1] += cmap_.pin_demand;
      }
    }
    for (std::size_t k : touched_) dovf_ += bin_overflow(demand_[k], cmap_.cap[k]) - bin_overflow(saved_[k], cmap_.cap[k]);
  }
  return d + alpha_ * dcross_ + beta_ * dovf_;
}

bool Annealer::legal(const Move& m) const {
  for (int k = 0; k < m.n; ++k) {
    const int a = m.part[z(k)];
    if (!L_.inside_ok(a, m.pos[z(k)], m.rot[z(k)])) return false;
    if (L_.find_conflict(a, m.pos[z(k)], m.rot[z(k)], m.n > 1 ? m.part[z(1 - k)] : -1) >= 0) return false;
  }
  if (m.n == 2 && L_.pair_conflict(m.part[0], m.pos[0], m.rot[0], m.part[1], m.pos[1], m.rot[1])) return false;
  return true;
}

void Annealer::revert(const Move& m) {
  for (int k = m.n - 1; k >= 0; --k) {
    pl_.pos[z(m.part[z(k)])] = m.old_pos[z(k)];
    pl_.rot[z(m.part[z(k)])] = m.old_rot[z(k)];
  }
  if (beta_ > 0)
    for (std::size_t k : touched_) demand_[k] = saved_[k];
  touched_.clear();
}

void Annealer::commit(const Move& m) {
  for (int k = 0; k < m.n; ++k) L_.insert(m.part[z(k)], m.pos[z(k)], m.rot[z(k)]);
  for (std::size_t k = 0; k < nets_.size(); ++k) {
    const int n = nets_[k];
    whp_ += p_.nets[z(n)].weight * (new_hp_[k] - hp_[z(n)]);
    hp_[z(n)] = new_hp_[k];
  }
  if (alpha_ > 0 && !cnets_.empty()) {
    for (std::size_t k = 0; k < cnets_.size(); ++k) {
      remove_segs(cnets_[k]);
      insert_segs(cnets_[k], new_segs_[k]);
    }
    cross_ += dcross_;
  }
  if (beta_ > 0) {
    for (std::size_t k = 0; k < nets_.size(); ++k)
      if (rudy_net(p_, nets_[k])) nbox_[z(nets_[k])] = new_box_[k];
    ovf_ += dovf_;
  }
  touched_.clear();
}

void Annealer::mst_of(int n, std::vector<Seg>& out) {
  out.clear();
  pts_.clear();
  for (int pi : p_.nets[z(n)].pins) pts_.push_back(pl_.pin(p_, pi));
  net_mst(pts_, n, out);
}

void Annealer::cell_range(const Seg& s, int& x0, int& y0, int& x1, int& y1) const {
  auto cx = [&](Coord v) { return static_cast<int>(std::clamp<Coord>((v - gox_) / gcell_, 0, gnx_ - 1)); };
  auto cy = [&](Coord v) { return static_cast<int>(std::clamp<Coord>((v - goy_) / gcell_, 0, gny_ - 1)); };
  x0 = cx(std::min(s.a.x, s.b.x));
  x1 = cx(std::max(s.a.x, s.b.x));
  y0 = cy(std::min(s.a.y, s.b.y));
  y1 = cy(std::max(s.a.y, s.b.y));
}

void Annealer::insert_segs(int n, const std::vector<Seg>& segs) {
  for (const Seg& s : segs) {
    int id;
    if (!free_.empty()) {
      id = free_.back();
      free_.pop_back();
      pool_[z(id)] = s;
    } else {
      id = static_cast<int>(pool_.size());
      pool_.push_back(s);
      stamp_.push_back(0);
    }
    net_ids_[z(n)].push_back(id);
    int x0, y0, x1, y1;
    cell_range(s, x0, y0, x1, y1);
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) cells_[z(y * gnx_ + x)].push_back(id);
  }
}

void Annealer::remove_segs(int n) {
  for (int id : net_ids_[z(n)]) {
    int x0, y0, x1, y1;
    cell_range(pool_[z(id)], x0, y0, x1, y1);
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        auto& c = cells_[z(y * gnx_ + x)];
        c.erase(std::find(c.begin(), c.end(), id));
      }
    free_.push_back(id);
  }
  net_ids_[z(n)].clear();
}

// Crossings of s with grid segments of nets outside the changed set.
std::int64_t Annealer::grid_cross(const Seg& s) {
  if (++epoch_ == 0) {
    std::fill(stamp_.begin(), stamp_.end(), 0);
    epoch_ = 1;
  }
  int x0, y0, x1, y1;
  cell_range(s, x0, y0, x1, y1);
  std::int64_t c = 0;
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x)
      for (int id : cells_[z(y * gnx_ + x)]) {
        if (stamp_[z(id)] == epoch_) continue;
        stamp_[z(id)] = epoch_;
        const Seg& t = pool_[z(id)];
        if (in_c_[z(t.net)]) continue;
        if (proper_cross(s, t)) ++c;
      }
  return c;
}

std::int64_t Annealer::within(const std::vector<std::vector<Seg>>& sets) {
  std::int64_t c = 0;
  for (std::size_t a = 0; a < sets.size(); ++a)
    for (std::size_t b = a + 1; b < sets.size(); ++b)
      for (const Seg& s : sets[a])
        for (const Seg& t : sets[b])
          if (proper_cross(s, t)) ++c;
  return c;
}

void finish_result(const Problem& p, const AnnealOptions& o, AnnealResult& res) {
  res.whpwl = weighted_hpwl(p, res.pl);
  res.crossings = count_crossings(p, res.pl);
  if (o.beta_congestion > 0) {
    const CongestionMap m = o.congestion ? *o.congestion : make_congestion_map(p);
    res.overflow = rudy_overflow(p, res.pl, m);
  }
}

}  // namespace detail

std::int64_t anneal_cost(const Problem& p, const Placement& pl, double alpha_cross_mm, double beta, const CongestionMap* m) {
  const std::int64_t a = detail::alpha_units(alpha_cross_mm);
  const std::int64_t b = detail::beta_units(beta);
  std::int64_t c = weighted_hpwl(p, pl) + (a > 0 ? a * count_crossings(p, pl) : 0);
  if (b > 0) c += b * rudy_overflow(p, pl, m ? *m : make_congestion_map(p));
  return c;
}

AnnealResult anneal(const Problem& p, const Placement& start, const AnnealOptions& o) {
  const auto pn = part_nets(p);
  const std::uint64_t moves = std::max<std::uint64_t>(20'000, static_cast<std::uint64_t>(o.effort * 4000.0 * p.movable_count()));
  if (o.tempering && o.runs >= 2) return detail::anneal_tempering(p, pn, start, o, moves);
  AnnealResult res;
  const int runs = std::max(1, o.runs);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long>(o.time_limit_s * 1000));
  std::vector<std::unique_ptr<detail::Annealer>> an(z(runs));
  auto work = [&](int r) {
    an[z(r)] = std::make_unique<detail::Annealer>(p, pn, start, o, r);
    an[z(r)]->run(moves, deadline, o.time_limit_s > 0);
  };
  const int threads = std::clamp(o.threads, 1, runs);
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t)
    pool.emplace_back([&, t] {
      for (int r = t; r < runs; r += threads) work(r);
    });
  for (auto& th : pool) th.join();
  for (int r = 0; r < runs; ++r) {
    const auto& a = *an[z(r)];
    res.run_costs.push_back(a.best_cost());
    res.moves += a.moves();
    res.accepted += a.accepted();
    res.illegal += a.illegal();
    res.lns_tried += a.lns_tried();
    res.lns_improved += a.lns_improved();
    res.time_limited |= a.time_limited();
    if (res.best_run < 0 || a.best_cost() < res.cost) {
      res.best_run = r;
      res.cost = a.best_cost();
    }
  }
  const auto& best = *an[z(res.best_run)];
  res.pl = best.best();
  res.start_cost = best.start_cost();
  res.trace = best.trace();
  detail::finish_result(p, o, res);
  return res;
}

}  // namespace tmk::place

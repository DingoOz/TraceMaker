#include "place/anneal.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <thread>

#include "core/rng.hpp"
#include "place/global.hpp"
#include "place/wirelength.hpp"

namespace tmk::place {

namespace {

constexpr std::uint32_t kStageAnneal = 0x504C4145u;  // "PLAE"
constexpr Coord kLattice = 10'000;                   // committed positions on a 10 µm lattice

Coord snap(Coord v) { return (v >= 0 ? (v + kLattice / 2) : (v - kLattice / 2)) / kLattice * kLattice; }

std::int64_t alpha_units(double alpha_mm) { return static_cast<std::int64_t>(std::llround(alpha_mm * 1e6)) * kSignalWeight; }

Point body_centre(const Part& pt, int rot) {
  const Box& b = pt.geom[z(rot)].body;
  return Point{(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2};
}

struct Move {
  int n = 0;  // parts moved (1 or 2)
  int part[2] = {-1, -1};
  Point pos[2], old_pos[2];
  std::uint8_t rot[2] = {0, 0}, old_rot[2] = {0, 0};
};

class Annealer {
 public:
  Annealer(const Problem& p, const std::vector<std::vector<int>>& pn, const Placement& start, const AnnealOptions& o, int run)
      : p_(p), pn_(pn), o_(o), pl_(start), L_(p), rng_(o.seed, kStageAnneal, static_cast<std::uint64_t>(run)) {
    alpha_ = alpha_units(o.alpha_cross_mm);
    L_.reset(pl_);
    for (std::size_t i = 0; i < p.parts.size(); ++i)
      if (p.parts[i].movable) movable_.push_back(static_cast<int>(i));
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
    const Coord w = p.region.x1 - p.region.x0, h = p.region.y1 - p.region.y0;
    r0_ = o.refine ? 5'000'000 : std::min<Coord>(std::max(w, h) / 4, 20'000'000);
    r0_ = std::max<Coord>(r0_, 500'000);
  }

  std::int64_t cost() const { return whp_ + alpha_ * cross_; }

  void run(std::uint64_t moves, std::chrono::steady_clock::time_point deadline, bool has_deadline) {
    best_pl_ = pl_;
    best_cost_ = cost();
    start_cost_ = best_cost_;
    if (movable_.empty()) return;
    // Initial temperature: mean uphill delta of random shift moves (no legality), accepted with p0.
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
    double p0 = o_.refine ? 0.05 : 0.3;
    if (const char* e = std::getenv("TM_P0")) p0 = std::atof(e);  // TEMP tuning
    double tend_ratio = 1e-4;
    if (const char* e = std::getenv("TM_TEND")) tend_ratio = std::atof(e);  // TEMP tuning
    const double t0 = nup ? (up / nup) / -std::log(p0) : 1.0;
    const double t_end = t0 * tend_ratio;
    const double ln_ratio = std::log(t_end / t0);
    // Shift radius adapts to keep the acceptance ratio of shift moves near 0.44 (Lam & Delosme, "Performance
    // of a new annealing schedule", DAC 1988).
    double radius = static_cast<double>(r0_);
    std::uint64_t win_tried = 0, win_acc = 0;
    for (std::uint64_t k = 0; k < moves; ++k) {
      if (has_deadline && (k & 1023) == 0 && std::chrono::steady_clock::now() > deadline) {
        time_limited_ = true;
        break;
      }
      if (win_tried >= 500) {
        const double acc = static_cast<double>(win_acc) / static_cast<double>(win_tried);
        radius = std::clamp(radius * (acc > 0.44 ? 1.15 : 0.87), 50'000.0, static_cast<double>(r0_));
        win_tried = win_acc = 0;
      }
      const double frac = static_cast<double>(k) / static_cast<double>(moves);
      const double t = t0 * std::exp(ln_ratio * frac);
      const Coord rad = static_cast<Coord>(radius);
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
      if (!ok) continue;
      if (shift) ++win_tried;
      const std::int64_t d = evaluate(m);
      const double v = next();
      const bool accept = d <= 0 || v < std::exp(-static_cast<double>(d) / t);
      if (!accept || !legal(m)) {
        if (accept) ++illegal_;
        revert(m);
        continue;
      }
      if (shift) ++win_acc;
      commit(m, d);
      ++accepted_;
      if (cost() < best_cost_) {
        best_cost_ = cost();
        best_pl_.pos = pl_.pos;
        best_pl_.rot = pl_.rot;
      }
    }
  }

  const Placement& best() const { return best_pl_; }
  std::int64_t best_cost() const { return best_cost_; }
  std::int64_t start_cost() const { return start_cost_; }
  std::uint64_t moves() const { return moves_; }
  std::uint64_t accepted() const { return accepted_; }
  std::uint64_t illegal() const { return illegal_; }
  bool time_limited() const { return time_limited_; }

 private:
  double next() { return rng_.uniform(ctr_++); }
  std::uint64_t next_u64() { return rng_.u64(ctr_++); }
  int pick_movable() { return movable_[z(static_cast<int>(next_u64() % movable_.size()))]; }

  bool propose_shift(Move& m, Coord radius) {
    const int a = pick_movable();
    const Coord dx = static_cast<Coord>((next() * 2 - 1) * static_cast<double>(radius));
    const Coord dy = static_cast<Coord>((next() * 2 - 1) * static_cast<double>(radius));
    const Point q{snap(pl_.pos[z(a)].x + dx), snap(pl_.pos[z(a)].y + dy)};
    if (q == pl_.pos[z(a)]) return false;
    return single(m, a, q, pl_.rot[z(a)]);
  }

  // Weighted median of the bounding intervals of the part's nets (other pins only): the exact HPWL optimum
  // for a point-like part in each axis.
  bool propose_median(Move& m, Coord radius) {
    const int a = pick_movable();
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
    // The part's pins centroid sits at the median.
    Point mean{};
    const auto& pins = p_.parts[z(a)].pins;
    for (int pi : pins) mean = mean + p_.pins[z(pi)].off[pl_.rot[z(a)]];
    if (!pins.empty()) mean = Point{mean.x / static_cast<Coord>(pins.size()), mean.y / static_cast<Coord>(pins.size())};
    const Coord j = std::max<Coord>(radius / 8, 20'000);
    const Coord jx = static_cast<Coord>((next() * 2 - 1) * static_cast<double>(j));
    const Coord jy = static_cast<Coord>((next() * 2 - 1) * static_cast<double>(j));
    const Point q{snap(wmedian(xs) - mean.x + jx), snap(wmedian(ys) - mean.y + jy)};
    if (q == pl_.pos[z(a)]) return false;
    return single(m, a, q, pl_.rot[z(a)]);
  }

  bool propose_rotate(Move& m) {
    const int a = pick_movable();
    const int r0 = pl_.rot[z(a)];
    const int r = (r0 + 1 + static_cast<int>(next_u64() % 3)) & 3;
    const Point c = pl_.pos[z(a)] + body_centre(p_.parts[z(a)], r0);
    const Point q = c - body_centre(p_.parts[z(a)], r);
    return single(m, a, Point{snap(q.x), snap(q.y)}, static_cast<std::uint8_t>(r));
  }

  bool propose_swap(Move& m) {
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
  bool propose_swap_near(Move& m, Coord radius) {
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
    const Point qa = cb - body_centre(p_.parts[z(a)], m.rot[0]), qb = ca - body_centre(p_.parts[z(b)], m.rot[1]);
    m.pos[0] = Point{snap(qa.x), snap(qa.y)};
    m.pos[1] = Point{snap(qb.x), snap(qb.y)};
    return true;
  }

  bool single(Move& m, int a, Point q, std::uint8_t r) {
    m.n = 1;
    m.part[0] = a;
    m.old_pos[0] = pl_.pos[z(a)];
    m.old_rot[0] = pl_.rot[z(a)];
    m.pos[0] = q;
    m.rot[0] = r;
    return true;
  }

  // Applies the move to pl_ and returns the cost delta (pl_ stays modified; call revert or commit).
  std::int64_t evaluate(const Move& m) {
    for (int k = 0; k < m.n; ++k) {
      pl_.pos[z(m.part[k])] = m.pos[k];
      pl_.rot[z(m.part[k])] = m.rot[k];
    }
    nets_.clear();
    for (int k = 0; k < m.n; ++k) nets_.insert(nets_.end(), pn_[z(m.part[k])].begin(), pn_[z(m.part[k])].end());
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
      }
    }
    return d + alpha_ * dcross_;
  }

  bool legal(const Move& m) const {
    for (int k = 0; k < m.n; ++k) {
      const int a = m.part[k];
      if (!L_.inside_ok(a, m.pos[k], m.rot[k])) return false;
      if (L_.find_conflict(a, m.pos[k], m.rot[k], m.n > 1 ? m.part[1 - k] : -1) >= 0) return false;
    }
    if (m.n == 2 && L_.pair_conflict(m.part[0], m.pos[0], m.rot[0], m.part[1], m.pos[1], m.rot[1])) return false;
    return true;
  }

  void revert(const Move& m) {
    for (int k = m.n - 1; k >= 0; --k) {
      pl_.pos[z(m.part[k])] = m.old_pos[k];
      pl_.rot[z(m.part[k])] = m.old_rot[k];
    }
  }

  void commit(const Move& m, std::int64_t d) {
    for (int k = 0; k < m.n; ++k) L_.insert(m.part[k], m.pos[k], m.rot[k]);
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
    (void)d;
  }

  void mst_of(int n, std::vector<Seg>& out) {
    out.clear();
    pts_.clear();
    for (int pi : p_.nets[z(n)].pins) pts_.push_back(pl_.pin(p_, pi));
    net_mst(pts_, n, out);
  }

  void cell_range(const Seg& s, int& x0, int& y0, int& x1, int& y1) const {
    auto cx = [&](Coord v) { return static_cast<int>(std::clamp<Coord>((v - gox_) / gcell_, 0, gnx_ - 1)); };
    auto cy = [&](Coord v) { return static_cast<int>(std::clamp<Coord>((v - goy_) / gcell_, 0, gny_ - 1)); };
    x0 = cx(std::min(s.a.x, s.b.x));
    x1 = cx(std::max(s.a.x, s.b.x));
    y0 = cy(std::min(s.a.y, s.b.y));
    y1 = cy(std::max(s.a.y, s.b.y));
  }

  void insert_segs(int n, const std::vector<Seg>& segs) {
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

  void remove_segs(int n) {
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
  std::int64_t grid_cross(const Seg& s) {
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

  static std::int64_t within(const std::vector<std::vector<Seg>>& sets) {
    std::int64_t c = 0;
    for (std::size_t a = 0; a < sets.size(); ++a)
      for (std::size_t b = a + 1; b < sets.size(); ++b)
        for (const Seg& s : sets[a])
          for (const Seg& t : sets[b])
            if (proper_cross(s, t)) ++c;
    return c;
  }

  const Problem& p_;
  const std::vector<std::vector<int>>& pn_;
  const AnnealOptions& o_;
  Placement pl_, best_pl_;
  Legality L_;
  RngStream rng_;
  std::uint64_t ctr_ = 0;
  std::int64_t alpha_ = 0;
  std::vector<int> movable_;
  std::vector<int> group_;
  std::vector<std::vector<int>> groups_;
  std::vector<Coord> hp_;
  std::int64_t whp_ = 0, cross_ = 0;
  Coord r0_ = 0;
  // Segment grid.
  Coord gcell_ = 0, gox_ = 0, goy_ = 0;
  int gnx_ = 1, gny_ = 1;
  std::vector<std::vector<int>> cells_;
  std::vector<Seg> pool_;
  std::vector<int> free_;
  std::vector<std::vector<int>> net_ids_;
  std::vector<std::uint32_t> stamp_;
  std::uint32_t epoch_ = 0;
  std::vector<std::uint8_t> in_c_;
  // Scratch.
  std::vector<int> nets_, cnets_, near_;
  std::vector<Coord> new_hp_;
  std::vector<std::vector<Seg>> old_segs_, new_segs_;
  std::vector<Point> pts_;
  std::int64_t dcross_ = 0;
  // Results.
  std::int64_t best_cost_ = 0, start_cost_ = 0;
  std::uint64_t moves_ = 0, accepted_ = 0, illegal_ = 0;
  bool time_limited_ = false;
};

}  // namespace

std::int64_t anneal_cost(const Problem& p, const Placement& pl, double alpha_cross_mm) {
  const std::int64_t a = alpha_units(alpha_cross_mm);
  return weighted_hpwl(p, pl) + (a > 0 ? a * count_crossings(p, pl) : 0);
}

AnnealResult anneal(const Problem& p, const Placement& start, const AnnealOptions& o) {
  AnnealResult res;
  const auto pn = part_nets(p);
  const int runs = std::max(1, o.runs);
  const std::uint64_t moves = std::max<std::uint64_t>(20'000, static_cast<std::uint64_t>(o.effort * 4000.0 * p.movable_count()));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long>(o.time_limit_s * 1000));
  std::vector<std::unique_ptr<Annealer>> an(z(runs));
  auto work = [&](int r) {
    an[z(r)] = std::make_unique<Annealer>(p, pn, start, o, r);
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
    res.time_limited |= a.time_limited();
    if (res.best_run < 0 || a.best_cost() < res.cost) {
      res.best_run = r;
      res.cost = a.best_cost();
    }
  }
  const auto& best = *an[z(res.best_run)];
  res.pl = best.best();
  res.start_cost = best.start_cost();
  res.whpwl = weighted_hpwl(p, res.pl);
  res.crossings = count_crossings(p, res.pl);
  return res;
}

}  // namespace tmk::place

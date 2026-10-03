#include "place/routable.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "place/anneal.hpp"
#include "place/wirelength.hpp"

namespace tmk::place {

namespace {

constexpr Coord kLattice = 10'000;
Coord snap(Coord v) { return (v >= 0 ? (v + kLattice / 2) : (v - kLattice / 2)) / kLattice * kLattice; }
Point snap(Point q) { return Point{snap(q.x), snap(q.y)}; }
Point centre(const Part& pt, int r) {
  const Box& b = pt.geom[z(r)].body;
  return Point{(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2};
}
std::string fmt_mm(Coord v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(v) / 1e6);
  return buf;
}

// One ECO move: one part (b < 0) or a swap of two parts.
struct EcoMove {
  int a = -1, b = -1;
  Point pa, pb;
  std::uint8_t ra = 0, rb = 0;
  std::string what;
  std::int64_t score = 0;
  std::size_t order = 0;
  std::array<std::int64_t, 8> key() const { return {a, pa.x, pa.y, ra, b, pb.x, pb.y, rb}; }
};

// Problem copy with the failed nets' weights boosted (ranking and re-placement only; reports use the original).
Problem boosted(const Problem& p, const std::vector<std::string>& nets, int boost) {
  Problem q = p;
  if (boost <= 1) return q;
  std::map<std::string, int> idx;
  for (std::size_t n = 0; n < q.nets.size(); ++n) idx.emplace(q.nets[n].name, static_cast<int>(n));
  for (const auto& name : nets) {
    const auto it = idx.find(name);
    if (it != idx.end()) q.nets[z(it->second)].weight = p.nets[z(it->second)].weight * boost;
  }
  return q;
}

// Movable parts at the ends of failed connections or with bodies near their boxes (sorted, unique).
std::vector<int> parts_near(const Problem& p, const Placement& pl, const std::vector<RouteEval::Failure>& failed, Coord corridor) {
  std::vector<Box> boxes;
  std::vector<int> out;
  for (const auto& f : failed) {
    Box b;
    b.add(f.a);
    b.add(f.b);
    boxes.push_back(b.inflated(corridor));
    for (int q : {f.part_a, f.part_b})
      if (q >= 0 && z(q) < p.parts.size() && p.parts[z(q)].movable) out.push_back(q);
  }
  for (std::size_t i = 0; i < p.parts.size(); ++i) {
    if (!p.parts[i].movable) continue;
    const Box& g = p.parts[i].geom[pl.rot[i]].body;
    const Box body{g.x0 + pl.pos[i].x, g.y0 + pl.pos[i].y, g.x1 + pl.pos[i].x, g.y1 + pl.pos[i].y};
    for (const Box& b : boxes)
      if (b.intersects(body)) {
        out.push_back(static_cast<int>(i));
        break;
      }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// Distance from a point to a part's body box (0 inside).
Coord box_dist(const Problem& p, int part, const Placement& pl, Point q) {
  const Box& g = p.parts[z(part)].geom[pl.rot[z(part)]].body;
  const Point o = pl.pos[z(part)];
  const Coord dx = std::max<Coord>({g.x0 + o.x - q.x, 0, q.x - g.x1 - o.x});
  const Coord dy = std::max<Coord>({g.y0 + o.y - q.y, 0, q.y - g.y1 - o.y});
  return std::max(dx, dy);
}

void say(const std::function<void(const std::string&)>& log, const std::string& s) {
  if (log) log(s);
}

std::string describe(const RouteEval& e) {
  return std::to_string(e.unrouted()) + " unrouted (" + std::to_string(e.routed) + "/" + std::to_string(e.connections) + ")";
}

}  // namespace

bool better(const Candidate& a, const Candidate& b) {
  if (a.eval.ok != b.eval.ok) return a.eval.ok;
  if (a.eval.unrouted() != b.eval.unrouted()) return a.eval.unrouted() < b.eval.unrouted();
  return a.hpwl < b.hpwl;
}

EcoResult eco_place(const Problem& p, const Placement& start, const RouteEval& start_eval, const EcoOptions& o, const RouteFn& route) {
  EcoResult res;
  res.pl = start;
  res.eval = start_eval;
  EcoMemory local;
  auto& tried = (o.memory ? *o.memory : local).tried;
  std::vector<std::string> nets;
  const std::int64_t disp_units = static_cast<std::int64_t>(std::llround(o.disp_cost * kSignalWeight));
  const std::int64_t room_units = static_cast<std::int64_t>(std::llround(o.room_gain * kSignalWeight));
  for (int round = 0; round < o.rounds && res.eval.ok && res.eval.unrouted() > 0; ++round) {
    for (const auto& f : res.eval.failed) nets.push_back(f.net);
    std::sort(nets.begin(), nets.end());
    nets.erase(std::unique(nets.begin(), nets.end()), nets.end());
    const Problem q = boosted(p, nets, o.weight_boost);
    CongestionMap cm = make_congestion_map(p);
    std::vector<Point> fp;
    for (const auto& f : res.eval.failed) {
      fp.push_back(f.a);
      fp.push_back(f.b);
    }
    scale_bins(cm, fp, cm.cell, 0.5);
    const std::int64_t base = anneal_cost(q, res.pl, o.alpha_cross_mm, o.beta, &cm);
    const std::vector<int> involved = parts_near(p, res.pl, res.eval.failed, o.corridor);
    Legality L(p);
    L.reset(res.pl);
    std::vector<EcoMove> moves;
    auto add = [&](EcoMove m) {
      const auto k = m.key();
      if (std::find(tried.begin(), tried.end(), k) != tried.end()) return;
      Placement t = res.pl;
      t.pos[z(m.a)] = m.pa;
      t.rot[z(m.a)] = m.ra;
      Coord disp = std::llabs(m.pa.x - res.pl.pos[z(m.a)].x) + std::llabs(m.pa.y - res.pl.pos[z(m.a)].y);
      if (m.b >= 0) {
        t.pos[z(m.b)] = m.pb;
        t.rot[z(m.b)] = m.rb;
        disp += std::llabs(m.pb.x - res.pl.pos[z(m.b)].x) + std::llabs(m.pb.y - res.pl.pos[z(m.b)].y);
      }
      m.score = anneal_cost(q, t, o.alpha_cross_mm, o.beta, &cm) - base + disp_units * disp;
      // Room around the failed pads: the router mostly reports pads it could not escape from ("boxed in"), so
      // moving a neighbour's body away from such a pad (up to o.room) is credited.
      for (int which = 0; which < (m.b >= 0 ? 2 : 1); ++which) {
        const int part = which == 0 ? m.a : m.b;
        for (const auto& f : res.eval.failed)
          for (int e = 0; e < 2; ++e) {
            if ((e == 0 ? f.part_a : f.part_b) == part) continue;  // its own pad moves with it
            const Point pt = e == 0 ? f.a : f.b;
            const Coord d0 = std::min(o.room, box_dist(p, part, res.pl, pt)), d1 = std::min(o.room, box_dist(p, part, t, pt));
            m.score -= room_units * (d1 - d0);
          }
      }
      m.order = moves.size();
      moves.push_back(m);
    };
    for (int a : involved) {
      const Part& pt = p.parts[z(a)];
      const Point pos = res.pl.pos[z(a)];
      const int r = res.pl.rot[z(a)];
      static constexpr int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
      static constexpr const char* dname[4] = {"+x", "-x", "+y", "-y"};
      for (int d = 0; d < 4; ++d)
        for (int s = 1; s <= o.max_steps; ++s) {
          const Point np = snap(pos + Point{dirs[d][0] * s * o.pitch, dirs[d][1] * s * o.pitch});
          if (!L.legal(a, np, r)) continue;
          add(EcoMove{a, -1, np, {}, static_cast<std::uint8_t>(r), 0, pt.ref + " shift " + dname[d] + " " + fmt_mm(s * o.pitch) + " mm", 0, 0});
        }
      const Point c = pos + centre(pt, r);
      for (int dr = 1; dr <= 3; ++dr) {
        const int r2 = (r + dr) & 3;
        const Point np = snap(c - centre(pt, r2));
        if (!L.legal(a, np, r2)) continue;
        add(EcoMove{a, -1, np, {}, static_cast<std::uint8_t>(r2), 0, pt.ref + " rotate " + std::to_string(90 * dr), 0, 0});
      }
      // Swap with an interchangeable part (same footprint, side and orientation class).
      for (std::size_t bi = 0; bi < p.parts.size(); ++bi) {
        const int b = static_cast<int>(bi);
        const Part& pb = p.parts[bi];
        if (b == a || !pb.movable || pb.shape_key != pt.shape_key) continue;
        const double dk_f = geom::norm_deg(pb.angle0 - pt.angle0) / 90.0;
        if (std::fabs(dk_f - std::round(dk_f)) > 1e-6) continue;
        const int dk = static_cast<int>(std::lround(dk_f)) & 3;
        const Point na = res.pl.pos[bi], nb = pos;
        const auto ra = static_cast<std::uint8_t>((res.pl.rot[bi] + dk) & 3);
        const auto rb = static_cast<std::uint8_t>((r - dk + 4) & 3);
        if (!L.inside_ok(a, na, ra) || !L.inside_ok(b, nb, rb)) continue;
        if (L.find_conflict(a, na, ra, b) >= 0 || L.find_conflict(b, nb, rb, a) >= 0 || L.pair_conflict(a, na, ra, b, nb, rb)) continue;
        if (b < a && std::binary_search(involved.begin(), involved.end(), b)) continue;  // the pair is generated once
        add(EcoMove{a, b, na, nb, ra, rb, pt.ref + " swap " + pb.ref, 0, 0});
      }
    }
    res.ranked += static_cast<int>(moves.size());
    std::sort(moves.begin(), moves.end(), [](const EcoMove& x, const EcoMove& y) { return x.score != y.score ? x.score < y.score : x.order < y.order; });
    say(o.log, "eco round " + std::to_string(round + 1) + ": " + std::to_string(involved.size()) + " parts near " +
                   std::to_string(res.eval.failed.size()) + " failed connections, " + std::to_string(moves.size()) + " legal moves");
    int best = -1;
    RouteEval best_eval;
    Placement best_pl;
    for (int i = 0; i < std::min<int>(o.candidates, static_cast<int>(moves.size())); ++i) {
      const EcoMove& m = moves[z(i)];
      tried.push_back(m.key());
      Placement t = res.pl;
      t.pos[z(m.a)] = m.pa;
      t.rot[z(m.a)] = m.ra;
      if (m.b >= 0) {
        t.pos[z(m.b)] = m.pb;
        t.rot[z(m.b)] = m.rb;
      }
      const RouteEval e = route(t);
      ++res.routes;
      say(o.log, "  " + m.what + ": " + describe(e));
      if (!e.ok) continue;
      if (e.unrouted() < res.eval.unrouted() && (best < 0 || e.unrouted() < best_eval.unrouted())) {
        best = i;
        best_eval = e;
        best_pl = t;
      }
    }
    if (best >= 0) {
      res.moves.push_back(moves[z(best)].what + ": " + std::to_string(res.eval.unrouted()) + " -> " + std::to_string(best_eval.unrouted()) + " unrouted");
      res.pl = best_pl;
      res.eval = best_eval;
      ++res.committed;
    }
  }
  return res;
}

LoopResult routability_loop(const Problem& p, std::vector<Candidate> seeds, const LoopOptions& o, const RouteFn& route) {
  LoopResult res;
  auto record = [&](const Candidate& c) {
    Candidate light = c;
    light.pl = Placement{};
    res.tried.push_back(light);
    say(o.log, c.label + ": " + describe(c.eval) + ", HPWL " + fmt_mm(c.hpwl) + " mm");
  };
  int best = -1;
  auto stop = [&] { return o.out_of_time && o.out_of_time(); };
  for (std::size_t i = 0; i < seeds.size(); ++i) {
    auto& s = seeds[i];
    if (i > 0 && !s.eval.ok && stop()) {
      say(o.log, "time limit: remaining seeds not routed");
      break;
    }
    if (!s.eval.ok) {
      s.eval = route(s.pl);
      ++res.routes;
    }
    s.hpwl = total_hpwl(p, s.pl);
    record(s);
    if (best < 0 || better(s, seeds[z(best)])) best = static_cast<int>(i);
  }
  if (best < 0) return res;
  Candidate inc = seeds[z(best)];
  say(o.log, "seed kept: " + inc.label);
  if (o.on_incumbent) o.on_incumbent(inc.label);
  CongestionMap cm = make_congestion_map(p);
  std::vector<std::string> nets;
  std::vector<int> focus;
  EcoMemory memory;
  for (int round = 1; round <= o.rounds && inc.eval.ok && inc.eval.unrouted() > 0; ++round) {
    if (stop()) {
      say(o.log, "time limit: stopped before round " + std::to_string(round));
      break;
    }
    ++res.rounds;
    std::vector<Point> pts;
    for (const auto& f : inc.eval.failed) {
      pts.push_back(f.a);
      pts.push_back(f.b);
      nets.push_back(f.net);
    }
    std::sort(nets.begin(), nets.end());
    nets.erase(std::unique(nets.begin(), nets.end()), nets.end());
    scale_bins(cm, pts, cm.cell, o.penalty);
    const std::vector<int> near = parts_near(p, inc.pl, inc.eval.failed, 1'500'000);
    focus.insert(focus.end(), near.begin(), near.end());
    std::sort(focus.begin(), focus.end());
    focus.erase(std::unique(focus.begin(), focus.end()), focus.end());

    // Re-place around the failures: refine annealing with the routability term over the shrunk capacity map.
    const Problem q = boosted(p, nets, o.weight_boost);
    PlaceOptions po = o.place;
    po.mode = "refine";
    po.beta_congestion = o.beta;
    po.congestion = &cm;
    po.focus = focus;
    po.lns_rate = std::max(po.lns_rate, 0.02);
    po.seed = o.place.seed + static_cast<std::uint64_t>(round) * 1'000'003u;
    Candidate c;
    c.label = "round " + std::to_string(round) + " re-place";
    if (o.place.trace)
      po.trace = [&o, lbl = c.label](const std::string& st, const Placement& x) { o.place.trace(lbl + "|" + st, x); };
    c.pl = inc.pl;
    const PlaceReport pr = place(q, c.pl, po);
    if (pr.legal) {
      c.eval = route(c.pl);
      ++res.routes;
      c.hpwl = total_hpwl(p, c.pl);
      record(c);
      if (c.eval.ok && c.eval.unrouted() < inc.eval.unrouted()) {
        inc = c;
        say(o.log, "  accepted");
        if (o.on_incumbent) o.on_incumbent(inc.label);
      }
    } else {
      say(o.log, c.label + ": not legal, skipped");
    }
    // ECO moves around what is still unrouted.
    if (o.eco_candidates > 0 && inc.eval.unrouted() > 0 && !stop()) {
      EcoOptions eo;
      eo.rounds = 1;
      eo.candidates = o.eco_candidates;
      eo.beta = o.beta;
      eo.alpha_cross_mm = o.place.alpha_cross_mm;
      eo.weight_boost = o.weight_boost;
      eo.log = o.log;
      eo.memory = &memory;
      const EcoResult er = eco_place(p, inc.pl, inc.eval, eo, route);
      res.routes += er.routes;
      if (er.committed > 0) {
        Candidate e;
        e.label = "round " + std::to_string(round) + " eco (" + er.moves.back() + ")";
        e.pl = er.pl;
        e.eval = er.eval;
        e.hpwl = total_hpwl(p, e.pl);
        record(e);
        inc = e;
        if (o.place.trace) o.place.trace(e.label + "|eco", e.pl);
        if (o.on_incumbent) o.on_incumbent(e.label);
      }
    }
  }
  res.best = inc;
  return res;
}

}  // namespace tmk::place

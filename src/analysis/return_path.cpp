// SPDX-License-Identifier: GPL-3.0-or-later
// Reference-return audit (issue #7, doc 05 §42): see return_path.hpp.
#include "analysis/return_path.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>

#include "analysis/region.hpp"
#include "drc/refill.hpp"
#include "geom/clip.hpp"

namespace tmk::analysis {

namespace {

using geom::Point;
using geom::Ring;
using geom::Rings;
using model::NetId;

using detail::dist;
using detail::Pt;
using detail::Region;
using detail::to_point;
using detail::to_pt;
double mm(double nm) { return nm / 1e6; }

// ---------------------------------------------------------------------------------------------------- reference copper

// What the board offers on one copper layer as reference: all zone fills together, and each zone net's own.
struct LayerCopper {
  Region all;
  std::vector<std::pair<NetId, Region>> by_net;
  bool has = false;
  NetId net_at(Pt p) const {
    for (const auto& [n, r] : by_net)
      if (r.contains(p)) return n;
    return 0;
  }
  // The net of the nearest reference copper: a via has an antipad in the plane it passes through, so its own position is a void by design.
  NetId net_near(Pt p, double max_r) const {
    if (const NetId n = net_at(p)) return n;
    for (double r = 0.15e6; r <= max_r; r += 0.15e6)
      for (int k = 0; k < 16; ++k) {
        const double a = k * 3.14159265358979 / 8;
        if (const NetId n = net_at({p.x + r * std::cos(a), p.y + r * std::sin(a)})) return n;
      }
    return 0;
  }
};

// ------------------------------------------------------------------------------------------------------ shortest return

struct Return {
  bool found = false;
  double length = 0;
  std::vector<Pt> path;
};

// The shortest path from a to b that stays on the region's copper: A* over the visibility graph of a, b and the region's vertices. Only
// vertices inside the ellipse with foci a and b and a path length of `bound` can be on a path no longer than `bound`, so the graph is built
// from those and the bound grows until a path is found (then it is the shortest: a shorter one would lie in the same ellipse). Two points
// on different islands of copper have no path at all.
Return shortest_on_copper(const Region& reg, Pt a, Pt b) {
  Return out;
  if (dist(a, b) < 1.0) {
    out.found = true;
    out.path = {a, b};
    return out;
  }
  if (reg.sees(a, b)) {
    out.found = true;
    out.length = dist(a, b);
    out.path = {a, b};
    return out;
  }
  const int ia = reg.island_of(a), ib = reg.island_of(b);
  if (ia < 0 || ib < 0 || ia != ib) return out;  // a split: the copper does not connect them
  const double direct = dist(a, b);
  for (double bound = 1.5 * direct + 0.5e6; bound <= 400e6; bound = bound * 2 + 1e6) {
    std::vector<Pt> nodes{a, b};
    reg.vertices_near(a, b, bound, nodes);
    if (nodes.size() > 2500) nodes.resize(2500);  // a pathological pour: a valid path still comes out, not always the shortest
    const std::size_t n = nodes.size();
    std::vector<double> g(n, std::numeric_limits<double>::infinity());
    std::vector<int> prev(n, -1);
    std::vector<char> done(n, 0);
    using QE = std::pair<double, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<>> open;
    g[0] = 0;
    open.emplace(direct, 0);
    while (!open.empty()) {
      const auto [f, u] = open.top();
      open.pop();
      (void)f;
      if (done[static_cast<std::size_t>(u)]) continue;
      done[static_cast<std::size_t>(u)] = 1;
      if (u == 1) break;
      for (std::size_t v = 0; v < n; ++v) {
        if (done[v] || static_cast<int>(v) == u) continue;
        const double d = dist(nodes[static_cast<std::size_t>(u)], nodes[v]);
        if (g[static_cast<std::size_t>(u)] + d >= g[v]) continue;
        if (g[static_cast<std::size_t>(u)] + d + dist(nodes[v], b) > bound) continue;  // outside the ellipse: not on a path within the bound
        if (!reg.sees(nodes[static_cast<std::size_t>(u)], nodes[v])) continue;
        g[v] = g[static_cast<std::size_t>(u)] + d;
        prev[v] = u;
        open.emplace(g[v] + dist(nodes[v], b), static_cast<int>(v));
      }
    }
    if (done[1]) {
      out.found = true;
      out.length = g[1];
      for (int u = 1; u >= 0; u = prev[static_cast<std::size_t>(u)]) out.path.push_back(nodes[static_cast<std::size_t>(u)]);
      std::reverse(out.path.begin(), out.path.end());
      return out;
    }
  }
  return out;
}

// ----------------------------------------------------------------------------------------------------- traces

struct Chain {
  int layer = 0;
  std::vector<Pt> pts;  // vertices, pad to pad (or to a junction or via)
  double length() const {
    double l = 0;
    for (std::size_t i = 1; i < pts.size(); ++i) l += dist(pts[i - 1], pts[i]);
    return l;
  }
  Pt at(double s) const {  // the point at arc length s
    for (std::size_t i = 1; i < pts.size(); ++i) {
      const double d = dist(pts[i - 1], pts[i]);
      if (s <= d || i + 1 == pts.size()) {
        const double t = d > 0 ? std::clamp(s / d, 0.0, 1.0) : 0.0;
        return {pts[i - 1].x + t * (pts[i].x - pts[i - 1].x), pts[i - 1].y + t * (pts[i].y - pts[i - 1].y)};
      }
      s -= d;
    }
    return pts.empty() ? Pt{} : pts.front();
  }
  // The vertices between arc lengths s0 and s1, with the cut points at the ends.
  std::vector<Pt> piece(double s0, double s1) const {
    std::vector<Pt> out{at(s0)};
    double s = 0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
      s += dist(pts[i - 1], pts[i]);
      if (s > s0 + 1e-6 && s < s1 - 1e-6) out.push_back(pts[i]);
    }
    out.push_back(at(s1));
    return out;
  }
};

// Maximal paths of a net's tracks on one layer: from an end or a junction to the next end or junction.
std::vector<Chain> chains_of(const model::Board& b, NetId net, int layer) {
  std::map<std::pair<Coord, Coord>, std::vector<int>> at;  // end point -> tracks
  std::vector<int> mine;
  for (std::size_t i = 0; i < b.tracks.size(); ++i) {
    const auto& t = b.tracks[i];
    if (t.net != net || t.layer != layer || t.a == t.b) continue;
    mine.push_back(static_cast<int>(i));
    at[{t.a.x, t.a.y}].push_back(static_cast<int>(i));
    at[{t.b.x, t.b.y}].push_back(static_cast<int>(i));
  }
  std::vector<char> used(b.tracks.size(), 0);
  std::vector<Chain> out;
  auto walk = [&](int first, Point from) {
    Chain c;
    c.layer = layer;
    c.pts.push_back(to_pt(from));
    int cur = first;
    Point pos = from;
    for (;;) {
      used[static_cast<std::size_t>(cur)] = 1;
      const auto& t = b.tracks[static_cast<std::size_t>(cur)];
      const Point next = t.a == pos ? t.b : t.a;
      c.pts.push_back(to_pt(next));
      const auto& here = at[{next.x, next.y}];
      if (here.size() != 2) break;
      const int other = here[0] == cur ? here[1] : here[0];
      if (used[static_cast<std::size_t>(other)]) break;
      cur = other;
      pos = next;
    }
    out.push_back(std::move(c));
  };
  for (int i : mine) {  // from every end or junction
    if (used[static_cast<std::size_t>(i)]) continue;
    const auto& t = b.tracks[static_cast<std::size_t>(i)];
    for (const Point p : {t.a, t.b})
      if (at[{p.x, p.y}].size() != 2 && !used[static_cast<std::size_t>(i)]) walk(i, p);
  }
  for (int i : mine)  // a closed loop of degree-2 nodes: broken anywhere
    if (!used[static_cast<std::size_t>(i)]) walk(i, b.tracks[static_cast<std::size_t>(i)].a);
  return out;
}

std::string pad_label(const model::Board& b, const model::Pad& p) {
  return p.footprint >= 0 ? b.footprints[static_cast<std::size_t>(p.footprint)].reference + "." + p.number : p.number;
}

}  // namespace

// --------------------------------------------------------------------------------------------------------- analysis

ReturnPathReport analyse_return_paths(const model::Board& in, const model::DesignRules& rules, const ReturnPathOptions& opt) {
  ReturnPathReport rep;
  rep.options = opt;
  const int nl = in.copper_count();
  // The nets the options select.
  std::set<NetId> selected;
  for (const auto& n : in.nets) {
    if (n.id <= 0 || n.name.empty()) continue;
    for (const auto& p : opt.name_patterns)
      if (model::wildcard_match(p, n.name)) selected.insert(n.id);
    for (const auto& c : opt.net_classes)
      if (rules.class_for(n.name).name == c) selected.insert(n.id);
  }
  if (opt.diff_pairs)
    for (const auto& [x, y] : opt.pair_nets) selected.insert(x), selected.insert(y);
  if (selected.empty() || nl < 2) {
    if (nl < 2) rep.notes.push_back("the board has one copper layer: there is no reference layer");
    return rep;
  }
  // Zones without fills are refilled (the engine's refill, doc 05 §36): the analysis needs the copper that is really there.
  model::Board refilled;
  const model::Board* bp = &in;
  bool any_zone = false, unfilled = false;
  for (const auto& z : in.zones)
    if (!z.rule_area && z.net > 0) {
      any_zone = true;
      unfilled = unfilled || z.fills.empty();
    }
  if ((opt.refill && unfilled) || (opt.refill_always && any_zone)) {
    auto rf = drc::refill_zones(in, rules);
    refilled = std::move(rf.board);
    bp = &refilled;
    rep.notes.push_back(std::string(unfilled ? "zones without fills were" : "zones were") + " refilled by the engine (" + std::to_string(rf.zones) +
                        " zones): KiCad's own refill may differ in detail");
  } else if (unfilled) {
    rep.notes.push_back("some zones have no fill and --no-refill was given: they are not reference copper");
  }
  if (!any_zone) rep.notes.push_back("the board has no copper zones: no layer has reference copper");
  const model::Board& b = *bp;
  // Reference copper per layer.
  std::vector<LayerCopper> lc(static_cast<std::size_t>(nl));
  {
    std::vector<std::vector<Ring>> all(static_cast<std::size_t>(nl));
    std::vector<std::map<NetId, std::vector<Ring>>> per(static_cast<std::size_t>(nl));
    for (const auto& z : b.zones) {
      if (z.rule_area) continue;
      for (const auto& [layer, pts] : z.fills) {
        if (layer < 0 || layer >= nl || pts.size() < 3) continue;
        all[static_cast<std::size_t>(layer)].push_back(pts);
        per[static_cast<std::size_t>(layer)][z.net].push_back(pts);
      }
    }
    for (int l = 0; l < nl; ++l) {
      auto& c = lc[static_cast<std::size_t>(l)];
      if (all[static_cast<std::size_t>(l)].empty()) continue;
      c.has = true;
      c.all = Region(geom::unite(all[static_cast<std::size_t>(l)]));
      for (auto& [net, rings] : per[static_cast<std::size_t>(l)]) c.by_net.emplace_back(net, Region(geom::unite(rings)));
    }
  }
  // Pads and vias that can close a return.
  auto nearest_pad = [&](Pt p, const std::set<NetId>& nets, const model::Pad*& found) {
    double best = opt.stitch_radius_mm * 1e6;
    found = nullptr;
    for (const auto& pad : b.pads) {
      if (!nets.count(pad.net)) continue;
      if (const double d = dist(p, to_pt(pad.pos)); d < best) best = d, found = &pad;
    }
    return found ? best : -1.0;
  };
  const double radius_nm = opt.stitch_radius_mm * 1e6;
  // One net at a time, in net order.
  for (const NetId net : selected) {
    NetReturn nr;
    nr.net = net;
    nr.name = b.nets[static_cast<std::size_t>(net)].name;
    std::vector<Chain> chains;
    for (int l = 0; l < nl; ++l)
      for (auto& c : chains_of(b, net, l)) chains.push_back(std::move(c));
    if (std::any_of(b.arcs.begin(), b.arcs.end(), [&](const auto& a) { return a.net == net; })) nr.warnings.push_back("arc tracks are not analysed");
    if (chains.empty()) {
      nr.warnings.push_back("no routed copper");
      rep.nets.push_back(std::move(nr));
      continue;
    }
    // The reference layer of each signal layer.
    std::map<int, int> ref_of;
    for (const auto& c : chains) {
      if (ref_of.count(c.layer)) continue;
      int forced = -2;
      for (const auto& [pat, lname] : opt.reference_layers)
        if (model::wildcard_match(pat, nr.name)) forced = b.copper_index(lname);
      if (forced >= 0) {
        ref_of[c.layer] = forced;
        continue;
      }
      // The adjacent layer (N-1 or N+1) with the most copper under the net's traces on this layer.
      int best = -1;
      double best_cov = 0;
      for (const int cand : {c.layer + 1, c.layer - 1}) {
        if (cand < 0 || cand >= nl || !lc[static_cast<std::size_t>(cand)].has) continue;
        double cov = 0, tot = 0;
        for (const auto& c2 : chains) {
          if (c2.layer != c.layer) continue;
          const double len = c2.length();
          const int n = std::max(1, static_cast<int>(len / (opt.sample_mm * 4e6)));
          for (int i = 0; i <= n; ++i) {
            ++tot;
            cov += lc[static_cast<std::size_t>(cand)].all.contains(c2.at(len * i / n)) ? 1 : 0;
          }
        }
        if (tot > 0 && cov / tot > best_cov + 1e-9) best_cov = cov / tot, best = cand;
      }
      ref_of[c.layer] = best;
    }
    for (const auto& [l, r] : ref_of) nr.layers.push_back(l), nr.reference_layers.push_back(r);
    // Each chain: copper under it, the voids, the return.
    for (const auto& c : chains) {
      const double len = c.length();
      nr.signal_mm += mm(len);
      std::vector<Pt> sig = c.pts;
      nr.signal.emplace_back(c.layer, std::vector<Point>());
      for (const auto& p : sig) nr.signal.back().second.push_back(to_point(p));
      const int rl = ref_of[c.layer];
      if (rl < 0 || !lc[static_cast<std::size_t>(rl)].has) {
        nr.uncovered_mm += mm(len);
        continue;
      }
      const LayerCopper& ref = lc[static_cast<std::size_t>(rl)];
      const double step = opt.sample_mm * 1e6;
      const int n = std::max(1, static_cast<int>(std::ceil(len / step)));
      std::vector<char> cov(static_cast<std::size_t>(n) + 1);
      for (int i = 0; i <= n; ++i) cov[static_cast<std::size_t>(i)] = ref.all.contains(c.at(len * i / n));
      // The boundary between a covered and an uncovered sample, to within a micrometre.
      auto refine = [&](double s_cov, double s_unc) {
        for (int k = 0; k < 14; ++k) {
          const double m = (s_cov + s_unc) / 2;
          (ref.all.contains(c.at(m)) ? s_cov : s_unc) = m;
        }
        return s_cov;  // the last position that still has copper
      };
      std::vector<Pt> ret;  // the return along this chain
      double carried = 0;   // arc length already copied into ret
      int i = 0;
      while (i <= n) {
        if (cov[static_cast<std::size_t>(i)]) {
          ++i;
          continue;
        }
        int j = i;
        while (j + 1 <= n && !cov[static_cast<std::size_t>(j) + 1]) ++j;
        const double s_a = len * i / n, s_b = len * j / n;
        if (s_b - s_a + len / n < opt.min_void_mm * 1e6) {
          i = j + 1;
          continue;
        }
        // The void runs from sample i to sample j.
        double e_s = i == 0 ? 0 : refine(len * (i - 1) / n, s_a);
        double x_s = j == n ? len : len * (j + 1) / n;
        if (j != n) {
          double lo = len * (j + 1) / n, hi = s_b;  // lo has copper, hi has none
          for (int k = 0; k < 14; ++k) {
            const double m = (lo + hi) / 2;
            (ref.all.contains(c.at(m)) ? lo : hi) = m;
          }
          x_s = lo;
        }
        Pt E = c.at(e_s), X = c.at(x_s);
        // An end of the trace over the void: the return starts at the nearest reference copper.
        Pt snap;
        if (!ref.all.contains(E) && ref.all.nearest_boundary(E, 20e6, snap)) E = snap;
        if (!ref.all.contains(X) && ref.all.nearest_boundary(X, 20e6, snap)) X = snap;
        ReturnVoid v;
        v.layer = c.layer;
        v.entry = to_point(E);
        v.exit = to_point(X);
        v.signal_mm = mm(x_s - e_s);
        const Return r = shortest_on_copper(ref.all, E, X);
        if (!r.found) {
          v.no_path = true;
          v.return_mm = 0;
          v.path = {v.entry, v.exit};
          ++nr.split_count;
        } else {
          v.return_mm = mm(r.length);
          v.detour_mm = std::max(0.0, v.return_mm - v.signal_mm);
          for (const auto& p : r.path) v.path.push_back(to_point(p));
          // The farthest the return gets from the trace: sampled along the path against the trace's piece.
          const auto trace = c.piece(e_s, x_s);
          double far = 0;
          Pt far_at = E;
          for (std::size_t k = 1; k < r.path.size(); ++k) {
            const double seg = dist(r.path[k - 1], r.path[k]);
            const int m = std::max(1, static_cast<int>(seg / 20'000.0));
            for (int q = 0; q <= m; ++q) {
              const Pt p{r.path[k - 1].x + (r.path[k].x - r.path[k - 1].x) * q / m, r.path[k - 1].y + (r.path[k].y - r.path[k - 1].y) * q / m};
              double best = std::numeric_limits<double>::infinity();
              for (std::size_t t = 1; t < trace.size(); ++t) {
                const double dx = trace[t].x - trace[t - 1].x, dy = trace[t].y - trace[t - 1].y, l2 = dx * dx + dy * dy;
                const double u = l2 > 0 ? std::clamp(((p.x - trace[t - 1].x) * dx + (p.y - trace[t - 1].y) * dy) / l2, 0.0, 1.0) : 0.0;
                best = std::min(best, dist(p, {trace[t - 1].x + u * dx, trace[t - 1].y + u * dy}));
              }
              if (best > far) far = best, far_at = p;
            }
          }
          v.max_separation_mm = mm(far);
          v.farthest = to_point(far_at);
        }
        ++nr.void_count;
        nr.detour_mm += v.detour_mm;
        nr.max_separation_mm = std::max(nr.max_separation_mm, v.max_separation_mm);
        // The return over the copper before this void is the trace itself; the void's part is the detour.
        if (e_s > carried) {
          const auto piece = c.piece(carried, e_s);
          ret.insert(ret.end(), piece.begin(), piece.end());
        }
        for (const auto& p : v.path) ret.push_back(to_pt(p));
        carried = x_s;
        nr.voids.push_back(std::move(v));
        i = j + 1;
      }
      if (carried < len) {
        const auto piece = c.piece(carried, len);
        ret.insert(ret.end(), piece.begin(), piece.end());
      }
      nr.ret.emplace_back(rl, std::vector<Point>());
      for (const auto& p : ret) nr.ret.back().second.push_back(to_point(p));
    }
    // Layer changes: a via of the net where traces on two layers meet.
    for (const auto& v : b.vias) {
      if (v.net != net) continue;
      std::set<int> on;
      for (const auto& c : chains)
        for (const Pt p : {c.pts.front(), c.pts.back()})
          if (dist(p, to_pt(v.pos)) < 1000.0) on.insert(c.layer);
      if (on.size() < 2) continue;
      ReturnLayerChange ch;
      ch.at = v.pos;
      ch.from_layer = *on.begin();
      ch.to_layer = *on.rbegin();
      const int ra = ref_of.count(ch.from_layer) ? ref_of[ch.from_layer] : -1, rb = ref_of.count(ch.to_layer) ? ref_of[ch.to_layer] : -1;
      const NetId na = ra >= 0 ? lc[static_cast<std::size_t>(ra)].net_near(to_pt(v.pos), 2e6) : 0, nb = rb >= 0 ? lc[static_cast<std::size_t>(rb)].net_near(to_pt(v.pos), 2e6) : 0;
      ch.ref_from = na > 0 ? b.nets[static_cast<std::size_t>(na)].name : "";
      ch.ref_to = nb > 0 ? b.nets[static_cast<std::size_t>(nb)].name : "";
      if (na == 0 || nb == 0) {
        ch.closure = "no reference";
        ch.missing = true;
      } else if (na == nb) {
        double best = radius_nm;
        const Point* where = nullptr;
        for (const auto& s : b.vias)
          if (&s != &v && s.net == na) {
            if (const double d = dist(to_pt(v.pos), to_pt(s.pos)); d < best) best = d, where = &s.pos;
          }
        for (const auto& pad : b.pads)
          if (pad.net == na && pad.drill_x > 0) {
            if (const double d = dist(to_pt(v.pos), to_pt(pad.pos)); d < best) best = d, where = &pad.pos;
          }
        ch.closure = where ? "stitch via" : "none";
        ch.missing = !where;
        if (where) ch.distance_mm = mm(best), ch.closure_at = *where;
      } else {
        double best = radius_nm;
        const model::Footprint* cap = nullptr;
        for (const auto& fp : b.footprints) {
          if (fp.reference.empty() || fp.reference[0] != 'C') continue;
          bool has_a = false, has_b = false;
          for (int pi : fp.pads) {
            has_a = has_a || b.pads[static_cast<std::size_t>(pi)].net == na;
            has_b = has_b || b.pads[static_cast<std::size_t>(pi)].net == nb;
          }
          if (!has_a || !has_b) continue;
          if (const double d = dist(to_pt(v.pos), to_pt(fp.pos)); d < best) best = d, cap = &fp;
        }
        ch.closure = cap ? "capacitor" : "none";
        ch.missing = !cap;
        if (cap) ch.distance_mm = mm(best), ch.closure_at = cap->pos;
      }
      if (ch.missing) ++nr.missing_stitches;
      nr.changes.push_back(std::move(ch));
    }
    // The ends at pads: the assumed return pad is the nearest pad of the reference net, not one assumed ground ball.
    for (const auto& c : chains)
      for (const Pt p : {c.pts.front(), c.pts.back()})
        for (const auto& pad : b.pads) {
          if (pad.net != net || dist(p, to_pt(pad.pos)) > 1000.0 || !(pad.copper & model::layer_bit(c.layer))) continue;
          ReturnEnd e;
          e.at = pad.pos;
          e.pad = pad_label(b, pad);
          const int rl = ref_of[c.layer];
          std::set<NetId> ref_nets;
          if (rl >= 0)
            for (const auto& [rn, reg] : lc[static_cast<std::size_t>(rl)].by_net) ref_nets.insert(rn);
          const model::Pad* found = nullptr;
          const double d = nearest_pad(to_pt(pad.pos), ref_nets, found);
          if (found) e.ref_pad = pad_label(b, *found), e.distance_mm = mm(d), e.ref_at = found->pos;
          nr.ends.push_back(std::move(e));
          break;
        }
    // Thresholds are warnings, not DRC errors.
    char buf[160];
    if (nr.detour_mm > opt.warn_detour_mm) {
      std::snprintf(buf, sizeof buf, "return detours %.2f mm (over %.2f mm)", nr.detour_mm, opt.warn_detour_mm);
      nr.warnings.push_back(buf);
    }
    if (nr.max_separation_mm > opt.warn_separation_mm) {
      std::snprintf(buf, sizeof buf, "return is %.2f mm from the trace at the farthest (over %.2f mm)", nr.max_separation_mm, opt.warn_separation_mm);
      nr.warnings.push_back(buf);
    }
    if (nr.split_count > 0) nr.warnings.push_back(std::to_string(nr.split_count) + " void(s) the reference copper does not connect across (a split plane): no return path on that layer");
    if (nr.missing_stitches > 0) nr.warnings.push_back(std::to_string(nr.missing_stitches) + " layer change(s) without a stitching via or capacitor within " + std::to_string(opt.stitch_radius_mm).substr(0, 4) + " mm");
    if (nr.uncovered_mm > 0.05) {
      std::snprintf(buf, sizeof buf, "%.1f mm of trace has no reference layer (no zone fill on the adjacent layers)", nr.uncovered_mm);
      nr.warnings.push_back(buf);
    }
    rep.nets.push_back(std::move(nr));
  }
  return rep;
}

// ------------------------------------------------------------------------------------------------------------ output

namespace {

nlohmann::json pts_json(const std::vector<Point>& pts) {
  nlohmann::json a = nlohmann::json::array();
  for (const auto& p : pts) a.push_back({std::round(mm(static_cast<double>(p.x)) * 1000) / 1000, std::round(mm(static_cast<double>(p.y)) * 1000) / 1000});
  return a;
}
double r3(double v) { return std::round(v * 1000) / 1000; }

}  // namespace

nlohmann::json return_path_json(const model::Board& b, const ReturnPathReport& r) {
  using nlohmann::json;
  json j;
  j["options"] = {{"stitch_radius_mm", r.options.stitch_radius_mm}, {"warn_detour_mm", r.options.warn_detour_mm}, {"warn_separation_mm", r.options.warn_separation_mm},
                  {"min_void_mm", r.options.min_void_mm}, {"sample_mm", r.options.sample_mm}};
  j["notes"] = r.notes;
  j["nets"] = json::array();
  auto lname = [&](int l) { return l >= 0 && l < b.copper_count() ? b.copper_name(l) : std::string(); };
  for (const auto& n : r.nets) {
    json nj;
    nj["net"] = n.name;
    nj["layers"] = json::array();
    for (std::size_t i = 0; i < n.layers.size(); ++i) nj["layers"].push_back({{"layer", lname(n.layers[i])}, {"reference", lname(n.reference_layers[i])}});
    nj["signal_mm"] = r3(n.signal_mm);
    nj["detour_mm"] = r3(n.detour_mm);
    nj["max_separation_mm"] = r3(n.max_separation_mm);
    nj["void_crossings"] = n.void_count;
    nj["splits"] = n.split_count;
    nj["missing_stitches"] = n.missing_stitches;
    nj["uncovered_mm"] = r3(n.uncovered_mm);
    nj["warnings"] = n.warnings;
    nj["voids"] = json::array();
    for (const auto& v : n.voids)
      nj["voids"].push_back({{"layer", lname(v.layer)}, {"entry", pts_json({v.entry})[0]}, {"exit", pts_json({v.exit})[0]}, {"signal_mm", r3(v.signal_mm)},
                             {"return_mm", r3(v.return_mm)}, {"detour_mm", r3(v.detour_mm)}, {"max_separation_mm", r3(v.max_separation_mm)},
                             {"no_return_path", v.no_path}});
    nj["layer_changes"] = json::array();
    for (const auto& c : n.changes)
      nj["layer_changes"].push_back({{"at", pts_json({c.at})[0]}, {"from", lname(c.from_layer)}, {"to", lname(c.to_layer)}, {"reference_from", c.ref_from},
                                     {"reference_to", c.ref_to}, {"closure", c.closure}, {"distance_mm", c.distance_mm < 0 ? json(nullptr) : json(r3(c.distance_mm))},
                                     {"missing", c.missing}});
    nj["ends"] = json::array();
    for (const auto& e : n.ends)
      nj["ends"].push_back({{"pad", e.pad}, {"return_pad", e.ref_pad}, {"distance_mm", e.distance_mm < 0 ? json(nullptr) : json(r3(e.distance_mm))}});
    j["nets"].push_back(std::move(nj));
  }
  return j;
}

std::string return_path_message(const model::Board& b, const NetReturn& n) {
  using nlohmann::json;
  json m;
  m["type"] = "return_path";
  m["net"] = n.net;
  m["name"] = n.name;
  m["detour_mm"] = r3(n.detour_mm);
  m["max_separation_mm"] = r3(n.max_separation_mm);
  m["voids"] = n.void_count;
  m["splits"] = n.split_count;
  m["missing_stitches"] = n.missing_stitches;
  auto line = [&](const std::pair<int, std::vector<Point>>& pl) {
    json a = json::array();
    for (const auto& p : pl.second) a.push_back({p.x, p.y});
    return json{{"layer", pl.first}, {"pts", a}};
  };
  m["signal"] = json::array();
  for (const auto& s : n.signal) m["signal"].push_back(line(s));
  m["return"] = json::array();
  for (const auto& s : n.ret) m["return"].push_back(line(s));
  json co = json::array();
  auto callout = [&](const char* kind, Point p, const std::string& text) { co.push_back({{"kind", kind}, {"p", {p.x, p.y}}, {"text", text}}); };
  char buf[160];
  for (const auto& e : n.ends) {
    callout("signal_pad", e.at, "signal pad " + e.pad);
    if (!e.ref_pad.empty()) {
      std::snprintf(buf, sizeof buf, "assumed return pad %s, %.2f mm", e.ref_pad.c_str(), e.distance_mm);
      callout("ref_pad", e.ref_at, buf);
    }
  }
  for (const auto& c : n.changes) {
    callout("signal_via", c.at,
            c.ref_from.empty() && c.ref_to.empty() ? std::string("signal via: no reference copper near it")
                                                    : "signal via: " + (c.ref_from.empty() ? std::string("no reference") : c.ref_from) + " to " + (c.ref_to.empty() ? std::string("no reference") : c.ref_to));
    if (c.closure == "stitch via" || c.closure == "capacitor") {
      std::snprintf(buf, sizeof buf, "%s, %.2f mm", c.closure == "capacitor" ? "capacitor" : "return via", c.distance_mm);
      callout("return_via", c.closure_at, buf);
    } else {
      callout("missing_stitch", c.at, "no stitching via or capacitor within the radius");
    }
  }
  for (const auto& v : n.voids)
    if (!v.no_path) {
      std::snprintf(buf, sizeof buf, "separation %.2f mm, detour %.2f mm", v.max_separation_mm, v.detour_mm);
      callout("separation", v.farthest, buf);
    } else {
      callout("split", v.entry, "no return path: the reference copper does not connect across");
    }
  m["callouts"] = co;
  (void)b;
  return m.dump();
}

std::vector<std::string> return_path_lines(const model::Board& b, const ReturnPathReport& r) {
  std::vector<std::string> out;
  for (const auto& n : r.nets) {
    std::string layers;
    for (std::size_t i = 0; i < n.layers.size(); ++i)
      layers += (i ? ", " : "") + b.copper_name(n.layers[i]) + (n.reference_layers[i] >= 0 ? " over " + b.copper_name(n.reference_layers[i]) : " (no reference)");
    char buf[320];
    std::snprintf(buf, sizeof buf, "%-24s %-28s %7.1f mm  voids %d  detour %5.2f mm  max separation %5.2f mm  missing stitches %d%s", n.name.c_str(), layers.c_str(), n.signal_mm,
                  n.void_count, n.detour_mm, n.max_separation_mm, n.missing_stitches, n.warnings.empty() ? "" : "  WARN");
    out.push_back(buf);
    for (const auto& w : n.warnings) out.push_back("    warning: " + w);
  }
  for (const auto& note : r.notes) out.push_back("note: " + note);
  return out;
}

}  // namespace tmk::analysis

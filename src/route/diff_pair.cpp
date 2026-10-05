#include "route/diff_pair.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include "geom/shape.hpp"

namespace tmk::route {

using geom::Point;
using model::NetId;

namespace {

constexpr int kDx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int kDy[8] = {0, -1, -1, -1, 0, 1, 1, 1};
// Offsets are rounded to the nanometre; this margin per side keeps the real gap at or above the rule.
constexpr Coord kRoundMargin = 1'000;

Coord ceil_half(Coord v) { return (v + 1) / 2; }

}  // namespace

PairRule pair_rule(const model::Board& b, const model::DesignRules& rules, const drc::RuleEngine& re, NetId a, NetId c, Coord via_mask) {
  const auto& na = rules.class_for(b.nets[static_cast<std::size_t>(a)].name);
  const auto& nc = rules.class_for(b.nets[static_cast<std::size_t>(c)].name);
  const auto& m = rules.minimums;
  PairRule r;
  // Width: the class's diff-pair width where the project sets one, else the track width; a custom track_width rule
  // on the net raises it (opt wins when given). The wider of the two halves' classes, never below the board minimum.
  auto class_w = [](const model::NetClass& k) { return k.has_diff_pair_gap ? k.diff_pair_width : k.track_width; };
  Coord w = std::max({class_w(na), class_w(nc), m.track_width});
  for (NetId n : {a, c})
    if (const auto k = re.net_constraint(n, "track_width")) {
      if (k->opt) w = std::max(w, *k->opt);
      if (k->min) w = std::max(w, *k->min);
    }
  r.width = w;
  // The clearance KiCad requires between the halves: the class clearance, relaxed to the class diff-pair gap only for
  // pairs it recognises by name (drc::RuleEngine::clearance does the same), never below the board minimum.
  const Coord clr = std::max(na.clearance, nc.clearance);
  Coord req = clr;
  if (re.coupled_diff_pair(a, c)) req = std::max(std::min(clr, na.diff_pair_gap), std::min(clr, nc.diff_pair_gap));
  req = std::max(req, m.clearance);
  r.required_gap = req;
  Coord gap = req;
  r.source = "clearance";
  if (na.has_diff_pair_gap || nc.has_diff_pair_gap) {
    gap = std::max(na.has_diff_pair_gap ? na.diff_pair_gap : 0, nc.has_diff_pair_gap ? nc.diff_pair_gap : 0);
    r.source = "net class";
  }
  if (const auto k = re.net_constraint(a, "diff_pair_gap"); k && (k->opt || k->min)) {
    gap = k->opt ? *k->opt : *k->min;
    r.source = "rule";
  }
  r.gap = std::max(gap, req);
  r.offset = ceil_half(r.width + r.gap) + kRoundMargin;
  // Vias: the class via raised to the board minimums (as the router's own vias), side by side at the via gap.
  r.via_drill = std::max({na.via_drill, nc.via_drill, m.through_hole_diameter});
  r.via_diameter = std::max({na.via_diameter, nc.via_diameter, m.via_diameter, r.via_drill + 2 * m.via_annular_width});
  Coord vgap = req;
  if (na.has_diff_pair_gap) vgap = std::max(vgap, na.diff_pair_via_gap);
  if (nc.has_diff_pair_gap) vgap = std::max(vgap, nc.diff_pair_via_gap);
  if (via_mask > 0) vgap = std::max(vgap, 2 * via_mask + 1'000);  // as route::Obstacles checks untented vias
  r.via_offset = std::max({r.offset, ceil_half(r.via_diameter + vgap), ceil_half(r.via_drill + m.hole_to_hole),
                           ceil_half(r.via_drill / 2 + r.via_diameter / 2 + m.hole_clearance)}) +
                 kRoundMargin;
  if (const auto k = re.net_constraint(a, "diff_pair_uncoupled"); k && k->max) r.max_uncoupled = *k->max;
  return r;
}

Dir2 unit_dir(int d) {
  const double l = (d & 1) ? std::numbers::sqrt2 : 1.0;
  return {kDx[d] / l, kDy[d] / l};
}

Dir2 left_normal(int d) {
  const Dir2 u = unit_dir(d);
  return {-u.y, u.x};
}

Point offset_point(Point p, Dir2 v, double s) {
  return {p.x + geom::kiround(v.x * s), p.y + geom::kiround(v.y * s)};
}

Point miter_point(Point c, int d1, int d2, double s) {
  const Dir2 n1 = left_normal(d1), n2 = left_normal(d2);
  const double k = 1.0 + n1.x * n2.x + n1.y * n2.y;  // 1 + cos(turn); turns of at most 90 degrees only
  return offset_point(c, Dir2{(n1.x + n2.x) / k, (n1.y + n2.y) / k}, s);
}

PairStats measure_pair(const std::vector<model::Track>& tracks, const std::vector<model::Via>& vias, NetId a, NetId c, Coord coupled_gap) {
  PairStats st;
  std::vector<const model::Track*> ta, tc;
  for (const auto& t : tracks) {
    if (t.net == a) ta.push_back(&t);
    if (t.net == c) tc.push_back(&t);
  }
  for (const auto& v : vias) {
    if (v.net == a) ++st.vias_a;
    if (v.net == c) ++st.vias_b;
  }
  std::vector<double> gaps;
  auto side = [&](const std::vector<const model::Track*>& mine, const std::vector<const model::Track*>& other, double& len, double& coupled) {
    for (const auto* t : mine) {
      const double dx = static_cast<double>(t->b.x - t->a.x), dy = static_cast<double>(t->b.y - t->a.y);
      const double L = std::hypot(dx, dy);
      len += L;
      if (L <= 0) continue;
      const int k = std::max(1, static_cast<int>(std::ceil(L / 20'000.0)));  // 20 um samples
      for (int i = 0; i < k; ++i) {
        const double f = (i + 0.5) / k;
        const Point p{t->a.x + geom::kiround(dx * f), t->a.y + geom::kiround(dy * f)};
        double best = 1e300;
        for (const auto* u : other) {
          if (u->layer != t->layer) continue;
          const double ux = static_cast<double>(u->b.x - u->a.x), uy = static_cast<double>(u->b.y - u->a.y), UL = std::hypot(ux, uy);
          if (UL <= 0 || std::fabs(dx * uy - dy * ux) > 0.05 * L * UL) continue;  // parallel within 3 degrees only
          best = std::min(best, static_cast<double>(geom::point_seg_dist(p, u->a, u->b)) - static_cast<double>(t->width + u->width) / 2);
        }
        if (best <= static_cast<double>(coupled_gap)) {
          coupled += L / k;
          gaps.push_back(best);
        }
      }
    }
  };
  side(ta, tc, st.length_a, st.coupled_a);
  side(tc, ta, st.length_b, st.coupled_b);
  if (!gaps.empty()) {
    std::sort(gaps.begin(), gaps.end());
    st.gap_min = gaps.front();
    st.gap_median = gaps[gaps.size() / 2];
  }
  return st;
}

std::vector<std::pair<NetId, NetId>> named_pairs(const model::Board& b, const drc::RuleEngine& re) {
  std::map<std::string, NetId> by_name;
  for (std::size_t i = 1; i < b.nets.size(); ++i) by_name.emplace(b.nets[i].name, static_cast<NetId>(i));
  std::vector<std::pair<NetId, NetId>> out;
  for (std::size_t i = 1; i < b.nets.size(); ++i) {
    const std::string& n = b.nets[i].name;
    if (n.size() < 2 || (n.back() != 'P' && n.back() != '+')) continue;
    const auto it = by_name.find(n.substr(0, n.size() - 1) + (n.back() == 'P' ? "N" : "-"));
    if (it != by_name.end() && re.coupled_diff_pair(static_cast<NetId>(i), it->second)) out.emplace_back(static_cast<NetId>(i), it->second);
  }
  return out;
}

}  // namespace tmk::route

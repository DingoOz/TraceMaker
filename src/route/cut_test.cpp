// SPDX-License-Identifier: GPL-3.0-or-later
#include "route/cut_test.hpp"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <map>
#include <unordered_map>

#include "drc/copper.hpp"
#include "drc/rule_engine.hpp"
#include "geom/shape.hpp"

namespace tmk::route {
namespace {

using geom::Point;
using geom::Shape;
using LD = long double;

// Closest points of two segments (Ericson, Real-Time Collision Detection, 5.1.9).
LD closest_seg_seg(Point p1, Point q1, Point p2, Point q2, LD& x1, LD& y1, LD& x2, LD& y2, bool* parallel = nullptr) {
  const LD d1x = LD(q1.x - p1.x), d1y = LD(q1.y - p1.y), d2x = LD(q2.x - p2.x), d2y = LD(q2.y - p2.y);
  const LD rx = LD(p1.x - p2.x), ry = LD(p1.y - p2.y);
  const LD a = d1x * d1x + d1y * d1y, e = d2x * d2x + d2y * d2y, f = d2x * rx + d2y * ry;
  LD s = 0, t = 0;
  if (a <= 0 && e <= 0) {
  } else if (a <= 0) {
    t = std::clamp(f / e, LD(0), LD(1));
  } else {
    const LD c = d1x * rx + d1y * ry;
    if (e <= 0) {
      s = std::clamp(-c / a, LD(0), LD(1));
    } else {
      const LD b = d1x * d2x + d1y * d2y, den = a * e - b * b;
      if (den <= LD(1e-12) * a * e) {  // parallel: the middle of the overlap (a canonical cut between two facing edges)
        const LD s0 = std::clamp(-c / a, LD(0), LD(1)), s1 = std::clamp((b - c) / a, LD(0), LD(1));
        s = (s0 + s1) / 2;
        if (parallel) *parallel = std::fabs(s1 - s0) > LD(1e-9);  // facing edges that overlap, not two collinear edges meeting at a corner
      } else {
        s = std::clamp((b * f - c * e) / den, LD(0), LD(1));
      }
      t = (b * s + f) / e;
      if (t < 0) {
        t = 0;
        s = std::clamp(-c / a, LD(0), LD(1));
      } else if (t > 1) {
        t = 1;
        s = std::clamp((b - c) / a, LD(0), LD(1));
      }
    }
  }
  x1 = LD(p1.x) + d1x * s, y1 = LD(p1.y) + d1y * s;
  x2 = LD(p2.x) + d2x * t, y2 = LD(p2.y) + d2y * t;
  return std::hypot(x1 - x2, y1 - y2);
}

template <class F>
void each_edge(const Shape& s, F&& f) {
  if (s.pts.size() == 1) {
    f(s.pts[0], s.pts[0]);
    return;
  }
  for (std::size_t i = 0; i + 1 < s.pts.size(); ++i) f(s.pts[i], s.pts[i + 1]);
  if (s.closed && s.pts.size() > 2) f(s.pts.back(), s.pts.front());
}

// Closest points of the copper of two shape lists; false when they touch or overlap.
bool closest_copper(const std::vector<Shape>& A, const std::vector<Shape>& B, Point& pa, Point& pb, Coord& gap) {
  LD best = 1e30L, bx1 = 0, by1 = 0, bx2 = 0, by2 = 0, ra = 0, rb = 0;
  bool best_parallel = false;
  for (const auto& a : A)
    for (const auto& b : B) {
      if ((a.closed && geom::point_in_polygon(b.pts[0], a.pts)) || (b.closed && geom::point_in_polygon(a.pts[0], b.pts))) return false;
      each_edge(a, [&](Point p, Point q) {
        each_edge(b, [&](Point u, Point v) {
          LD x1, y1, x2, y2;
          bool par = false;
          const LD d = closest_seg_seg(p, q, u, v, x1, y1, x2, y2, &par) - LD(a.r) - LD(b.r);
          // Between two facing edges every cut is as short; the middle one (from the parallel pair) beats a tie at a corner.
          if (d < best - 1 || (d <= best + 1 && par && !best_parallel))
            best = std::min(best, d), bx1 = x1, by1 = y1, bx2 = x2, by2 = y2, ra = LD(a.r), rb = LD(b.r), best_parallel = par;
        });
      });
    }
  if (best <= 0) return false;
  const LD dx = bx2 - bx1, dy = by2 - by1, l = std::hypot(dx, dy);
  if (l <= 0) return false;
  pa = {static_cast<Coord>(std::llround(bx1 + dx / l * ra)), static_cast<Coord>(std::llround(by1 + dy / l * ra))};
  pb = {static_cast<Coord>(std::llround(bx2 - dx / l * rb)), static_cast<Coord>(std::llround(by2 - dy / l * rb))};
  gap = static_cast<Coord>(std::llround(best));
  return true;
}

// Distance from a point to the copper of a shape list (negative inside).
LD point_copper_gap(Point p, const std::vector<Shape>& S) {
  LD best = 1e30L;
  for (const auto& s : S) {
    if (s.closed && geom::point_in_polygon(p, s.pts)) return -LD(s.r) - 1;
    each_edge(s, [&](Point u, Point v) { best = std::min(best, geom::point_seg_dist(p, u, v) - LD(s.r)); });
  }
  return best;
}

// Does the segment p-q pass through the copper of `S` (deeper than 1 µm)?
bool segment_hits(Point p, Point q, const std::vector<Shape>& S) {
  for (const auto& s : S) {
    if (s.closed && (geom::point_in_polygon(p, s.pts) || geom::point_in_polygon(q, s.pts))) return true;
    bool hit = false;
    each_edge(s, [&](Point u, Point v) {
      if (!hit && geom::seg_seg_dist(p, q, u, v) < LD(s.r) - 1000) hit = true;
    });
    if (hit) return true;
  }
  return false;
}

int side(geom::i128 o) { return o >= 0 ? 1 : -1; }  // collinear counts as left (a symbolic perturbation)

// Proper crossing of segments p-q and a-b with the perturbation above.
bool crosses(Point p, Point q, Point a, Point b) {
  return side(geom::orient(p, q, a)) != side(geom::orient(p, q, b)) && side(geom::orient(a, b, p)) != side(geom::orient(a, b, q));
}

// A uniform grid of ids by bounding box.
struct Buckets {
  Coord cell = 1'000'000;
  std::unordered_map<std::int64_t, std::vector<int>> m;
  mutable std::vector<int> stamp;
  mutable int now = 0;
  static std::int64_t key(std::int64_t x, std::int64_t y) { return (x << 32) ^ (y & 0xffffffff); }
  void add(const geom::Box& b, int id) {
    for (Coord x = b.x0 / cell - (b.x0 < 0); x <= b.x1 / cell; ++x)
      for (Coord y = b.y0 / cell - (b.y0 < 0); y <= b.y1 / cell; ++y) m[key(x, y)].push_back(id);
    if (static_cast<int>(stamp.size()) <= id) stamp.resize(static_cast<std::size_t>(id) + 1, 0);
  }
  template <class F>
  void query(const geom::Box& b, F&& f) const {
    ++now;
    for (Coord x = b.x0 / cell - (b.x0 < 0); x <= b.x1 / cell; ++x)
      for (Coord y = b.y0 / cell - (b.y0 < 0); y <= b.y1 / cell; ++y) {
        auto it = m.find(key(x, y));
        if (it == m.end()) continue;
        for (int id : it->second)
          if (stamp[static_cast<std::size_t>(id)] != now) {
            stamp[static_cast<std::size_t>(id)] = now;
            f(id);
          }
      }
  }
};

geom::Box seg_box(Point a, Point b) {
  return {std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
}

struct Feat {
  const drc::CopperItem* item;
  CutFeature f;
};

struct Chain {
  model::NetId net;
  std::vector<Point> pts;
  std::vector<const drc::CopperItem*> edge_item;  // the track or arc each edge belongs to
};

struct Crossing {
  int chain;
  int k;   // edge of the chain
  LD pos;  // edge index + fraction along the chain
  LD u;    // fraction along the cut
  int dir;
  Point at;
};

// Splits each net's tracks and arcs on a layer into simple paths.
std::vector<Chain> chains_on(const drc::CopperModel& cm, int layer) {
  std::map<model::NetId, std::vector<const drc::CopperItem*>> by_net;
  for (const auto& it : cm.items)
    if ((it.kind == drc::ItemKind::Track || it.kind == drc::ItemKind::Arc) && !it.removed && (it.layers & model::layer_bit(layer)) &&
        !it.shapes.empty())
      by_net[it.net].push_back(&it);
  std::vector<Chain> out;
  for (const auto& [net, items] : by_net) {
    // Edges: consecutive points of each item's centreline.
    struct E {
      Point a, b;
      const drc::CopperItem* it;
    };
    std::vector<E> edges;
    for (const auto* it : items) {
      const auto& p = it->shapes[0].pts;
      for (std::size_t i = 0; i + 1 < p.size(); ++i)
        if (!(p[i] == p[i + 1])) edges.push_back({p[i], p[i + 1], it});
    }
    std::map<std::pair<Coord, Coord>, std::vector<std::pair<int, bool>>> adj;  // point -> (edge, at its end b)
    for (int i = 0; i < static_cast<int>(edges.size()); ++i) {
      adj[{edges[static_cast<std::size_t>(i)].a.x, edges[static_cast<std::size_t>(i)].a.y}].push_back({i, false});
      adj[{edges[static_cast<std::size_t>(i)].b.x, edges[static_cast<std::size_t>(i)].b.y}].push_back({i, true});
    }
    std::vector<char> used(edges.size(), 0);
    auto walk = [&](std::pair<Coord, Coord> start) {
      for (const auto& [e0, at_b0] : adj[start]) {
        if (used[static_cast<std::size_t>(e0)]) continue;
        Chain c{net, {Point{start.first, start.second}}, {}};
        int e = e0;
        bool at_b = at_b0;
        while (true) {
          used[static_cast<std::size_t>(e)] = 1;
          const auto& ed = edges[static_cast<std::size_t>(e)];
          const Point nxt = at_b ? ed.a : ed.b;
          c.pts.push_back(nxt);
          c.edge_item.push_back(ed.it);
          const auto& nb = adj[{nxt.x, nxt.y}];
          if (nb.size() != 2) break;
          const auto& o = nb[0].first == e ? nb[1] : nb[0];
          if (used[static_cast<std::size_t>(o.first)]) break;
          e = o.first;
          at_b = o.second;
        }
        out.push_back(std::move(c));
      }
    };
    for (const auto& [p, nb] : adj)
      if (nb.size() != 2) walk(p);
    for (const auto& [p, nb] : adj) walk(p);  // closed loops
  }
  return out;
}

}  // namespace

CutReport cut_test(const model::Board& board, const model::DesignRules& rules, const CutTestOptions& opt) {
  CutReport rep;
  const drc::CopperModel cm = drc::build_copper(board);
  const drc::RuleEngine re(board, rules);
  struct ViaPad {
    int via;
    const drc::CopperItem* pad;
    int layer;
    Coord need;
  };
  std::vector<ViaPad> via_pad;                             // pad-via cuts, for the via room test
  std::vector<std::vector<std::pair<Point, Point>>> walls(static_cast<std::size_t>(board.copper_count()));  // pad-pad cuts
  std::vector<char> via_failed(board.vias.size(), 0);

  for (int L = 0; L < board.copper_count(); ++L) {
    std::vector<Feat> feats;
    for (const auto& it : cm.items) {
      if (it.removed || it.bare_of >= 0 || it.shapes.empty() || !(it.layers & model::layer_bit(L))) continue;
      if (it.kind == drc::ItemKind::Pad)
        feats.push_back({&it, {false, it.index, it.net, board.pads[static_cast<std::size_t>(it.index)].pos}});
      else if (it.kind == drc::ItemKind::Via)
        feats.push_back({&it, {true, it.index, it.net, board.vias[static_cast<std::size_t>(it.index)].pos}});
    }
    const auto chains = chains_on(cm, L);
    Buckets fb, wb;
    for (int i = 0; i < static_cast<int>(feats.size()); ++i) fb.add(feats[static_cast<std::size_t>(i)].item->box, i);
    struct WEdge {
      int chain, k;
    };
    std::vector<WEdge> wedges;
    for (int c = 0; c < static_cast<int>(chains.size()); ++c)
      for (int k = 0; k + 1 < static_cast<int>(chains[static_cast<std::size_t>(c)].pts.size()); ++k) {
        wb.add(seg_box(chains[static_cast<std::size_t>(c)].pts[static_cast<std::size_t>(k)], chains[static_cast<std::size_t>(c)].pts[static_cast<std::size_t>(k) + 1]),
               static_cast<int>(wedges.size()));
        wedges.push_back({c, k});
      }

    // Does the loop (crossing x1, the chain between, crossing x2, back along the cut) enclose a feature?
    auto empty_loop = [&](const Crossing& x1, const Crossing& x2) {
      const auto& ch = chains[static_cast<std::size_t>(x1.chain)];
      std::vector<Point> poly{x1.at};
      for (int k = x1.k + 1; k <= x2.k; ++k) poly.push_back(ch.pts[static_cast<std::size_t>(k)]);
      poly.push_back(x2.at);
      geom::Box bx{poly[0].x, poly[0].y, poly[0].x, poly[0].y};
      for (const auto& p : poly) bx.add(p);
      bool empty = true;
      fb.query(bx, [&](int i) {
        if (empty && geom::point_in_polygon(feats[static_cast<std::size_t>(i)].f.pos, poly)) empty = false;
      });
      return empty;
    };

    struct Entry {
      model::NetId net;
      Coord width;
      const drc::CopperItem* item;
    };
    // The cut a-b: its wires, gap and need. Returns false when there is nothing to check.
    auto evaluate = [&](const Feat& a, const Feat& b, Point& pa, Point& pb, Coord& gap, Coord& need, double& s, std::vector<model::NetId>& nets,
                        std::vector<int>& tracks) {
      if (!closest_copper(a.item->shapes, b.item->shapes, pa, pb, gap)) return false;
      if (gap > opt.max_gap) return false;
      geom::Box cb = seg_box(pa, pb);
      bool blocked = false;
      fb.query(cb, [&](int i) {
        const Feat& f = feats[static_cast<std::size_t>(i)];
        if (!blocked && &f != &a && &f != &b && segment_hits(pa, pb, f.item->shapes)) blocked = true;
      });
      if (blocked) return false;  // not visible: the cuts to the feature in between cover it
      std::map<int, std::vector<Crossing>> per;
      const LD ex = LD(pb.x - pa.x), ey = LD(pb.y - pa.y), l2 = ex * ex + ey * ey;
      wb.query(cb, [&](int w) {
        const auto& we = wedges[static_cast<std::size_t>(w)];
        const auto& ch = chains[static_cast<std::size_t>(we.chain)];
        const Point A = ch.pts[static_cast<std::size_t>(we.k)], B = ch.pts[static_cast<std::size_t>(we.k) + 1];
        if (!crosses(pa, pb, A, B)) return;
        const LD oa = LD(geom::orient(pa, pb, A)), ob = LD(geom::orient(pa, pb, B));
        const LD t = oa == ob ? 0 : oa / (oa - ob);
        const LD x = LD(A.x) + t * LD(B.x - A.x), y = LD(A.y) + t * LD(B.y - A.y);
        const LD u = ((x - LD(pa.x)) * ex + (y - LD(pa.y)) * ey) / l2;
        per[we.chain].push_back({we.chain, we.k, LD(we.k) + t, u, side(geom::orient(pa, pb, B)),
                                 {static_cast<Coord>(std::llround(x)), static_cast<Coord>(std::llround(y))}});
      });
      std::vector<std::pair<LD, Entry>> items;
      for (auto& [c, xs] : per) {
        std::sort(xs.begin(), xs.end(), [](const Crossing& p, const Crossing& q) { return p.pos < q.pos; });
        std::vector<Crossing> st;
        for (const auto& x : xs) {
          if (!st.empty() && st.back().dir == -x.dir && empty_loop(st.back(), x)) {
            st.pop_back();
            rep.crossings_cancelled += 2;
          } else {
            st.push_back(x);
          }
        }
        const auto& ch = chains[static_cast<std::size_t>(c)];
        for (const auto& x : st) {
          const auto* it = ch.edge_item[static_cast<std::size_t>(x.k)];
          items.push_back({x.u, {ch.net, it->width, it}});
        }
      }
      std::sort(items.begin(), items.end(), [](const auto& p, const auto& q) { return p.first < q.first; });
      std::vector<Entry> seq{{a.f.net, 0, a.item}};
      for (const auto& [u, e] : items) seq.push_back(e);
      seq.push_back({b.f.net, 0, b.item});
      nets.clear();
      tracks.clear();
      for (const auto& [u, e] : items) {
        nets.push_back(e.net);
        tracks.push_back(e.item->kind == drc::ItemKind::Track ? e.item->index : -1);
      }
      // Copper of one net may overlap: wires of an end's own net beside it, and runs of one net, take no clearance between them.
      auto same = [](const Entry& p, const Entry& q) { return p.net == q.net && p.net != 0; };
      while (seq.size() > 2 && same(seq[1], seq[0])) seq.erase(seq.begin() + 1);
      while (seq.size() > 2 && same(seq[seq.size() - 2], seq.back())) seq.erase(seq.end() - 2);
      std::vector<Entry> runs;
      for (const auto& e : seq) {
        if (!runs.empty() && same(runs.back(), e)) runs.back().width = std::max(runs.back().width, e.width);
        else runs.push_back(e);
      }
      if (runs.size() < 2) return false;
      Coord widths = 0, clear = 0;
      for (const auto& r : runs) widths += r.width;
      for (std::size_t i = 0; i + 1 < runs.size(); ++i) clear += re.clearance(*runs[i].item, *runs[i + 1].item, L);
      need = widths + clear;
      s = clear > 0 ? static_cast<double>(gap - widths) / static_cast<double>(clear) : 1e9;
      return true;
    };

    for (int i = 0; i < static_cast<int>(feats.size()); ++i) {
      const Feat& a = feats[static_cast<std::size_t>(i)];
      std::vector<int> near;  // collected first: evaluate() queries the same buckets
      fb.query(a.item->box.inflated(opt.max_gap), [&](int j) {
        if (j > i) near.push_back(j);
      });
      std::sort(near.begin(), near.end());
      for (int j : near) {
        const Feat& b = feats[static_cast<std::size_t>(j)];
        Point pa, pb;
        Coord gap = 0, need = 0;
        double s = 0;
        std::vector<model::NetId> nets;
        std::vector<int> tracks;
        if (!evaluate(a, b, pa, pb, gap, need, s, nets, tracks)) continue;
        ++rep.cuts;
        if (a.f.via || b.f.via || !nets.empty()) rep.s_star = std::min(rep.s_star, s);  // two pads with nothing between: the board's own spacing
        if (!a.f.via && !b.f.via && gap < 1'000'000) walls[static_cast<std::size_t>(L)].push_back({pa, pb});
        if (a.f.via != b.f.via) {
          const Feat& v = a.f.via ? a : b;
          const Feat& p = a.f.via ? b : a;
          via_pad.push_back({v.f.index, p.item, L, need});
        }
        if (need - gap > opt.tolerance) {
          rep.overfull.push_back({L, a.f, b.f, pa, pb, gap, need, nets, tracks});
          if (a.f.via) via_failed[static_cast<std::size_t>(a.f.index)] = 1;
          if (b.f.via) via_failed[static_cast<std::size_t>(b.f.index)] = 1;
        }
      }
    }
  }

  // Via room: a via that fails a cut where it stands may still fit elsewhere in its cell. If no point of the cell keeps clear of
  // the pads with the wires the topology puts between them, it cannot (pads do not move; the wires' crossings do not change while
  // the via stays inside the walls).
  if (opt.via_room) {
    for (int vi = 0; vi < static_cast<int>(board.vias.size()); ++vi) {
      if (!via_failed[static_cast<std::size_t>(vi)]) continue;
      const auto& v = board.vias[static_cast<std::size_t>(vi)];
      const Coord rv = v.size / 2;
      std::vector<const ViaPad*> cons;
      for (const auto& c : via_pad)
        if (c.via == vi) cons.push_back(&c);
      std::vector<std::pair<Point, Point>> vw;
      for (int L = v.layer_top; L <= v.layer_bottom; ++L)
        for (const auto& w : walls[static_cast<std::size_t>(L)])
          if (std::abs(w.first.x - v.pos.x) < 2 * opt.via_search && std::abs(w.first.y - v.pos.y) < 2 * opt.via_search) vw.push_back(w);
      bool room = false;
      for (Coord dx = -opt.via_search; dx <= opt.via_search && !room; dx += opt.via_step)
        for (Coord dy = -opt.via_search; dy <= opt.via_search && !room; dy += opt.via_step) {
          const Point x{v.pos.x + dx, v.pos.y + dy};
          bool ok = true;
          for (const auto& w : vw)
            if (crosses(v.pos, x, w.first, w.second)) {
              ok = false;
              break;
            }
          for (std::size_t k = 0; ok && k < cons.size(); ++k)
            if (point_copper_gap(x, cons[k]->pad->shapes) - LD(rv) < LD(cons[k]->need - opt.tolerance)) ok = false;
          room = ok;
        }
      if (!room) rep.vias_without_room.push_back(vi);
    }
  }
  return rep;
}

}  // namespace tmk::route

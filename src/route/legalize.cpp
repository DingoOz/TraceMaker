// SPDX-License-Identifier: GPL-3.0-or-later
#include "route/legalize.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <tuple>

namespace tmk::route {

namespace {

constexpr double kMm = 1e-6;  // one nanometre in millimetres

struct PadI {
  double cx, cy, hx, hy;  // centre and half sizes (a circle: hx = hy = radius)
  bool circle;
  model::LayerMask layers;
  model::NetId net;
  double clearance;  // local override, < 0: none
};

struct SegI {
  int a, b;
  double w;
  int layer;
  model::NetId net;
};

struct ViaI {
  int node;
  double r, drill;
  int top, bottom;
  model::NetId net;
};

enum class Kind : std::uint8_t { PointSeg, PointPoint, PointPad, CornerSeg };

struct Con {
  Kind kind;
  int a = -1, b = -1, c = -1;  // PointSeg: node, seg end nodes. PointPoint: nodes a, b. PointPad: node a, pad c. CornerSeg: seg end nodes b, c
  double qx = 0, qy = 0;       // CornerSeg: the fixed corner
  double req = 0;              // required distance, centre of the node to the other feature's boundary (margin included)
};

double seg_param(double px, double py, double ax, double ay, double bx, double by) {
  const double dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy;
  if (l == 0) return 0;
  return std::clamp(((px - ax) * dx + (py - ay) * dy) / l, 0.0, 1.0);
}

}  // namespace

LegalizeResult legalize_routes(const model::Board& board, const model::DesignRules& rules, std::vector<ConnRoute>& routes,
                               const LegalizeOptions& opt_in) {
  LegalizeResult res;
  LegalizeOptions opt = opt_in;
  if (const char* e = std::getenv("TM_LEGALIZE_SWEEPS")) opt.sweeps = std::atoi(e);  // experiments
  if (const char* e = std::getenv("TM_LEGALIZE_BOX")) opt.box_mm = std::atof(e);
  // --- clearance of a pair of nets ------------------------------------------------------------------------------------
  std::vector<double> net_clr(board.nets.size(), 0);
  for (std::size_t n = 0; n < board.nets.size(); ++n)
    net_clr[n] = nm_to_mm(std::max(rules.class_for(board.nets[n].name).clearance, rules.minimums.clearance));
  const double min_clr = nm_to_mm(rules.minimums.clearance);
  auto clearance = [&](model::NetId a, model::NetId b) {
    const double ca = a >= 0 && static_cast<std::size_t>(a) < net_clr.size() ? net_clr[static_cast<std::size_t>(a)] : min_clr;
    const double cb = b >= 0 && static_cast<std::size_t>(b) < net_clr.size() ? net_clr[static_cast<std::size_t>(b)] : min_clr;
    return std::max({ca, cb, min_clr});
  };
  const double margin = opt.margin_mm;
  const double hole_gap = nm_to_mm(rules.minimums.hole_to_hole);

  // --- items ---------------------------------------------------------------------------------------------------------
  std::vector<PadI> pads;
  for (const auto& p : board.pads) {
    if (p.copper == 0) continue;
    PadI pi{};
    pi.cx = nm_to_mm(p.pos.x);
    pi.cy = nm_to_mm(p.pos.y);
    const double sx = nm_to_mm(p.size_x), sy = nm_to_mm(p.size_y);
    const double ang = p.angle * 3.14159265358979323846 / 180.0;
    const double ca = std::fabs(std::cos(ang)), sa = std::fabs(std::sin(ang));
    pi.circle = p.shape == model::PadShape::Circle && std::fabs(sx - sy) < 1e-9;
    pi.hx = pi.circle ? sx / 2 : (ca * sx + sa * sy) / 2;  // bounding box of the rotated pad: never smaller than the pad
    pi.hy = pi.circle ? sx / 2 : (sa * sx + ca * sy) / 2;
    pi.layers = p.copper;
    pi.net = p.net;
    pi.clearance = p.clearance >= 0 ? nm_to_mm(p.clearance) : -1;
    pads.push_back(pi);
  }

  // Nodes: tracks and vias of the routes, merged by net and position. Foreign copper of the board (existing tracks and vias)
  // is added with fixed nodes so that the routes keep their distance from it.
  std::map<std::tuple<model::NetId, Coord, Coord>, int> node_of;
  std::vector<double> px, py;
  std::vector<model::NetId> node_net;
  std::vector<std::uint8_t> fixed;
  auto node = [&](model::NetId net, geom::Point p, bool is_fixed) {
    const auto key = std::make_tuple(net, p.x, p.y);
    const auto it = node_of.find(key);
    if (it != node_of.end()) {
      if (is_fixed) fixed[static_cast<std::size_t>(it->second)] = 1;
      return it->second;
    }
    const int id = static_cast<int>(px.size());
    node_of.emplace(key, id);
    px.push_back(nm_to_mm(p.x));
    py.push_back(nm_to_mm(p.y));
    node_net.push_back(net);
    fixed.push_back(is_fixed ? 1 : 0);
    return id;
  };
  std::vector<SegI> segs;
  std::vector<ViaI> vias;
  for (const auto& cr : routes) {
    for (const auto& t : cr.tracks) {
      if (t.a == t.b) continue;
      segs.push_back({node(t.net, t.a, t.locked), node(t.net, t.b, t.locked), nm_to_mm(t.width), t.layer, t.net});
    }
    for (const auto& v : cr.vias) vias.push_back({node(v.net, v.pos, v.locked), nm_to_mm(v.size) / 2, nm_to_mm(v.drill), v.layer_top, v.layer_bottom, v.net});
  }
  const std::size_t n_route_nodes = px.size();
  for (const auto& t : board.tracks) {
    if (t.a == t.b) continue;
    segs.push_back({node(t.net, t.a, true), node(t.net, t.b, true), nm_to_mm(t.width), t.layer, t.net});
  }
  for (const auto& v : board.vias) vias.push_back({node(v.net, v.pos, true), nm_to_mm(v.size) / 2, nm_to_mm(v.drill), v.layer_top, v.layer_bottom, v.net});
  const std::size_t n = px.size();
  res.nodes = static_cast<int>(n_route_nodes);
  if (segs.empty() && vias.empty()) return res;

  // Half extent of the copper at a node (for the outline), and the pad a node lies in when it belongs to that pad's net.
  std::vector<double> half(n, 0);
  for (const auto& s : segs) {
    half[static_cast<std::size_t>(s.a)] = std::max(half[static_cast<std::size_t>(s.a)], s.w / 2);
    half[static_cast<std::size_t>(s.b)] = std::max(half[static_cast<std::size_t>(s.b)], s.w / 2);
  }
  for (const auto& v : vias) half[static_cast<std::size_t>(v.node)] = std::max(half[static_cast<std::size_t>(v.node)], v.r);
  std::vector<int> own_pad(n, -1);
  for (std::size_t i = 0; i < n; ++i)
    for (std::size_t k = 0; k < pads.size(); ++k) {
      const auto& p = pads[k];
      if (p.net != node_net[i] || p.net == 0) continue;
      if (std::fabs(px[i] - p.cx) <= p.hx + 1e-3 && std::fabs(py[i] - p.cy) <= p.hy + 1e-3) {
        own_pad[i] = static_cast<int>(k);
        break;
      }
    }
  for (std::size_t i = 0; i < n; ++i)  // a vertex at the centre of its own pad stays; elsewhere in the pad (a via in the pad) it may move inside it
    if (own_pad[i] >= 0 && std::fabs(px[i] - pads[static_cast<std::size_t>(own_pad[i])].cx) < 1e-3 &&
        std::fabs(py[i] - pads[static_cast<std::size_t>(own_pad[i])].cy) < 1e-3)
      fixed[i] = 1;

  // Board outline.
  std::vector<std::pair<double, double>> outline;
  for (const auto& p : opt.outline) outline.emplace_back(nm_to_mm(p.x), nm_to_mm(p.y));
  const double edge = opt.edge_mm + nm_to_mm(rules.minimums.copper_edge_clearance);
  auto inside_outline = [&](double x, double y, double m) {
    if (outline.size() < 3) return true;
    bool in = false;
    double dmin = 1e30;
    for (std::size_t i = 0, j = outline.size() - 1; i < outline.size(); j = i++) {
      const auto [x1, y1] = outline[j];
      const auto [x2, y2] = outline[i];
      if (((y1 > y) != (y2 > y)) && (x < (x2 - x1) * (y - y1) / (y2 - y1) + x1)) in = !in;
      const double t = seg_param(x, y, x1, y1, x2, y2);
      dmin = std::min(dmin, std::hypot(x - (x1 + t * (x2 - x1)), y - (y1 + t * (y2 - y1))));
    }
    return in && dmin >= m;
  };

  // --- candidate pairs ------------------------------------------------------------------------------------------------
  const double max_clr = [&] {
    double m = min_clr;
    for (double c : net_clr) m = std::max(m, c);
    return m + margin;
  }();
  const double reach = max_clr + 2 * opt.box_mm * 1.5 + 0.05;  // pairs that can come into contact while vertices move within their boxes
  constexpr double kCell = 0.5;
  struct Item {
    char type;  // 's' segment, 'v' via, 'p' pad
    int idx;
  };
  std::map<std::pair<int, int>, std::vector<Item>> grid;
  auto put = [&](Item it, double x0, double y0, double x1, double y1) {
    for (int gx = static_cast<int>(std::floor(x0 / kCell)); gx <= static_cast<int>(std::floor(x1 / kCell)); ++gx)
      for (int gy = static_cast<int>(std::floor(y0 / kCell)); gy <= static_cast<int>(std::floor(y1 / kCell)); ++gy) grid[{gx, gy}].push_back(it);
  };
  for (std::size_t i = 0; i < segs.size(); ++i) {
    const auto& s = segs[i];
    const double r = s.w / 2 + reach / 2;
    put({'s', static_cast<int>(i)}, std::min(px[static_cast<std::size_t>(s.a)], px[static_cast<std::size_t>(s.b)]) - r,
        std::min(py[static_cast<std::size_t>(s.a)], py[static_cast<std::size_t>(s.b)]) - r, std::max(px[static_cast<std::size_t>(s.a)], px[static_cast<std::size_t>(s.b)]) + r,
        std::max(py[static_cast<std::size_t>(s.a)], py[static_cast<std::size_t>(s.b)]) + r);
  }
  for (std::size_t i = 0; i < vias.size(); ++i) {
    const double r = vias[i].r + reach / 2, x = px[static_cast<std::size_t>(vias[i].node)], y = py[static_cast<std::size_t>(vias[i].node)];
    put({'v', static_cast<int>(i)}, x - r, y - r, x + r, y + r);
  }
  for (std::size_t i = 0; i < pads.size(); ++i)
    put({'p', static_cast<int>(i)}, pads[i].cx - pads[i].hx - reach / 2, pads[i].cy - pads[i].hy - reach / 2, pads[i].cx + pads[i].hx + reach / 2,
        pads[i].cy + pads[i].hy + reach / 2);

  std::vector<Con> cons;
  std::set<std::tuple<char, int, char, int>> seen;
  auto add_seg_point = [&](int point_node, const SegI& s, double req) { cons.push_back({Kind::PointSeg, point_node, s.a, s.b, 0, 0, req}); };
  auto layer_in = [](int layer, int top, int bottom) { return layer >= top && layer <= bottom; };
  for (const auto& [cell, items] : grid) {
    (void)cell;
    for (std::size_t x = 0; x < items.size(); ++x)
      for (std::size_t y = x + 1; y < items.size(); ++y) {
        Item p = items[x], q = items[y];
        if (p.type > q.type) std::swap(p, q);  // order 'p' < 's' < 'v'
        if (!seen.insert({p.type, p.idx, q.type, q.idx}).second) continue;
        if (p.type == 's' && q.type == 's') {
          const auto& s = segs[static_cast<std::size_t>(p.idx)];
          const auto& t = segs[static_cast<std::size_t>(q.idx)];
          if (s.net == t.net || s.layer != t.layer) continue;
          if (fixed[static_cast<std::size_t>(s.a)] && fixed[static_cast<std::size_t>(s.b)] && fixed[static_cast<std::size_t>(t.a)] && fixed[static_cast<std::size_t>(t.b)]) continue;
          const double req = clearance(s.net, t.net) + margin + s.w / 2 + t.w / 2;
          add_seg_point(s.a, t, req);
          add_seg_point(s.b, t, req);
          add_seg_point(t.a, s, req);
          add_seg_point(t.b, s, req);
        } else if (p.type == 's' && q.type == 'v') {
          const auto& s = segs[static_cast<std::size_t>(p.idx)];
          const auto& v = vias[static_cast<std::size_t>(q.idx)];
          if (s.net == v.net || !layer_in(s.layer, v.top, v.bottom)) continue;
          add_seg_point(v.node, s, clearance(s.net, v.net) + margin + s.w / 2 + v.r);
        } else if (p.type == 'v' && q.type == 'v') {
          const auto& v = vias[static_cast<std::size_t>(p.idx)];
          const auto& w = vias[static_cast<std::size_t>(q.idx)];
          if (v.net == w.net) continue;
          const double req = std::max(clearance(v.net, w.net) + v.r + w.r, v.drill / 2 + w.drill / 2 + hole_gap) + margin;
          cons.push_back({Kind::PointPoint, v.node, w.node, -1, 0, 0, req});
        } else if (p.type == 'p' && q.type == 's') {
          const auto& pd = pads[static_cast<std::size_t>(p.idx)];
          const auto& s = segs[static_cast<std::size_t>(q.idx)];
          if (pd.net == s.net || !(pd.layers & model::layer_bit(s.layer))) continue;
          const double req = std::max(clearance(s.net, pd.net), pd.clearance) + margin + s.w / 2;
          cons.push_back({Kind::PointPad, s.a, -1, p.idx, 0, 0, req});
          cons.push_back({Kind::PointPad, s.b, -1, p.idx, 0, 0, req});
          if (pd.circle) {
            cons.push_back({Kind::CornerSeg, -1, s.a, s.b, pd.cx, pd.cy, req + pd.hx});
          } else {
            for (const auto [sx, sy] : {std::pair{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}})
              cons.push_back({Kind::CornerSeg, -1, s.a, s.b, pd.cx + sx * pd.hx, pd.cy + sy * pd.hy, req});
          }
        } else if (p.type == 'p' && q.type == 'v') {
          const auto& pd = pads[static_cast<std::size_t>(p.idx)];
          const auto& v = vias[static_cast<std::size_t>(q.idx)];
          if (pd.net == v.net) continue;
          bool shared = false;
          for (int l = v.top; l <= v.bottom && !shared; ++l) shared = (pd.layers & model::layer_bit(l)) != 0;
          if (!shared) continue;
          cons.push_back({Kind::PointPad, v.node, -1, p.idx, 0, 0, std::max(clearance(v.net, pd.net), pd.clearance) + margin + v.r});
        }
      }
  }
  // A constraint between two fixed things does nothing; drop those with no movable node.
  cons.erase(std::remove_if(cons.begin(), cons.end(),
                            [&](const Con& c) {
                              auto fx = [&](int i) { return i < 0 || fixed[static_cast<std::size_t>(i)] != 0; };
                              switch (c.kind) {
                                case Kind::PointSeg: return fx(c.a) && fx(c.b) && fx(c.c);
                                case Kind::PointPoint: return fx(c.a) && fx(c.b);
                                case Kind::PointPad: return fx(c.a);
                                case Kind::CornerSeg: return fx(c.b) && fx(c.c);
                              }
                              return true;
                            }),
              cons.end());

  if (opt.seed != 0 && cons.size() > 1) {  // another visiting order: rotate and, for odd seeds, reverse
    std::rotate(cons.begin(), cons.begin() + static_cast<std::ptrdiff_t>((opt.seed * 2654435761ULL) % cons.size()), cons.end());
    if (opt.seed & 1) std::reverse(cons.begin(), cons.end());
  }

  // --- bounds ---------------------------------------------------------------------------------------------------------
  std::vector<double> x0 = px, y0 = py, lox(n), hix(n), loy(n), hiy(n);
  for (std::size_t i = 0; i < n; ++i) {
    lox[i] = px[i] - opt.box_mm;
    hix[i] = px[i] + opt.box_mm;
    loy[i] = py[i] - opt.box_mm;
    hiy[i] = py[i] + opt.box_mm;
    if (own_pad[i] >= 0 && !fixed[i]) {  // stays inside its own pad
      const auto& p = pads[static_cast<std::size_t>(own_pad[i])];
      lox[i] = std::max(lox[i], std::min(p.cx - p.hx + 0.01, px[i]));
      hix[i] = std::min(hix[i], std::max(p.cx + p.hx - 0.01, px[i]));
      loy[i] = std::max(loy[i], std::min(p.cy - p.hy + 0.01, py[i]));
      hiy[i] = std::min(hiy[i], std::max(p.cy + p.hy - 0.01, py[i]));
    }
    if (fixed[i]) lox[i] = hix[i] = px[i], loy[i] = hiy[i] = py[i];
  }

  // --- evaluation and projection --------------------------------------------------------------------------------------
  // Distance of a constraint's node to its feature and the unit vector away from it; false when degenerate.
  struct Geo {
    double d, nx, ny, t;
  };
  auto eval = [&](const Con& c, Geo& g) {
    switch (c.kind) {
      case Kind::PointSeg: {
        const auto a = static_cast<std::size_t>(c.b), b = static_cast<std::size_t>(c.c), p = static_cast<std::size_t>(c.a);
        const double t = seg_param(px[p], py[p], px[a], py[a], px[b], py[b]);
        const double fx = px[a] + t * (px[b] - px[a]), fy = py[a] + t * (py[b] - py[a]);
        const double ex = px[p] - fx, ey = py[p] - fy, d = std::hypot(ex, ey);
        if (d < 1e-9) return false;
        g = {d, ex / d, ey / d, t};
        return true;
      }
      case Kind::PointPoint: {
        const auto a = static_cast<std::size_t>(c.a), b = static_cast<std::size_t>(c.b);
        const double ex = px[a] - px[b], ey = py[a] - py[b], d = std::hypot(ex, ey);
        if (d < 1e-9) return false;
        g = {d, ex / d, ey / d, 0};
        return true;
      }
      case Kind::PointPad: {
        const auto p = static_cast<std::size_t>(c.a);
        const auto& pd = pads[static_cast<std::size_t>(c.c)];
        double ex, ey, d;
        if (pd.circle) {
          ex = px[p] - pd.cx;
          ey = py[p] - pd.cy;
          d = std::hypot(ex, ey);
          if (d < 1e-9) return false;
          g = {d - pd.hx, ex / d, ey / d, 0};
          return true;
        }
        const double qx = std::clamp(px[p], pd.cx - pd.hx, pd.cx + pd.hx), qy = std::clamp(py[p], pd.cy - pd.hy, pd.cy + pd.hy);
        ex = px[p] - qx;
        ey = py[p] - qy;
        d = std::hypot(ex, ey);
        if (d < 1e-9) return false;
        g = {d, ex / d, ey / d, 0};
        return true;
      }
      case Kind::CornerSeg: {
        const auto a = static_cast<std::size_t>(c.b), b = static_cast<std::size_t>(c.c);
        const double t = seg_param(c.qx, c.qy, px[a], py[a], px[b], py[b]);
        const double fx = px[a] + t * (px[b] - px[a]), fy = py[a] + t * (py[b] - py[a]);
        const double ex = fx - c.qx, ey = fy - c.qy, d = std::hypot(ex, ey);
        if (d < 1e-9) return false;
        g = {d, ex / d, ey / d, t};  // normal points from the corner to the segment
        return true;
      }
    }
    return false;
  };
  auto move_node = [&](int i, double dx, double dy) {
    const auto k = static_cast<std::size_t>(i);
    if (fixed[k]) return;
    const double nx = std::clamp(px[k] + dx, lox[k], hix[k]), ny = std::clamp(py[k] + dy, loy[k], hiy[k]);
    if (!outline.empty() && (nx != px[k] || ny != py[k]) && !inside_outline(nx, ny, half[k] + edge)) return;
    px[k] = nx;
    py[k] = ny;
  };
  auto violated = [&](const Con& c) {
    Geo g{};
    return eval(c, g) && g.d < c.req - margin - 1e-9;
  };
  for (const auto& c : cons) res.pairs_before += violated(c) ? 1 : 0;

  for (int sweep = 0; sweep < opt.sweeps; ++sweep) {
    double worst = 0;
    for (const auto& c : cons) {
      Geo g{};
      if (!eval(c, g) || g.d >= c.req) continue;
      const double miss = c.req - g.d;
      worst = std::max(worst, miss);
      auto weight = [&](int i) { return i >= 0 && !fixed[static_cast<std::size_t>(i)] ? 1.0 : 0.0; };
      switch (c.kind) {
        case Kind::PointSeg: {
          const double wp = weight(c.a), wa = weight(c.b) * (1 - g.t) * (1 - g.t), wb = weight(c.c) * g.t * g.t, den = wp + wa + wb;
          if (den == 0) break;
          const double lam = miss / den;
          if (wp > 0) move_node(c.a, g.nx * lam, g.ny * lam);
          if (wa > 0) move_node(c.b, -g.nx * lam * (1 - g.t), -g.ny * lam * (1 - g.t));
          if (wb > 0) move_node(c.c, -g.nx * lam * g.t, -g.ny * lam * g.t);
          break;
        }
        case Kind::PointPoint: {
          const double wa = weight(c.a), wb = weight(c.b), den = wa + wb;
          if (den == 0) break;
          const double lam = miss / den;
          if (wa > 0) move_node(c.a, g.nx * lam, g.ny * lam);
          if (wb > 0) move_node(c.b, -g.nx * lam, -g.ny * lam);
          break;
        }
        case Kind::PointPad:
          if (weight(c.a) > 0) move_node(c.a, g.nx * miss, g.ny * miss);
          break;
        case Kind::CornerSeg: {
          const double wa = weight(c.b) * (1 - g.t) * (1 - g.t), wb = weight(c.c) * g.t * g.t, den = wa + wb;
          if (den == 0) break;
          const double lam = miss / den;
          if (wa > 0) move_node(c.b, g.nx * lam * (1 - g.t), g.ny * lam * (1 - g.t));
          if (wb > 0) move_node(c.c, g.nx * lam * g.t, g.ny * lam * g.t);
          break;
        }
      }
    }
    res.sweeps_used = sweep + 1;
    if (worst <= margin) break;  // everything within the margin of its target: the next sweeps would only chase it
  }
  for (const auto& c : cons) res.pairs_after += violated(c) ? 1 : 0;
  if (const char* e = std::getenv("TM_DEBUG_LEGALIZE"); e && e[0] == '2')  // list what is left
    for (const auto& c : cons) {
      Geo g{};
      if (!eval(c, g) || g.d >= c.req - margin - 1e-9) continue;
      const int node_i = c.kind == Kind::CornerSeg ? c.b : c.a;
      const auto k = static_cast<std::size_t>(node_i);
      std::fprintf(stderr, "  left: kind %d at (%.3f, %.3f) net %d: %.4f of %.4f mm; fixed:%d%d%d\n", static_cast<int>(c.kind), px[k], py[k], node_net[k],
                   g.d, c.req - margin, c.a >= 0 ? fixed[static_cast<std::size_t>(c.a)] : 9, c.b >= 0 ? fixed[static_cast<std::size_t>(c.b)] : 9,
                   c.c >= 0 && c.kind != Kind::PointPad ? fixed[static_cast<std::size_t>(c.c)] : 9);
    }

  // --- write back -----------------------------------------------------------------------------------------------------
  // Positions are rounded to nanometres; a node is looked up by its original net and position.
  std::vector<geom::Point> new_pos(n);
  for (std::size_t i = 0; i < n; ++i) {
    new_pos[i] = {mm_to_nm(px[i]), mm_to_nm(py[i])};
    const double mv = std::hypot(px[i] - x0[i], py[i] - y0[i]);
    if (mv > 1e-6) ++res.moved;
    res.max_move_mm = std::max(res.max_move_mm, mv);
  }
  res.moved = std::min(res.moved, res.nodes);
  for (auto& cr : routes) {
    for (auto& t : cr.tracks) {
      if (t.a == t.b || t.locked) continue;
      const int a = node_of.at(std::make_tuple(t.net, t.a.x, t.a.y)), b = node_of.at(std::make_tuple(t.net, t.b.x, t.b.y));
      t.a = new_pos[static_cast<std::size_t>(a)];
      t.b = new_pos[static_cast<std::size_t>(b)];
    }
    for (auto& v : cr.vias) {
      if (v.locked) continue;
      v.pos = new_pos[static_cast<std::size_t>(node_of.at(std::make_tuple(v.net, v.pos.x, v.pos.y)))];
    }
  }
  return res;
}

}  // namespace tmk::route

// SPDX-License-Identifier: GPL-3.0-or-later
#include "route/legalize.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <tuple>

namespace tmk::route {

namespace {

struct PadI {
  double cx, cy, hx, hy;  // centre and half sizes of the inner rectangle in the pad's own frame (a circle: hx = hy = radius)
  double rr = 0;          // the pad is that rectangle grown by rr (the corner radius of a rounded rectangle, half the width of an oval)
  double ca = 1, sa = 0;  // the pad's rotation: its frame's x axis is (ca, sa) on the board, its y axis (-sa, ca)
  double bx, by;          // half sizes of its bounding box on the board
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
  if (const char* e = std::getenv("TM_LEGALIZE_REALIZE"); e && opt.realize_iters > 0) opt.realize_iters = std::atoi(e);  // a caller's 0 stays 0
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
  const double via_extra = nm_to_mm(std::max<Coord>(rules.via_clearance_extra, 0));  // a ramp stage that keeps vias at the board's clearance
  const double hole_gap = nm_to_mm(rules.minimums.hole_to_hole);

  // --- items ---------------------------------------------------------------------------------------------------------
  const bool bbox_pads = [] {
    const char* e = std::getenv("TM_LEGALIZE_BBOX_PADS");
    return e && e[0] == '1';
  }();
  std::vector<PadI> pads;
  for (const auto& p : board.pads) {
    if (p.copper == 0) continue;
    PadI pi{};
    const geom::Point centre = p.pos + geom::rotate(p.drill_offset, p.angle);  // the copper's centre (an offset pad's is not its hole's)
    pi.cx = nm_to_mm(centre.x);
    pi.cy = nm_to_mm(centre.y);
    const double sx = nm_to_mm(p.size_x), sy = nm_to_mm(p.size_y);
    const double ang = p.angle * 3.14159265358979323846 / 180.0;
    const double ca = std::fabs(std::cos(ang)), sa = std::fabs(std::sin(ang));
    pi.circle = (p.shape == model::PadShape::Circle || p.shape == model::PadShape::Oval) && std::fabs(sx - sy) < 1e-9;
    pi.bx = pi.circle ? sx / 2 : (ca * sx + sa * sy) / 2;  // bounding box of the rotated pad
    pi.by = pi.circle ? sx / 2 : (sa * sx + ca * sy) / 2;
    // A pad is a rectangle in its own frame, turned by its angle and grown by its corner radius, so a pad at 45 or 60 degrees, an oval and a rounded
    // rectangle are what they are (D159: as the bounding box of a sharp rectangle, rotated pads were up to 0.2 mm too large and the legaliser pushed
    // tracks that were legal). Chamfered, trapezoid and custom pads stay rectangles. TM_LEGALIZE_BBOX_PADS=1 keeps the bounding box (experiments).
    if (!pi.circle && !bbox_pads) {
      if (p.shape == model::PadShape::Oval) pi.rr = std::min(sx, sy) / 2;
      else if (p.shape == model::PadShape::RoundRect) pi.rr = std::clamp(p.roundrect_ratio, 0.0, 0.5) * std::min(sx, sy);
      pi.hx = sx / 2 - pi.rr;
      pi.hy = sy / 2 - pi.rr;
      pi.ca = std::cos(ang);  // geom::rotate's convention: the pad's x axis is (cos, -sin) on the board
      pi.sa = -std::sin(ang);
    } else {
      pi.hx = pi.bx;
      pi.hy = pi.by;
    }
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
  // Tracks to cut into pieces (opt.subdivide_mm): their end nodes now, the pieces once every node of the board exists.
  struct Cut {
    std::size_t route, track;
    SegI seg;  // the whole track
  };
  std::vector<Cut> cuts;
  for (std::size_t ri = 0; ri < routes.size(); ++ri) {
    const auto& cr = routes[ri];
    for (std::size_t ti = 0; ti < cr.tracks.size(); ++ti) {
      const auto& t = cr.tracks[ti];
      if (t.a == t.b) continue;
      const SegI whole{node(t.net, t.a, t.locked), node(t.net, t.b, t.locked), nm_to_mm(t.width), t.layer, t.net};
      if (opt.subdivide_mm > 0 && !t.locked) cuts.push_back({ri, ti, whole});
      else segs.push_back(whole);
    }
    for (const auto& v : cr.vias) vias.push_back({node(v.net, v.pos, v.locked), nm_to_mm(v.size) / 2, nm_to_mm(v.drill), v.layer_top, v.layer_bottom, v.net});
  }
  const std::size_t n_route_nodes = px.size();
  for (const auto& t : board.tracks) {
    if (t.a == t.b) continue;
    segs.push_back({node(t.net, t.a, true), node(t.net, t.b, true), nm_to_mm(t.width), t.layer, t.net});
  }
  for (const auto& v : board.vias) vias.push_back({node(v.net, v.pos, true), nm_to_mm(v.size) / 2, nm_to_mm(v.drill), v.layer_top, v.layer_bottom, v.net});
  // The pieces of the cut tracks. A vertex of the same net that lies on the track (a junction, a via) stays a vertex of it, so the
  // copper stays joined whichever way the pieces bend; between those the track is cut evenly.
  std::map<std::pair<std::size_t, std::size_t>, std::vector<int>> chain_of;
  std::size_t n_cut_nodes = 0;
  const std::size_t known = px.size();
  if (!cuts.empty()) {
    for (const auto& c : cuts) {
      const double ax = px[static_cast<std::size_t>(c.seg.a)], ay = py[static_cast<std::size_t>(c.seg.a)];
      const double bx = px[static_cast<std::size_t>(c.seg.b)], by = py[static_cast<std::size_t>(c.seg.b)];
      std::vector<std::pair<double, int>> anchors{{0.0, c.seg.a}, {1.0, c.seg.b}};
      for (std::size_t k = 0; k < known; ++k) {
        if (node_net[k] != c.seg.net || static_cast<int>(k) == c.seg.a || static_cast<int>(k) == c.seg.b) continue;
        const double t = seg_param(px[k], py[k], ax, ay, bx, by);
        if (t <= 1e-9 || t >= 1 - 1e-9) continue;
        if (std::hypot(px[k] - (ax + t * (bx - ax)), py[k] - (ay + t * (by - ay))) < 1e-3) anchors.emplace_back(t, static_cast<int>(k));
      }
      std::sort(anchors.begin(), anchors.end());
      std::vector<int> chain;
      const double len = std::hypot(bx - ax, by - ay);
      for (std::size_t k = 0; k + 1 < anchors.size(); ++k) {
        chain.push_back(anchors[k].second);
        const double gap = (anchors[k + 1].first - anchors[k].first) * len;
        const int pieces = std::max(1, static_cast<int>(std::ceil(gap / opt.subdivide_mm)));
        for (int j = 1; j < pieces; ++j) {
          const double t = anchors[k].first + (anchors[k + 1].first - anchors[k].first) * j / pieces;
          const std::size_t before = px.size();
          chain.push_back(node(c.seg.net, {mm_to_nm(ax + t * (bx - ax)), mm_to_nm(ay + t * (by - ay))}, false));
          n_cut_nodes += px.size() - before;
        }
      }
      chain.push_back(c.seg.b);
      for (std::size_t k = 0; k + 1 < chain.size(); ++k) segs.push_back({chain[k], chain[k + 1], c.seg.w, c.seg.layer, c.seg.net});
      chain_of[{c.route, c.track}] = std::move(chain);
    }
  }
  const std::size_t n = px.size();
  std::vector<std::uint8_t> is_anchor(n, 0);  // the vertices the copper had before the cutting
  std::fill(is_anchor.begin(), is_anchor.begin() + static_cast<std::ptrdiff_t>(known), 1);
  res.nodes = static_cast<int>(n_route_nodes + n_cut_nodes);
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
      if (std::fabs(px[i] - p.cx) <= p.bx + 1e-3 && std::fabs(py[i] - p.cy) <= p.by + 1e-3) {
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
    put({'p', static_cast<int>(i)}, pads[i].cx - pads[i].bx - reach / 2, pads[i].cy - pads[i].by - reach / 2, pads[i].cx + pads[i].bx + reach / 2,
        pads[i].cy + pads[i].by + reach / 2);

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
          add_seg_point(v.node, s, clearance(s.net, v.net) + via_extra + margin + s.w / 2 + v.r);
        } else if (p.type == 'v' && q.type == 'v') {
          const auto& v = vias[static_cast<std::size_t>(p.idx)];
          const auto& w = vias[static_cast<std::size_t>(q.idx)];
          if (v.net == w.net) continue;
          const double req = std::max(clearance(v.net, w.net) + via_extra + v.r + w.r, v.drill / 2 + w.drill / 2 + hole_gap) + margin;
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
            // The closest points of a segment and a convex pad: an end of the segment (PointPad) or a corner of the inner rectangle.
            for (const auto [sx, sy] : {std::pair{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}}) {
              if ((sy > 0 && pd.hy < 1e-9) || (sx > 0 && pd.hx < 1e-9)) continue;  // an oval's inner rectangle is a line: two corners
              const double lx = sx * pd.hx, ly = sy * pd.hy;
              cons.push_back({Kind::CornerSeg, -1, s.a, s.b, pd.cx + pd.ca * lx - pd.sa * ly, pd.cy + pd.sa * lx + pd.ca * ly, req + pd.rr});
            }
          }
        } else if (p.type == 'p' && q.type == 'v') {
          const auto& pd = pads[static_cast<std::size_t>(p.idx)];
          const auto& v = vias[static_cast<std::size_t>(q.idx)];
          if (pd.net == v.net) continue;
          bool shared = false;
          for (int l = v.top; l <= v.bottom && !shared; ++l) shared = (pd.layers & model::layer_bit(l)) != 0;
          if (!shared) continue;
          cons.push_back({Kind::PointPad, v.node, -1, p.idx, 0, 0, std::max(clearance(v.net, pd.net), pd.clearance) + via_extra + margin + v.r});
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
      lox[i] = std::max(lox[i], std::min(p.cx - p.bx + 0.01, px[i]));
      hix[i] = std::min(hix[i], std::max(p.cx + p.bx - 0.01, px[i]));
      loy[i] = std::max(loy[i], std::min(p.cy - p.by + 0.01, py[i]));
      hiy[i] = std::min(hiy[i], std::max(p.cy + p.by - 0.01, py[i]));
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
        // In the pad's frame: the nearest point of the rectangle, and the offset to it turned back onto the board.
        const double dx = px[p] - pd.cx, dy = py[p] - pd.cy;
        const double lx = pd.ca * dx + pd.sa * dy, ly = -pd.sa * dx + pd.ca * dy;
        const double fx = lx - std::clamp(lx, -pd.hx, pd.hx), fy = ly - std::clamp(ly, -pd.hy, pd.hy);
        ex = pd.ca * fx - pd.sa * fy;
        ey = pd.sa * fx + pd.ca * fy;
        d = std::hypot(ex, ey);
        if (d < 1e-9) return false;
        g = {d - pd.rr, ex / d, ey / d, 0};
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
  // --- realizer (doc 20 §2.3) ------------------------------------------------------------------------------------------
  // The sweeps chase one pair at a time. Where a bundle of tracks has to shift together they stall (poncho's last hand-over: 71 pairs,
  // the same after 3000 sweeps). The realizer moves everything at once: sequential quadratic programming. Each round linearises the
  // constraints within kNear of their target and solves the elastic QP  min 1/2 |d|^2 + kM sum s  s.t.  g_i + a_i.d + s_i >= 0, s >= 0,
  // d within the node's box and the trust region rho, by dual coordinate ascent (Hildreth 1957: one multiplier per row in [0, kM], the
  // box kept by clipping). A round is kept if it leaves fewer pairs, or as many less deep; otherwise rho halves. Rows in a fixed order:
  // deterministic.
  if (opt.realize_iters > 0) {
    auto tally = [&] {
      std::pair<int, double> t{0, 0.0};
      for (const auto& c : cons) {
        Geo g{};
        if (eval(c, g) && g.d < c.req - margin - 1e-9) {
          ++t.first;
          t.second = std::max(t.second, c.req - margin - g.d);
        }
      }
      return t;
    };
    auto cur = tally();
    if (cur.first > 0) {
      res.pairs_projected = cur.first;
      constexpr double kNear = 0.05, kM = 1.0, kRhoMin = 1e-4;
      constexpr int kPasses = 200;
      struct Term {
        int node;
        double gx, gy;
      };
      struct Row {
        double g;
        int first, count;
      };
      std::vector<Row> rows;
      std::vector<Term> terms;
      std::vector<double> dx(n, 0), dy(n, 0), dlo_x(n), dhi_x(n), dlo_y(n), dhi_y(n), lam;
      std::vector<std::uint8_t> touched(n, 0);
      std::vector<int> nodes_in;
      double rho = 0.05;
      for (int it = 0; it < opt.realize_iters && cur.first > 0; ++it) {
        rows.clear();
        terms.clear();
        for (const auto& c : cons) {
          Geo g{};
          if (!eval(c, g) || g.d - c.req >= kNear) continue;
          const int first = static_cast<int>(terms.size());
          auto add = [&](int i, double gx, double gy) {
            if (i >= 0 && !fixed[static_cast<std::size_t>(i)] && (gx != 0 || gy != 0)) terms.push_back({i, gx, gy});
          };
          switch (c.kind) {  // gradients of the distance (the segment parameter t held: the envelope theorem)
            case Kind::PointSeg:
              add(c.a, g.nx, g.ny);
              add(c.b, -(1 - g.t) * g.nx, -(1 - g.t) * g.ny);
              add(c.c, -g.t * g.nx, -g.t * g.ny);
              break;
            case Kind::PointPoint:
              add(c.a, g.nx, g.ny);
              add(c.b, -g.nx, -g.ny);
              break;
            case Kind::PointPad:
              add(c.a, g.nx, g.ny);
              break;
            case Kind::CornerSeg:
              add(c.b, (1 - g.t) * g.nx, (1 - g.t) * g.ny);
              add(c.c, g.t * g.nx, g.t * g.ny);
              break;
          }
          const int count = static_cast<int>(terms.size()) - first;
          if (count > 0) rows.push_back({g.d - c.req, first, count});
        }
        if (rows.empty()) break;
        nodes_in.clear();
        for (const auto& t : terms) {
          const auto k = static_cast<std::size_t>(t.node);
          if (touched[k]) continue;
          touched[k] = 1;
          nodes_in.push_back(t.node);
          dx[k] = dy[k] = 0;
          dlo_x[k] = std::max(-rho, lox[k] - px[k]);
          dhi_x[k] = std::min(rho, hix[k] - px[k]);
          dlo_y[k] = std::max(-rho, loy[k] - py[k]);
          dhi_y[k] = std::min(rho, hiy[k] - py[k]);
        }
        lam.assign(rows.size(), 0.0);
        for (int pass = 0; pass < kPasses; ++pass) {
          double change = 0;
          for (std::size_t r = 0; r < rows.size(); ++r) {
            double ad = 0, aa = 0;
            for (int j = rows[r].first; j < rows[r].first + rows[r].count; ++j) {
              const auto& t = terms[static_cast<std::size_t>(j)];
              const auto k = static_cast<std::size_t>(t.node);
              ad += t.gx * dx[k] + t.gy * dy[k];
              aa += t.gx * t.gx + t.gy * t.gy;
            }
            if (aa == 0) continue;
            const double nl = std::clamp(lam[r] + (-rows[r].g - ad) / aa, 0.0, kM), dl = nl - lam[r];
            if (dl == 0) continue;
            lam[r] = nl;
            change = std::max(change, std::fabs(dl));
            for (int j = rows[r].first; j < rows[r].first + rows[r].count; ++j) {
              const auto& t = terms[static_cast<std::size_t>(j)];
              const auto k = static_cast<std::size_t>(t.node);
              dx[k] = std::clamp(dx[k] + dl * t.gx, dlo_x[k], dhi_x[k]);
              dy[k] = std::clamp(dy[k] + dl * t.gy, dlo_y[k], dhi_y[k]);
            }
          }
          if (change < 1e-9) break;
        }
        const std::vector<double> ox = px, oy = py;
        for (int i : nodes_in) {
          const auto k = static_cast<std::size_t>(i);
          touched[k] = 0;
          const double nx = px[k] + dx[k], ny = py[k] + dy[k];
          if (!outline.empty() && !inside_outline(nx, ny, half[k] + edge)) continue;  // a move off the board is not taken
          px[k] = nx;
          py[k] = ny;
        }
        res.realize_iters_used = it + 1;
        const auto next = tally();
        if (next.first < cur.first || (next.first == cur.first && next.second < cur.second)) {
          cur = next;
        } else {
          px = ox;
          py = oy;
          rho *= 0.5;
          if (rho < kRhoMin) break;
        }
      }
    }
  }
  // A vertex in its own pad moved within the pad's bounding box. If its track's end no longer touches the copper of a rotated, oval or
  // rounded pad (the box's corners are not copper), the track would be unconnected, so the vertex goes back to where it was. A vertex
  // that left the copper by less than its half width still connects; resetting those cost routes (D159).
  auto in_pad = [&](const PadI& pd, double x, double y, double grow) {
    const double dx = x - pd.cx, dy = y - pd.cy;
    if (pd.circle) return std::hypot(dx, dy) <= pd.hx + grow + 1e-6;
    const double lx = pd.ca * dx + pd.sa * dy, ly = -pd.sa * dx + pd.ca * dy;
    return std::hypot(lx - std::clamp(lx, -pd.hx, pd.hx), ly - std::clamp(ly, -pd.hy, pd.hy)) <= pd.rr + grow + 1e-6;
  };
  for (std::size_t i = 0; i < n; ++i) {
    if (own_pad[i] < 0 || fixed[i]) continue;
    const auto& pd = pads[static_cast<std::size_t>(own_pad[i])];
    const double touch = std::max(half[i] - 0.001, 0.0);  // the end's disc overlaps the copper by at least 1 µm
    if (in_pad(pd, x0[i], y0[i], 0) && !in_pad(pd, px[i], py[i], touch)) px[i] = x0[i], py[i] = y0[i];
  }
  for (const auto& c : cons) res.pairs_after += violated(c) ? 1 : 0;
  // Experiments (doc 20 §2.3): the problem as the projection leaves it, for a realizer prototype. TM_LEGALIZE_DUMP=DIR writes
  // DIR/legalize-<n>.json for every call that leaves pairs too close: nodes (start, now, bounds, fixed), pads and constraints.
  if (const char* dir = std::getenv("TM_LEGALIZE_DUMP"); dir && res.pairs_after > 0) {
    static std::atomic<int> dumps{0};
    const std::string path = std::string(dir) + "/legalize-" + std::to_string(dumps++) + ".json";
    if (std::FILE* f = std::fopen(path.c_str(), "w")) {
      std::fprintf(f, "{\"margin\": %.9g, \"box\": %.9g, \"nodes\": [", margin, opt.box_mm);
      for (std::size_t i = 0; i < n; ++i)
        std::fprintf(f, "%s[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%d,%.9g]", i ? "," : "", x0[i], y0[i], px[i], py[i], lox[i], hix[i], loy[i], hiy[i],
                     fixed[i] ? 1 : 0, own_pad[i], half[i]);
      std::fprintf(f, "], \"pads\": [");
      for (std::size_t i = 0; i < pads.size(); ++i)
        std::fprintf(f, "%s[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d]", i ? "," : "", pads[i].cx, pads[i].cy, pads[i].hx, pads[i].hy, pads[i].rr, pads[i].ca,
                     pads[i].sa, pads[i].circle ? 1 : 0);
      std::fprintf(f, "], \"cons\": [");
      for (std::size_t i = 0; i < cons.size(); ++i)
        std::fprintf(f, "%s[%d,%d,%d,%d,%.9g,%.9g,%.9g]", i ? "," : "", static_cast<int>(cons[i].kind), cons[i].a, cons[i].b, cons[i].c, cons[i].qx, cons[i].qy,
                     cons[i].req);
      std::fprintf(f, "]}\n");
      std::fclose(f);
    }
  }
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
  // A cut track comes back as the vertices that matter: those that moved, and the ones the pieces need to stay on the same line.
  auto merged = [&](const std::vector<int>& chain) {
    std::vector<int> kept{chain.front()};
    for (std::size_t k = 1; k + 1 < chain.size(); ++k) {
      const auto a = static_cast<std::size_t>(kept.back()), m = static_cast<std::size_t>(chain[k]), b = static_cast<std::size_t>(chain[k + 1]);
      const double dx = nm_to_mm(new_pos[b].x - new_pos[a].x), dy = nm_to_mm(new_pos[b].y - new_pos[a].y), l = std::hypot(dx, dy);
      const double ex = nm_to_mm(new_pos[m].x - new_pos[a].x), ey = nm_to_mm(new_pos[m].y - new_pos[a].y);
      const double cross = l > 0 ? std::fabs(dx * ey - dy * ex) / l : std::hypot(ex, ey);
      const double along = l > 0 ? (dx * ex + dy * ey) / l : 0;
      const bool joint = node_net[m] == node_net[a] && is_anchor[m];  // a junction or a via sits here: it stays a vertex
      if (!joint && cross < 5e-4 && along > 0 && along < l) continue;
      kept.push_back(chain[k]);
    }
    kept.push_back(chain.back());
    return kept;
  };
  for (std::size_t ri = 0; ri < routes.size(); ++ri) {
    auto& cr = routes[ri];
    std::vector<model::Track> out;
    out.reserve(cr.tracks.size());
    for (std::size_t ti = 0; ti < cr.tracks.size(); ++ti) {
      auto t = cr.tracks[ti];
      if (t.a == t.b || t.locked) {
        out.push_back(t);
        continue;
      }
      if (const auto it = chain_of.find({ri, ti}); it != chain_of.end()) {
        const auto kept = merged(it->second);
        for (std::size_t k = 0; k + 1 < kept.size(); ++k) {
          model::Track u = t;
          u.a = new_pos[static_cast<std::size_t>(kept[k])];
          u.b = new_pos[static_cast<std::size_t>(kept[k + 1])];
          if (!(u.a == u.b)) out.push_back(u);
        }
        continue;
      }
      const int a = node_of.at(std::make_tuple(t.net, t.a.x, t.a.y)), b = node_of.at(std::make_tuple(t.net, t.b.x, t.b.y));
      t.a = new_pos[static_cast<std::size_t>(a)];
      t.b = new_pos[static_cast<std::size_t>(b)];
      out.push_back(t);
    }
    cr.tracks = std::move(out);
    for (auto& v : cr.vias) {
      if (v.locked) continue;
      v.pos = new_pos[static_cast<std::size_t>(node_of.at(std::make_tuple(v.net, v.pos.x, v.pos.y)))];
    }
  }
  return res;
}

}  // namespace tmk::route

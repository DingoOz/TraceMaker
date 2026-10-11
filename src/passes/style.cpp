// SPDX-License-Identifier: GPL-3.0-or-later
#include "passes/style.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

#include "drc/connectivity.hpp"
#include "drc/copper.hpp"
#include "geom/shape.hpp"

namespace tmk::passes {
namespace {

using geom::i128;
using model::NetId;
using model::Point;

struct Piece {
  Point a, b;
  Coord width = 0;
  int layer = -1;
  NetId net = 0;
  bool arc = false;
  Coord len = 0;
  auto key() const { return std::tie(net, layer, a.x, a.y, b.x, b.y, width, arc, len); }
};

Coord dist(Point p, Point q) {
  return static_cast<Coord>(std::llround(std::hypot(static_cast<double>(q.x - p.x), static_cast<double>(q.y - p.y))));
}

// Shortest path between two points with steps along the axes and the diagonals: max + (sqrt 2 - 1) min.
Coord octile(Point p, Point q) {
  const Coord dx = std::llabs(q.x - p.x), dy = std::llabs(q.y - p.y);
  return std::max(dx, dy) + static_cast<Coord>(std::llround(0.41421356237309505 * static_cast<double>(std::min(dx, dy))));
}

// More than 0.1 degree away from every multiple of 45 degrees (tan 0.1 deg = 0.0017453).
bool off_octilinear(Point d) {
  const i128 m = std::min(std::llabs(d.x), std::llabs(d.y)), big = std::max(std::llabs(d.x), std::llabs(d.y));
  if (m * 1'000'000'0 < big * 17'453) return false;                 // along an axis
  return (big - m) * 1'000'000'0 >= (big + m) * 17'453;             // tan(45 deg - angle) = (big - m) / (big + m)
}

struct Step {  // one directed segment of a connection
  Point p, q;
  Coord width = 0, len = 0;
  int layer = -1;
  bool arc = false;
  bool via_after = false;  // a layer change (or a via) between this segment and the next
};

void measure_connection(std::vector<Step>& raw, const StyleOptions& opt, ConnectionStyle& c) {
  // Join collinear pieces of one width: the router commits a straight run as several lattice steps.
  std::vector<Step> s;
  for (const auto& x : raw) {
    if (!s.empty() && !s.back().arc && !x.arc && !s.back().via_after && s.back().layer == x.layer && s.back().width == x.width) {
      const Point d0 = s.back().q - s.back().p, d1 = x.q - x.p;
      const i128 cross = static_cast<i128>(d0.x) * d1.y - static_cast<i128>(d0.y) * d1.x;
      const i128 dot = static_cast<i128>(d0.x) * d1.x + static_cast<i128>(d0.y) * d1.y;
      if (cross == 0 && dot > 0) {
        s.back().q = x.q;
        s.back().len += x.len;
        s.back().via_after = x.via_after;
        continue;
      }
    }
    s.push_back(x);
  }
  c.segments = static_cast<int>(s.size());
  // turn[i]: the bend between segment i and i + 1: 0 none (straight on, a layer change or an arc), +1 / -1 by side.
  std::vector<int> turn(s.size(), 0);
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto& x = s[i];
    c.length += x.len;
    c.vias += x.via_after && i + 1 < s.size();
    if (x.arc) {
      ++c.arcs;
      continue;
    }
    c.off_angle += off_octilinear(x.q - x.p);
    c.short_segments += x.len < x.width;
    if (i + 1 >= s.size() || x.via_after || s[i + 1].arc || s[i + 1].layer != x.layer) continue;
    const Point d0 = x.q - x.p, d1 = s[i + 1].q - s[i + 1].p;
    const i128 cross = static_cast<i128>(d0.x) * d1.y - static_cast<i128>(d0.y) * d1.x;
    const i128 dot = static_cast<i128>(d0.x) * d1.x + static_cast<i128>(d0.y) * d1.y;
    if (cross == 0 && dot > 0) continue;  // collinear pieces of two widths
    ++c.bends;
    c.acute += dot < 0;  // the direction turns by more than 90 degrees: the copper encloses less than 90
    turn[i] = cross > 0 ? 1 : cross < 0 ? -1 : 2;  // 2: doubles back on itself (no side)
  }
  // Stair-steps: maximal runs of short segments joined by bends that alternate sides.
  auto is_short = [&](std::size_t i) { return !s[i].arc && s[i].len < opt.stair_widths * s[i].width; };
  for (std::size_t i = 0; i < s.size();) {
    if (!is_short(i)) {
      ++i;
      continue;
    }
    std::size_t j = i;
    while (j + 1 < s.size() && is_short(j + 1) && (turn[j] == 1 || turn[j] == -1) && (j == i || turn[j] == -turn[j - 1])) ++j;
    if (static_cast<int>(j - i + 1) >= opt.stair_min_segments) ++c.stair_steps;
    // A run that ended because two bends turned the same way can start again at its last segment.
    i = j > i && j + 1 < s.size() && is_short(j + 1) && (turn[j] == 1 || turn[j] == -1) ? j : j + 1;
  }
}

}  // namespace

StyleReport measure_style(const model::Board& b, const std::vector<model::Track>& tracks, const std::vector<model::ArcTrack>& arcs,
                          const std::vector<model::Via>& vias, const StyleOptions& opt) {
  std::vector<Piece> pieces;
  for (const auto& t : tracks) {
    if (t.a == t.b) continue;
    Piece p{t.a, t.b, t.width, t.layer, t.net, false, dist(t.a, t.b)};
    if (std::tie(p.b.x, p.b.y) < std::tie(p.a.x, p.a.y)) std::swap(p.a, p.b);
    pieces.push_back(p);
  }
  for (const auto& t : arcs) {
    if (t.a == t.b) continue;
    Piece p{t.a, t.b, t.width, t.layer, t.net, true, 0};
    const auto pts = geom::arc_points(t.a, t.mid, t.b, 1'000);
    for (std::size_t k = 0; k + 1 < pts.size(); ++k) p.len += dist(pts[k], pts[k + 1]);
    if (std::tie(p.b.x, p.b.y) < std::tie(p.a.x, p.a.y)) std::swap(p.a, p.b);
    pieces.push_back(p);
  }
  std::sort(pieces.begin(), pieces.end(), [](const Piece& x, const Piece& y) { return x.key() < y.key(); });

  // Nodes: track ends by (net, layer, point); a via joins the nodes of its net at its position on the layers it
  // spans. Ids follow the sorted key order, and a joined group is named by its lowest id.
  using Key = std::tuple<NetId, Coord, Coord, int>;  // net, x, y, layer
  std::map<Key, int> ids;
  for (const auto& p : pieces)
    for (const Point e : {p.a, p.b}) ids.emplace(Key{p.net, e.x, e.y, p.layer}, 0);
  std::vector<Key> keys;
  for (auto& [k, id] : ids) {
    id = static_cast<int>(keys.size());
    keys.push_back(k);
  }
  drc::UnionFind uf(keys.size());
  std::vector<std::uint8_t> has_via(keys.size(), 0);
  for (const auto& v : vias) {
    int first = -1;
    for (int l = v.layer_top; l <= v.layer_bottom; ++l) {
      const auto it = ids.find(Key{v.net, v.pos.x, v.pos.y, l});
      if (it == ids.end()) continue;
      has_via[static_cast<std::size_t>(it->second)] = 1;
      if (first < 0) first = it->second;
      else uf.unite(first, it->second);
    }
  }
  auto node_of = [&](const Piece& p, Point e) { return uf.find(ids.at(Key{p.net, e.x, e.y, p.layer})); };

  struct Node {
    std::vector<std::pair<int, int>> ends;  // (piece, 0 = its a end, 1 = its b end), in piece order
    bool via = false, pad = false;
  };
  std::vector<Node> nodes(keys.size());
  for (std::size_t i = 0; i < keys.size(); ++i)
    if (has_via[i]) nodes[static_cast<std::size_t>(uf.find(static_cast<int>(i)))].via = true;
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    nodes[static_cast<std::size_t>(node_of(pieces[i], pieces[i].a))].ends.emplace_back(static_cast<int>(i), 0);
    nodes[static_cast<std::size_t>(node_of(pieces[i], pieces[i].b))].ends.emplace_back(static_cast<int>(i), 1);
  }
  // A node on a pad of its net ends the connection there, whatever passes through.
  {
    struct PadShape {
      std::vector<geom::Shape> shapes;
      geom::Box box;
      model::LayerMask copper = 0;
    };
    std::map<NetId, std::vector<PadShape>> pads;
    for (const auto& p : b.pads) {
      if (p.net == 0 || p.copper == 0) continue;
      PadShape ps{drc::pad_shapes(p), {}, p.copper};
      for (const auto& s : ps.shapes) ps.box.add(s.box);
      pads[p.net].push_back(std::move(ps));
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
      const auto& [net, x, y, layer] = keys[i];
      auto& n = nodes[static_cast<std::size_t>(uf.find(static_cast<int>(i)))];
      const auto it = pads.find(net);
      if (n.pad || it == pads.end()) continue;
      const Point pt{x, y};
      const geom::Shape probe = geom::Shape::point(pt, 0);
      for (const auto& ps : it->second) {
        if (!(ps.copper & model::layer_bit(layer)) || ps.box.empty() || pt.x < ps.box.x0 || pt.x > ps.box.x1 || pt.y < ps.box.y0 || pt.y > ps.box.y1) continue;
        for (const auto& s : ps.shapes)
          if (geom::closer_than(probe, s, 1)) n.pad = true;
        if (n.pad) break;
      }
    }
  }
  auto through = [&](int n) { return nodes[static_cast<std::size_t>(n)].ends.size() == 2 && !nodes[static_cast<std::size_t>(n)].pad; };

  StyleReport rep;
  std::vector<std::uint8_t> used(pieces.size(), 0);
  auto walk = [&](int start, int piece, int end) {
    ConnectionStyle c;
    std::vector<Step> steps;
    const Piece& first = pieces[static_cast<std::size_t>(piece)];
    c.net = first.net;
    c.a = end == 0 ? first.a : first.b;
    c.layer_a = first.layer;
    int node = start;
    for (;;) {
      const Piece& p = pieces[static_cast<std::size_t>(piece)];
      used[static_cast<std::size_t>(piece)] = 1;
      const Point from = end == 0 ? p.a : p.b, to = end == 0 ? p.b : p.a;
      steps.push_back({from, to, p.width, p.len, p.layer, p.arc, false});
      c.b = to;
      c.layer_b = p.layer;
      node = node_of(p, to);
      if (node == start || !through(node)) break;
      const auto& e = nodes[static_cast<std::size_t>(node)].ends;
      const auto& next = e[0].first == piece ? e[1] : e[0];
      if (used[static_cast<std::size_t>(next.first)]) break;
      steps.back().via_after = nodes[static_cast<std::size_t>(node)].via || pieces[static_cast<std::size_t>(next.first)].layer != p.layer;
      piece = next.first;
      end = next.second;
    }
    measure_connection(steps, opt, c);
    c.octile = octile(c.a, c.b);
    rep.connections.push_back(c);
  };
  for (std::size_t n = 0; n < nodes.size(); ++n) {
    if (nodes[n].ends.empty() || through(static_cast<int>(n))) continue;
    for (const auto& [piece, end] : nodes[n].ends)
      if (!used[static_cast<std::size_t>(piece)]) walk(static_cast<int>(n), piece, end);
  }
  for (std::size_t i = 0; i < pieces.size(); ++i)  // closed loops without any end
    if (!used[i]) walk(node_of(pieces[i], pieces[i].a), static_cast<int>(i), 0);

  std::sort(rep.connections.begin(), rep.connections.end(), [](const ConnectionStyle& x, const ConnectionStyle& y) {
    return std::tie(x.net, x.a.x, x.a.y, x.layer_a, x.b.x, x.b.y, x.layer_b, x.length, x.segments) <
           std::tie(y.net, y.a.x, y.a.y, y.layer_a, y.b.x, y.b.y, y.layer_b, y.length, y.segments);
  });
  rep.vias = static_cast<int>(vias.size());
  for (const auto& c : rep.connections) {
    rep.segments += c.segments;
    rep.bends += c.bends;
    rep.acute += c.acute;
    rep.stair_steps += c.stair_steps;
    rep.short_segments += c.short_segments;
    rep.off_angle += c.off_angle;
    rep.arcs += c.arcs;
    rep.length += c.length;
    if (c.octile > 0) {
      rep.detour_length += c.length;
      rep.detour_octile += c.octile;
      rep.long_detours += static_cast<i128>(c.length) * 100 > static_cast<i128>(c.octile) * opt.long_detour_pc;
    }
  }
  return rep;
}

nlohmann::json style_json(const model::Board& b, const StyleReport& r, bool per_connection) {
  // Ratios are rounded to four places so the text is the same wherever it is printed.
  auto r4 = [](double v) { return std::round(v * 1e4) / 1e4; };
  nlohmann::json j{{"connections", r.connections.size()},
                   {"segments", r.segments},
                   {"length_mm", static_cast<double>(r.length) / 1e6},
                   {"vias", r.vias},
                   {"bends", r.bends},
                   {"bends_per_connection", r4(r.bends_per_connection())},
                   {"acute_angles", r.acute},
                   {"stair_steps", r.stair_steps},
                   {"short_segments", r.short_segments},
                   {"off_angle_segments", r.off_angle},
                   {"arcs", r.arcs},
                   {"detour_ratio", r4(r.detour_ratio())},
                   {"detour_length_mm", static_cast<double>(r.detour_length) / 1e6},  // the ratio's two sums, so that
                   {"detour_octile_mm", static_cast<double>(r.detour_octile) / 1e6},  // boards can be added up
                   {"long_detours", r.long_detours}};
  if (per_connection) {
    nlohmann::json rows = nlohmann::json::array();
    for (const auto& c : r.connections)
      rows.push_back({{"net", b.nets[static_cast<std::size_t>(c.net)].name},
                      {"from", {c.a.x, c.a.y}},
                      {"to", {c.b.x, c.b.y}},
                      {"segments", c.segments},
                      {"length_mm", static_cast<double>(c.length) / 1e6},
                      {"vias", c.vias},
                      {"bends", c.bends},
                      {"acute_angles", c.acute},
                      {"stair_steps", c.stair_steps},
                      {"short_segments", c.short_segments},
                      {"off_angle_segments", c.off_angle},
                      {"arcs", c.arcs},
                      {"detour_ratio", c.octile > 0 ? r4(static_cast<double>(c.length) / static_cast<double>(c.octile)) : 1.0}});
    j["per_connection"] = std::move(rows);
  }
  return j;
}

}  // namespace tmk::passes

// SPDX-License-Identifier: GPL-3.0-or-later
// Routed KiCad board -> SRJ `traces` (design doc 08 §9).
//
// An SRJ trace is a path: wire points and vias in order. A routed net is a graph, so each connection gets the
// shortest path through its net's copper between the pads of its points (Dijkstra, "A note on two problems in
// connexion with graphs", Numer. Math. 1, 1959), and the copper no path used follows as further traces, so that a
// checker reading the file sees all of it.
#include <algorithm>
#include <cmath>
#include <map>
#include <nlohmann/json.hpp>
#include <queue>

#include "drc/copper.hpp"
#include "geom/shape.hpp"
#include "io/srj/srj.hpp"

namespace tmk::io {
namespace {
using geom::Point;
using nlohmann::ordered_json;

enum class Kind : std::uint8_t { Track, Via, Pad };

struct Node {
  Kind kind = Kind::Track;
  Point pos;
  int layer = -1;  // track nodes
  int index = -1;  // board via / pad index
};

struct Edge {
  int a = 0, b = 0;
  double length = 0;
  Coord width = 0;  // > 0: a piece of a track; 0: a join inside a via or pad
  bool used = false;
};

double mm_x(Coord v) { return static_cast<double>(v) / 1e6; }
double mm_y(Coord v) { return v == 0 ? 0.0 : -static_cast<double>(v) / 1e6; }

// One net's copper as a graph: track ends, vias and pads are nodes.
struct NetGraph {
  const model::Board& b;  // the routed board
  int layer_count;
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::vector<std::vector<int>> adj;  // edge indices per node, ascending
  std::map<std::tuple<Coord, Coord, int>, int> track_node;
  std::map<int, int> via_node, pad_node;
  std::vector<bool> via_used;  // per node (via nodes only)

  NetGraph(const model::Board& board, int layers) : b(board), layer_count(layers) {}

  int add(Node n) {
    nodes.push_back(n);
    adj.emplace_back();
    via_used.push_back(false);
    return static_cast<int>(nodes.size()) - 1;
  }
  int track_at(Point p, int layer) {
    const auto [it, fresh] = track_node.emplace(std::tuple{p.x, p.y, layer}, 0);
    if (fresh) it->second = add(Node{Kind::Track, p, layer, -1});
    return it->second;
  }
  void link(int a, int b2, Coord width) {
    if (a == b2) return;
    const Point d = nodes[static_cast<std::size_t>(a)].pos - nodes[static_cast<std::size_t>(b2)].pos;
    double length = std::hypot(static_cast<double>(d.x), static_cast<double>(d.y));
    // A join across a pad costs more than track over the same ground, so a path follows the copper drawn inside
    // the pad (a track running on to the pad centre) rather than cutting across to where the track enters it.
    if (width == 0) length = 4 * length + 1'000;
    edges.push_back(Edge{a, b2, length, width, false});
    adj[static_cast<std::size_t>(a)].push_back(static_cast<int>(edges.size()) - 1);
    adj[static_cast<std::size_t>(b2)].push_back(static_cast<int>(edges.size()) - 1);
  }
  int other(int edge, int node) const {
    const Edge& e = edges[static_cast<std::size_t>(edge)];
    return e.a == node ? e.b : e.a;
  }

  struct Seg {
    Point a, b;
    int layer;
    Coord width;
  };

  // The point of segment `s` nearest to `p`, to the nanometre.
  static Point nearest(Point p, const Seg& s) {
    const double dx = static_cast<double>(s.b.x - s.a.x), dy = static_cast<double>(s.b.y - s.a.y);
    const double len2 = dx * dx + dy * dy;
    if (len2 == 0) return s.a;
    const double u = std::clamp((static_cast<double>(p.x - s.a.x) * dx + static_cast<double>(p.y - s.a.y) * dy) / len2, 0.0, 1.0);
    if (u == 0.0) return s.a;
    if (u == 1.0) return s.b;
    return {s.a.x + geom::kiround(u * dx), s.a.y + geom::kiround(u * dy)};
  }

  // Copper of one net that touches is connected, as in KiCad 10's connectivity (items are linked where their
  // shapes collide): a track end or a via on another track, two tracks that cross, a track or a via over a pad.
  // Tracks are cut at those points, and a join (an edge without width) links what is not on the centre line.
  void build(model::NetId net) {
    std::vector<Seg> segs;
    for (const auto& t : b.tracks)
      if (t.net == net && t.layer >= 0) segs.push_back(Seg{t.a, t.b, t.layer, t.width});
    for (const auto& t : b.arcs) {
      if (t.net != net || t.layer < 0) continue;
      const auto pts = geom::arc_points(t.a, t.mid, t.b);
      for (std::size_t k = 1; k < pts.size(); ++k) segs.push_back(Seg{pts[k - 1], pts[k], t.layer, t.width});
    }
    std::vector<int> vias, pads;
    for (std::size_t k = 0; k < b.vias.size(); ++k)
      if (b.vias[k].net == net) vias.push_back(static_cast<int>(k));
    for (std::size_t k = 0; k < b.pads.size(); ++k)
      if (b.pads[k].net == net && b.pads[k].copper != 0) pads.push_back(static_cast<int>(k));
    auto spans = [&](const model::Via& via, int z) { return z >= via.layer_top && z <= via.layer_bottom; };

    struct Join {
      Point from, to;  // `from`: a track end (track joins only)
      int index, layer;
    };
    std::vector<std::vector<Point>> cuts(segs.size());
    std::vector<Join> track_joins, via_joins, pad_joins;
    for (std::size_t k = 0; k < segs.size(); ++k) cuts[k] = {segs[k].a, segs[k].b};
    for (std::size_t k = 0; k < segs.size(); ++k) {
      const Seg& s = segs[k];
      for (std::size_t m = 0; m < segs.size(); ++m) {
        const Seg& o = segs[m];
        if (m == k || o.layer != s.layer) continue;
        for (const Point e : {o.a, o.b})
          if (geom::point_seg_closer(e, s.a, s.b, (o.width + s.width) / 2)) {
            const Point q = nearest(e, s);
            cuts[k].push_back(q);
            if (q != e) track_joins.push_back(Join{e, q, -1, s.layer});
          }
        // A crossing in the middle of both tracks: the point where the centre lines meet.
        const double d1x = static_cast<double>(s.b.x - s.a.x), d1y = static_cast<double>(s.b.y - s.a.y);
        const double d2x = static_cast<double>(o.b.x - o.a.x), d2y = static_cast<double>(o.b.y - o.a.y);
        const double den = d1x * d2y - d1y * d2x;
        if (den != 0 && geom::segments_intersect(s.a, s.b, o.a, o.b)) {
          const double u = (static_cast<double>(o.a.x - s.a.x) * d2y - static_cast<double>(o.a.y - s.a.y) * d2x) / den;
          // Rounded once, from the segment with the lower index, so both tracks are cut at the same point.
          if (k < m) {
            const Point q{s.a.x + geom::kiround(u * d1x), s.a.y + geom::kiround(u * d1y)};
            cuts[k].push_back(q);
            cuts[m].push_back(q);
          }
        }
      }
      for (int v : vias) {
        const auto& via = b.vias[static_cast<std::size_t>(v)];
        if (spans(via, s.layer) && geom::point_seg_closer(via.pos, s.a, s.b, (via.size + s.width) / 2)) {
          const Point q = nearest(via.pos, s);
          cuts[k].push_back(q);
          via_joins.push_back(Join{via.pos, q, v, s.layer});
        }
      }
      for (int pi : pads) {
        const auto& pad = b.pads[static_cast<std::size_t>(pi)];
        if (!(pad.copper & model::layer_bit(s.layer))) continue;
        const auto shapes = drc::pad_shapes(pad);
        const geom::Shape track = geom::Shape::segment(s.a, s.b, s.width / 2);
        if (std::none_of(shapes.begin(), shapes.end(), [&](const geom::Shape& sh) { return geom::closer_than(sh, track, 0); })) continue;
        // Where the centre line is nearest to the pad centre. If that point is off the pad (a long pad, a track
        // that only grazes it) but an end of the track is on it, the track joins at that end.
        auto on_pad = [&](Point e) { return std::any_of(shapes.begin(), shapes.end(), [&](const geom::Shape& sh) { return geom::closer_than_disk(sh, e, 0, 1); }); };
        Point q = nearest(pad.pos, s);
        if (!on_pad(q)) {
          if (on_pad(s.a)) q = s.a;
          else if (on_pad(s.b)) q = s.b;
        }
        cuts[k].push_back(q);
        pad_joins.push_back(Join{pad.pos, q, pi, s.layer});
      }
    }
    for (std::size_t k = 0; k < segs.size(); ++k) {
      const Seg& s = segs[k];
      auto& on = cuts[k];
      auto along = [&](Point q) { return std::abs(static_cast<double>(q.x - s.a.x)) + std::abs(static_cast<double>(q.y - s.a.y)); };
      std::sort(on.begin(), on.end(), [&](Point l, Point r) { return along(l) != along(r) ? along(l) < along(r) : std::pair{l.x, l.y} < std::pair{r.x, r.y}; });
      on.erase(std::unique(on.begin(), on.end()), on.end());
      for (std::size_t m = 1; m < on.size(); ++m) link(track_at(on[m - 1], s.layer), track_at(on[m], s.layer), s.width);
    }
    for (const Join& jn : track_joins) link(track_at(jn.from, jn.layer), track_at(jn.to, jn.layer), 0);
    for (int v : vias) via_node[v] = add(Node{Kind::Via, b.vias[static_cast<std::size_t>(v)].pos, -1, v});
    for (const Join& jn : via_joins) link(via_node[jn.index], track_at(jn.to, jn.layer), 0);
    for (std::size_t k = 0; k < vias.size(); ++k)
      for (std::size_t m = k + 1; m < vias.size(); ++m) {
        const auto& x = b.vias[static_cast<std::size_t>(vias[k])];
        const auto& y = b.vias[static_cast<std::size_t>(vias[m])];
        const Point d = x.pos - y.pos;
        if (std::max(x.layer_top, y.layer_top) <= std::min(x.layer_bottom, y.layer_bottom) &&
            std::hypot(static_cast<double>(d.x), static_cast<double>(d.y)) < static_cast<double>(x.size + y.size) / 2)
          link(via_node[vias[k]], via_node[vias[m]], 0);
      }
    for (int pi : pads) {
      const auto& pad = b.pads[static_cast<std::size_t>(pi)];
      const int n = add(Node{Kind::Pad, pad.pos, -1, pi});
      pad_node[pi] = n;
      const auto shapes = drc::pad_shapes(pad);
      for (int v : vias) {
        const auto& via = b.vias[static_cast<std::size_t>(v)];
        model::LayerMask on = 0;
        for (int z = via.layer_top; z <= via.layer_bottom; ++z) on |= model::layer_bit(z);
        if ((on & pad.copper) && std::any_of(shapes.begin(), shapes.end(), [&](const geom::Shape& sh) { return geom::closer_than_disk(sh, via.pos, via.size / 2, 0); }))
          link(n, via_node[v], 0);
      }
    }
    for (const Join& jn : pad_joins) link(pad_node[jn.index], track_at(jn.to, jn.layer), 0);
  }

  // Node path from `from` to `to` (both included), empty if there is none. Ties go to the lower node index.
  std::vector<int> path(int from, int to) const {
    std::vector<double> dist(nodes.size(), -1);
    std::vector<int> via_edge(nodes.size(), -1);
    using Item = std::pair<double, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    dist[static_cast<std::size_t>(from)] = 0;
    open.emplace(0.0, from);
    while (!open.empty()) {
      const auto [d, n] = open.top();
      open.pop();
      if (d > dist[static_cast<std::size_t>(n)]) continue;
      if (n == to) break;
      for (int e : adj[static_cast<std::size_t>(n)]) {
        const int m = other(e, n);
        const double nd = d + edges[static_cast<std::size_t>(e)].length;
        if (dist[static_cast<std::size_t>(m)] < 0 || nd < dist[static_cast<std::size_t>(m)]) {
          dist[static_cast<std::size_t>(m)] = nd;
          via_edge[static_cast<std::size_t>(m)] = e;
          open.emplace(nd, m);
        }
      }
    }
    std::vector<int> out;
    if (dist[static_cast<std::size_t>(to)] < 0) return out;
    for (int n = to; n != from; n = other(via_edge[static_cast<std::size_t>(n)], n)) out.push_back(n);
    out.push_back(from);
    std::reverse(out.begin(), out.end());
    return out;
  }

  int edge_between(int a, int b2) const {
    for (int e : adj[static_cast<std::size_t>(a)])
      if (other(e, a) == b2) return e;
    return -1;
  }

  void mark(const std::vector<int>& p) {
    for (std::size_t i = 0; i < p.size(); ++i) {
      if (nodes[static_cast<std::size_t>(p[i])].kind == Kind::Via) via_used[static_cast<std::size_t>(p[i])] = true;
      if (i > 0) edges[static_cast<std::size_t>(edge_between(p[i - 1], p[i]))].used = true;
    }
  }

  bool pad_has(const Node& n, int layer) const { return layer >= 0 && (b.pads[static_cast<std::size_t>(n.index)].copper & model::layer_bit(layer)); }

  // The SRJ route along a node path. `first` / `last`: the problem's own coordinates for the end pads.
  ordered_json route(const std::vector<int>& p, const SrjPoint* first, const SrjPoint* last, Coord default_width, bool blind) const {
    ordered_json out = ordered_json::array();
    Coord width = default_width;
    for (std::size_t i = 1; i < p.size(); ++i)
      if (const Edge& e = edges[static_cast<std::size_t>(edge_between(p[i - 1], p[i]))]; e.width > 0) {
        width = e.width;
        break;
      }
    // Two points that are the same to the nanometre are one point: a zero-length wire is no copper, and a
    // terminal written with the problem's own digits (`exact`) replaces the track end it coincides with.
    auto wire = [&](double x, double y, int layer, bool exact = false) {
      if (!out.empty() && out.back()["route_type"] == "wire" && out.back()["layer"] == srj_layer_name(layer, layer_count) &&
          mm_to_nm(out.back()["x"].get<double>()) == mm_to_nm(x) && mm_to_nm(out.back()["y"].get<double>()) == mm_to_nm(y)) {
        if (exact) {
          out.back()["x"] = x;
          out.back()["y"] = y;
        }
        return;
      }
      out.push_back({{"route_type", "wire"}, {"x", x}, {"y", y}, {"width", static_cast<double>(width) / 1e6}, {"layer", srj_layer_name(layer, layer_count)}});
    };
    // The layer on which the path leaves node i towards node i + 1, if that is decided by what follows.
    auto layer_after = [&](std::size_t i, int cur) -> int {
      for (std::size_t k = i + 1; k < p.size(); ++k) {
        const Node& n = nodes[static_cast<std::size_t>(p[k])];
        if (n.kind == Kind::Track) return n.layer;
        if (n.kind == Kind::Pad) {
          if (pad_has(n, cur)) return cur;
          for (int z = 0; z < layer_count; ++z)
            if (pad_has(n, z)) return z;
        }
      }
      return -1;
    };
    int cur = -1;
    for (std::size_t i = 0; i < p.size(); ++i) {
      const Node& n = nodes[static_cast<std::size_t>(p[i])];
      if (i > 0)
        if (const Edge& e = edges[static_cast<std::size_t>(edge_between(p[i - 1], p[i]))]; e.width > 0) width = e.width;
      if (n.kind == Kind::Track) {
        cur = n.layer;
        wire(mm_x(n.pos.x), mm_y(n.pos.y), cur);
      } else if (n.kind == Kind::Pad) {
        const SrjPoint* at = i == 0 ? first : i + 1 == p.size() ? last : nullptr;
        const double x = at ? at->x : mm_x(n.pos.x), y = at ? at->y : mm_y(n.pos.y);
        const int next = layer_after(i, cur);
        if (!pad_has(n, cur)) {
          cur = pad_has(n, next) ? next : -1;
          for (int z = 0; cur < 0 && z < layer_count; ++z)
            if (pad_has(n, z)) cur = z;
        }
        wire(x, y, cur, at != nullptr);
        // A through-hole pad may hand the path to another layer; SRJ has no route point for that, so the next
        // wire simply starts on the new layer at the pad.
        if (next >= 0 && next != cur && pad_has(n, next) && i + 1 < p.size() && nodes[static_cast<std::size_t>(p[i + 1])].kind == Kind::Track) {
          cur = next;
          wire(x, y, cur);
        }
      } else {
        const auto& via = b.vias[static_cast<std::size_t>(n.index)];
        const int from = cur >= via.layer_top && cur <= via.layer_bottom ? cur : via.layer_top;
        int to = layer_after(i, from);
        if (to < via.layer_top || to > via.layer_bottom) to = from == via.layer_bottom ? via.layer_top : via.layer_bottom;
        if (to == from) continue;
        const double x = mm_x(via.pos.x), y = mm_y(via.pos.y);
        wire(x, y, from);
        ordered_json v = {{"route_type", "via"}, {"x", x}, {"y", y}, {"from_layer", srj_layer_name(from, layer_count)}, {"to_layer", srj_layer_name(to, layer_count)}};
        if (blind) {
          v["layers"] = ordered_json::array();
          for (int z = via.layer_top; z <= via.layer_bottom; ++z) v["layers"].push_back(srj_layer_name(z, layer_count));
        }
        v["via_diameter"] = static_cast<double>(via.size) / 1e6;
        v["via_hole_diameter"] = static_cast<double>(via.drill) / 1e6;
        out.push_back(std::move(v));
        cur = to;
        wire(x, y, cur);
      }
    }
    return out;
  }

  // Copper no connection path used, as node chains: unused track pieces joined end to end, through unused vias.
  std::vector<std::vector<int>> leftovers() {
    std::vector<std::vector<int>> out;
    auto grow = [&](std::vector<int>& chain) {
      for (;;) {
        const int tail = chain.back();
        int next_edge = -1;
        for (int e : adj[static_cast<std::size_t>(tail)]) {
          const Edge& k = edges[static_cast<std::size_t>(e)];
          const Node& m = nodes[static_cast<std::size_t>(other(e, tail))];
          const bool track = k.width > 0 && !k.used;
          const bool into_via = k.width == 0 && m.kind == Kind::Via && !via_used[static_cast<std::size_t>(other(e, tail))];
          const bool out_of_via = k.width == 0 && nodes[static_cast<std::size_t>(tail)].kind == Kind::Via && m.kind == Kind::Track && !k.used;
          if (track || into_via || out_of_via) {
            next_edge = e;
            break;
          }
        }
        if (next_edge < 0) return;
        const int m = other(next_edge, tail);
        edges[static_cast<std::size_t>(next_edge)].used = true;
        if (nodes[static_cast<std::size_t>(m)].kind == Kind::Via) via_used[static_cast<std::size_t>(m)] = true;
        chain.push_back(m);
      }
    };
    for (std::size_t e = 0; e < edges.size(); ++e) {
      if (edges[e].used || edges[e].width == 0) continue;
      edges[e].used = true;
      std::vector<int> chain{edges[e].a, edges[e].b};
      grow(chain);
      std::reverse(chain.begin(), chain.end());
      grow(chain);
      out.push_back(std::move(chain));
    }
    for (std::size_t n = 0; n < nodes.size(); ++n) {
      if (nodes[n].kind != Kind::Via || via_used[n]) continue;
      via_used[n] = true;
      std::vector<int> chain{static_cast<int>(n)};
      grow(chain);
      out.push_back(std::move(chain));
    }
    return out;
  }
};

}  // namespace

SrjExport srj_export(const std::string& problem_text, const model::Board& routed) {
  const SrjProblem p = read_srj(problem_text);
  ordered_json doc = ordered_json::parse(problem_text);
  SrjExport res;
  res.connections = static_cast<int>(p.connections.size());
  ordered_json traces = ordered_json::array();
  if (routed.copper_count() != std::max(2, p.layer_count))
    throw std::runtime_error("the board has " + std::to_string(routed.copper_count()) + " copper layers, the problem " + std::to_string(p.layer_count));
  std::vector<bool> known(routed.nets.size(), false);
  std::vector<bool> routed_ok(p.connections.size(), false);
  for (const auto& net : p.nets) {
    const model::NetId id = routed.net_by_name(net.name);
    if (id <= 0) {
      res.warnings.push_back("net '" + net.name + "' is not on the board");
      continue;
    }
    known[static_cast<std::size_t>(id)] = true;
    NetGraph g(routed, p.layer_count);
    g.build(id);
    // The pad under a point: a pad of the net with copper on one of the point's layers that contains it.
    auto pad_of = [&](const SrjPoint& pt) -> int {
      const Point q{mm_to_nm(pt.x), -mm_to_nm(pt.y)};
      int best = -1;
      for (const auto& [pad_index, node] : g.pad_node) {
        const auto& pad = routed.pads[static_cast<std::size_t>(pad_index)];
        if (std::none_of(pt.layers.begin(), pt.layers.end(), [&](int z) { return pad.copper & model::layer_bit(z); })) continue;
        const auto shapes = drc::pad_shapes(pad);
        // 1 µm: the importer accepts a point that far outside its obstacle, as tscircuit does.
        if (std::none_of(shapes.begin(), shapes.end(), [&](const geom::Shape& s) { return geom::closer_than_disk(s, q, 0, 1'001); })) continue;
        if (pad.pos == q) return node;
        if (best < 0) best = node;
      }
      return best;
    };
    const Coord default_width = mm_to_nm(p.rules.min_trace_width);
    for (int ci : net.connections) {
      const auto& c = p.connections[static_cast<std::size_t>(ci)];
      bool ok = c.points.size() >= 2;
      std::vector<ordered_json> made;
      for (std::size_t i = 1; i < c.points.size() && ok; ++i) {
        const int a = pad_of(c.points[i - 1]), b = pad_of(c.points[i]);
        if (a < 0 || b < 0) {
          res.warnings.push_back("connection '" + c.name + "': no pad of its net under a point");
          ok = false;
          break;
        }
        if (a == b) continue;  // both points on one pad
        const auto path = g.path(a, b);
        if (path.empty()) {
          ok = false;
          break;
        }
        g.mark(path);
        ordered_json t;
        t["type"] = "pcb_trace";
        t["pcb_trace_id"] = c.points.size() == 2 ? c.name : c.name + "_" + std::to_string(i - 1);
        t["connection_name"] = c.name;
        t["route"] = g.route(path, &c.points[i - 1], &c.points[i], default_width, p.rules.blind_buried_vias);
        made.push_back(std::move(t));
      }
      routed_ok[static_cast<std::size_t>(ci)] = ok;
      // The pieces of a connection that is not whole are still copper on the board: they are exported too.
      for (auto& t : made) traces.push_back(std::move(t));
      res.traces += static_cast<int>(made.size());
    }
    int k = 0;
    for (const auto& chain : g.leftovers()) {
      ordered_json t;
      t["type"] = "pcb_trace";
      t["pcb_trace_id"] = net.name + "_extra_" + std::to_string(k++);
      t["connection_name"] = net.name;
      t["route"] = g.route(chain, nullptr, nullptr, default_width, p.rules.blind_buried_vias);
      traces.push_back(std::move(t));
      ++res.extra_traces;
    }
    res.vias += static_cast<int>(g.via_node.size());
  }
  for (std::size_t i = 0; i < p.connections.size(); ++i) {
    if (routed_ok[i]) ++res.routed;
    else res.unrouted.push_back(p.connections[i].name);
  }
  std::size_t foreign = 0;
  for (const auto& t : routed.tracks) foreign += !known[static_cast<std::size_t>(t.net)];
  for (const auto& t : routed.arcs) foreign += !known[static_cast<std::size_t>(t.net)];
  for (const auto& v : routed.vias) foreign += !known[static_cast<std::size_t>(v.net)];
  if (foreign) res.warnings.push_back(std::to_string(foreign) + " tracks and vias belong to no net of the problem and are not exported");
  doc["traces"] = std::move(traces);
  res.json = doc.dump(2) + "\n";
  return res;
}

}  // namespace tmk::io

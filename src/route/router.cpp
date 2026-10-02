#include "route/router.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <queue>
#include <deque>
#include <set>
#include <unordered_map>

#include "drc/connectivity.hpp"
#include "route/obstacles.hpp"

namespace tmk::route {

using geom::Point;
using model::NetId;

namespace {

// Octilinear directions: E, NE, N, NW, W, SW, S, SE (y down on screen; "N" is -y).
constexpr int kDx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int kDy[8] = {0, -1, -1, -1, 0, 1, 1, 1};
constexpr int kNoDir = 8;

std::string jnum(Coord v) { return std::to_string(v); }

}  // namespace

struct Router::Impl {
  const model::DesignRules& rules;
  const RouterOptions& opt;
  model::Board b;  // working copy
  std::unique_ptr<Obstacles> obs;
  RouteResult res;
  std::chrono::steady_clock::time_point t0;
  Coord pitch = 0;
  geom::Box lat;  // lattice bounds (aligned to pitch)
  int nx = 0, ny = 0, nl = 0;
  drc::UnionFind* clusters = nullptr;
  std::vector<int> pad_item;  // board pad -> copper item index (or -1)

  // ---- per-search scratch (sized to the window) ----
  struct Window { int x0, y0, w, h; };
  std::vector<std::uint32_t> gstamp, cstamp, vstamp;
  std::vector<std::int64_t> g;
  std::vector<std::int32_t> parent;
  std::vector<std::uint8_t> cell_state, via_state;  // 0 unknown, 1 free, 2 blocked (valid when stamp matches)
  std::uint32_t gen = 0;
  std::set<std::pair<int, std::int64_t>> learned_block;  // (net, layer-cell key) blocked after exact-check failures

  // ---- negotiation state ----
  struct ConnState {
    Connection c;
    bool routed = false;
    bool implicit = false;           // satisfied through other routes of the same net (no own copper)
    std::vector<int> items;          // copper item indices owned by this connection
    int rips = 0, fails = 0;
  };
  std::vector<ConnState> cs;
  std::unordered_map<std::int64_t, std::uint16_t> history;  // contested lattice cells (PathFinder history cost)
  std::vector<int> init_root;        // copper item -> initial cluster root (fixed copper)
  bool soft = false;                 // current search may cross routed copper
  // Persistent fixed-obstacle caches per net class (codes from Obstacles::fixed_code), lattice-indexed.
  struct ClassCache {
    std::vector<std::int32_t> margin, tight, via;  // INT32_MIN = not computed yet
    model::NetId rep = 0;
  };
  std::map<const model::NetClass*, ClassCache> caches;
  bool use_cache = true;
  int current = -1;                  // connection being routed
  std::vector<std::int64_t> soft_cells;  // cells of the last soft path that crossed routed copper

  Impl(const model::Board& in, const model::DesignRules& r, const RouterOptions& o) : rules(r), opt(o), b(in) {}

  double elapsed() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
  void emit(const std::string& s) {
    if (opt.sink) opt.sink->publish(s);
  }
  void emit_stats(const char* stage) {
    if (!opt.sink) return;
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  "{\"type\":\"stats\",\"stage\":\"%s\",\"iteration\":1,\"routed\":%d,\"total\":%d,\"unrouted\":%d,\"rips\":0,"
                  "\"failures\":%zu,\"elapsed_s\":%.2f}",
                  stage, res.routed, res.connections, res.connections - res.routed, res.failures.size(), elapsed());
    emit(buf);
  }

  const model::NetClass& netclass(NetId net) const { return rules.class_for(b.nets[static_cast<std::size_t>(net)].name); }
  // Via drill and diameter for a net: net-class values raised to the board minimums (drill, diameter and
  // annular ring: d >= drill + 2 * min_annular).
  Coord via_drill(NetId net) const { return std::max(netclass(net).via_drill, rules.minimums.through_hole_diameter); }
  Coord via_diameter(NetId net) const {
    return std::max({netclass(net).via_diameter, rules.minimums.via_diameter, via_drill(net) + 2 * rules.minimums.via_annular_width});
  }
  Coord track_width(NetId net) const { return std::max(netclass(net).track_width, rules.minimums.track_width); }

  // Lattice <-> board coordinates.
  Point at(int ix, int iy) const { return {lat.x0 + static_cast<Coord>(ix) * pitch, lat.y0 + static_cast<Coord>(iy) * pitch}; }
  int to_ix(Coord x) const { return static_cast<int>(std::llround(static_cast<double>(x - lat.x0) / static_cast<double>(pitch))); }
  int to_iy(Coord y) const { return static_cast<int>(std::llround(static_cast<double>(y - lat.y0) / static_cast<double>(pitch))); }

  void setup() {
    obs = std::make_unique<Obstacles>(b, rules);
    use_cache = !obs->has_custom_rules();
    nl = b.copper_count();
    // Pitch: a fraction of the smallest (width + clearance) so lattice tracks can pass between fine-pitch pads.
    if (opt.pitch > 0) {
      pitch = opt.pitch;
    } else {
      Coord wc = 1'000'000'000;
      for (const auto& c : rules.classes) wc = std::min(wc, std::max(c.track_width, rules.minimums.track_width) + std::max(c.clearance, rules.minimums.clearance));
      pitch = std::clamp<Coord>(wc / 6 / 5'000 * 5'000, 25'000, 100'000);
    }
    res.pitch = pitch;
    const auto bb = obs->bounds();
    lat = geom::Box{bb.x0 / pitch * pitch, bb.y0 / pitch * pitch, bb.x1, bb.y1};
    nx = static_cast<int>((lat.x1 - lat.x0) / pitch) + 1;
    ny = static_cast<int>((lat.y1 - lat.y0) / pitch) + 1;
  }

  // ---------------------------------------------------------------------------------------------------
  // Connections: a minimum spanning tree over each net's existing copper clusters (pads that are already joined
  // by copper count as one node); edges are pad pairs at minimum distance.
  // ---------------------------------------------------------------------------------------------------
  // Distance from a point to a zone fill (0 inside).
  double zone_dist(Point p, int zone_item) const {
    const auto& z = obs->copper().items[static_cast<std::size_t>(zone_item)];
    const auto& poly = z.shapes.front().pts;
    if (geom::point_in_polygon(p, poly)) return 0;
    long double best = 1e30L;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) best = std::min(best, geom::point_seg_dist(p, poly[j], poly[i]));
    return static_cast<double>(best);
  }

  std::vector<Connection> plan(drc::UnionFind& uf) {
    const auto& cm = obs->copper();
    struct Cluster { std::vector<int> pads, zones; };
    std::map<NetId, std::map<int, Cluster>> net_clusters;  // net -> root -> pads and zone fills
    pad_item.assign(b.pads.size(), -1);
    for (std::size_t i = 0; i < cm.items.size(); ++i) {
      const auto& it = cm.items[i];
      if (it.net == 0) continue;
      if (it.kind == drc::ItemKind::Pad) {
        pad_item[static_cast<std::size_t>(it.index)] = static_cast<int>(i);
        net_clusters[it.net][uf.find(static_cast<int>(i))].pads.push_back(it.index);
      } else if (it.kind == drc::ItemKind::Zone && it.footprint < 0) {
        net_clusters[it.net][uf.find(static_cast<int>(i))].zones.push_back(static_cast<int>(i));
      }
    }
    std::vector<Connection> out;
    for (auto& [net, cl] : net_clusters) {
      std::vector<Cluster> groups;
      for (auto& [r, c] : cl)
        if (!c.pads.empty()) groups.push_back(c);  // zone-only clusters (unused fills) are not targets on their own
      if (groups.size() < 2) continue;
      // Prim over clusters; a pad may connect to another cluster's pad or into its zone fill (plane).
      const std::size_t k = groups.size();
      std::vector<std::uint8_t> in(k, 0);
      std::vector<double> best(k, 1e300);
      std::vector<Connection> how(k);
      in[0] = 1;
      auto upd = [&](std::size_t from) {
        for (std::size_t j = 0; j < k; ++j) {
          if (in[j]) continue;
          auto consider = [&](int pa, int pb, int zb, double d) {
            if (d < best[j]) {
              best[j] = d;
              how[j] = Connection{net, pa, pb, zb, static_cast<Coord>(d)};
            }
          };
          for (int pa : groups[from].pads) {
            const Point A = b.pads[static_cast<std::size_t>(pa)].pos;
            for (int pb : groups[j].pads) {
              const Point B = b.pads[static_cast<std::size_t>(pb)].pos;
              consider(pa, pb, -1, std::hypot(static_cast<double>(A.x - B.x), static_cast<double>(A.y - B.y)));
            }
            for (int z : groups[j].zones) consider(pa, -1, z, zone_dist(A, z));
          }
          // And pads of cluster j into zones of the tree side.
          for (int pb : groups[j].pads)
            for (int z : groups[from].zones) consider(pb, -1, z, zone_dist(b.pads[static_cast<std::size_t>(pb)].pos, z));
        }
      };
      upd(0);
      for (std::size_t step = 1; step < k; ++step) {
        std::size_t nxt = k;
        for (std::size_t j = 0; j < k; ++j)
          if (!in[j] && (nxt == k || best[j] < best[nxt])) nxt = j;
        in[nxt] = 1;
        out.push_back(how[nxt]);
        upd(nxt);
      }
    }
    std::stable_sort(out.begin(), out.end(), [](const Connection& a, const Connection& c) { return a.length < c.length; });
    return out;
  }

  // ---------------------------------------------------------------------------------------------------
  // A* on (layer, cell, arrival direction).
  // ---------------------------------------------------------------------------------------------------
  struct Endpoint {
    std::vector<std::pair<int, std::int64_t>> cells;  // (layer, cell index in window)
  };

  static std::int64_t cell_key(int layer, int gx, int gy) {
    return (static_cast<std::int64_t>(layer) << 48) | (static_cast<std::int64_t>(gy) << 24) | gx;
  }
  std::int64_t hist_cost(int layer, int gx, int gy) const {
    if (history.empty()) return 0;
    const auto it = history.find(cell_key(layer, gx, gy));
    return it == history.end() ? 0 : static_cast<std::int64_t>(it->second) * pitch * 2;
  }
  ClassCache& cache_for(NetId net) {
    auto& cc = caches[&netclass(net)];
    if (cc.margin.empty()) {
      const std::size_t n = static_cast<std::size_t>(nl) * static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
      cc.margin.assign(n, INT32_MIN);
      cc.tight.assign(n, INT32_MIN);
      cc.via.assign(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), INT32_MIN);
      cc.rep = net;
    }
    return cc;
  }
  static bool code_ok(std::int32_t code, NetId net) { return code == Obstacles::kFree || code == net; }

  // State of a lattice point for the current net: 0 free, 1 crosses routed copper (soft), 2 blocked,
  // 3 legal only without the lattice margin ("tight").
  int point_state(int layer, int gx, int gy, NetId net, Coord hw) {
    const Point p = at(gx, gy);
    const Coord margin = pitch * 71 / 100 + 1;
    if (!use_cache) {
      int st = obs->disk_state(p, layer, hw, net, margin, soft);
      if (st == 2 && obs->disk_state(p, layer, hw, net, 0, soft) != 2) st = 3;
      return st;
    }
    auto& cc = cache_for(net);
    const std::size_t gi = (static_cast<std::size_t>(layer) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(gy)) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(gx);
    if (cc.margin[gi] == INT32_MIN) cc.margin[gi] = obs->fixed_code(p, layer, hw, margin, cc.rep);
    int st = 0;
    if (!code_ok(cc.margin[gi], net)) {
      if (cc.tight[gi] == INT32_MIN) cc.tight[gi] = obs->fixed_code(p, layer, hw, 0, cc.rep);
      if (!code_ok(cc.tight[gi], net)) return 2;
      st = 3;
    }
    const int r = obs->routed_state(geom::Shape::point(p, hw + (st == 3 ? 0 : margin)), layer, net, drc::ItemKind::Track, soft, nullptr);
    if (r == 2) return 2;
    return r == 1 ? 1 : st;
  }

  // Extra cost of entering a cell: -1 blocked, 0 free, > 0 crossing routed copper (soft mode only).
  std::int64_t cell_cost(const Window& w, int layer, int cx, int cy, NetId net, Coord hw) {
    const std::size_t idx = (static_cast<std::size_t>(layer) * static_cast<std::size_t>(w.h) + static_cast<std::size_t>(cy)) * static_cast<std::size_t>(w.w) + static_cast<std::size_t>(cx);
    const int gx = w.x0 + cx, gy = w.y0 + cy;
    if (cstamp[idx] != gen) {
      cstamp[idx] = gen;
      const std::int64_t key = cell_key(layer, gx, gy);
      ++obs->checks;
      cell_state[idx] = static_cast<std::uint8_t>(learned_block.count({net, key}) ? 2 : point_state(layer, gx, gy, net, hw));
    }
    const int st = cell_state[idx];
    if (st == 2) return -1;
    const std::int64_t hc = hist_cost(layer, gx, gy);
    if (st == 1) return static_cast<std::int64_t>(opt.soft_cost_mm * 1e6) + hc * 4;
    if (st == 3) return 3 * pitch + hc;
    return hc;
  }
  std::int64_t via_cost_at(const Window& w, int cx, int cy, NetId net, Coord d, Coord drill) {
    const std::size_t idx = static_cast<std::size_t>(cy) * static_cast<std::size_t>(w.w) + static_cast<std::size_t>(cx);
    if (vstamp[idx] != gen) {
      vstamp[idx] = gen;
      const int gx = w.x0 + cx, gy = w.y0 + cy;
      const Point p = at(gx, gy);
      const Coord margin = pitch * 71 / 100 + 1;
      int st;
      if (!use_cache) {
        st = obs->via_state(p, d, drill, net, margin, soft);
      } else {
        auto& cc = cache_for(net);
        const std::size_t gi = static_cast<std::size_t>(gy) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(gx);
        if (cc.via[gi] == INT32_MIN) cc.via[gi] = obs->fixed_via_code(p, d, drill, margin, cc.rep);
        st = code_ok(cc.via[gi], net) ? 0 : 2;
        for (int l = 0; l < nl && st != 2; ++l) {
          const int r = obs->routed_state(geom::Shape::point(p, d / 2 + margin), l, net, drc::ItemKind::Via, soft, nullptr, true, drill / 2 + margin);
          if (r == 2) st = 2;
          else if (r == 1) st = 1;
        }
      }
      via_state[idx] = static_cast<std::uint8_t>(st);
    }
    if (via_state[idx] == 2) return -1;
    return via_state[idx] == 1 ? static_cast<std::int64_t>(opt.soft_cost_mm * 1e6) : 0;
  }

  // Lattice cells of the window inside a pad's copper on each of its layers (falls back to cells next to the
  // pad centre for pads smaller than the pitch).
  Endpoint pad_cells(const Window& w, int pad) {
    Endpoint e;
    const auto& p = b.pads[static_cast<std::size_t>(pad)];
    const int item = pad_item[static_cast<std::size_t>(pad)];
    if (item < 0) return e;
    const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
    const int x0 = std::max(0, to_ix(it.box.x0) - w.x0), x1 = std::min(w.w - 1, to_ix(it.box.x1) - w.x0);
    const int y0 = std::max(0, to_iy(it.box.y0) - w.y0), y1 = std::min(w.h - 1, to_iy(it.box.y1) - w.y0);
    for (int l = 0; l < nl; ++l) {
      if (!(p.copper & model::layer_bit(l))) continue;
      std::size_t before = e.cells.size();
      for (int cy = y0; cy <= y1; ++cy)
        for (int cx = x0; cx <= x1; ++cx) {
          const geom::Shape pt = geom::Shape::point(at(w.x0 + cx, w.y0 + cy), 0);
          bool in = false;
          for (const auto& s : it.shapes)
            if (geom::closer_than(pt, s, 1)) in = true;
          if (in) e.cells.emplace_back(l, static_cast<std::int64_t>(cy) * w.w + cx);
        }
      if (e.cells.size() == before) {  // tiny pad: nearest lattice point
        const int cx = std::clamp(to_ix(p.pos.x) - w.x0, 0, w.w - 1), cy = std::clamp(to_iy(p.pos.y) - w.y0, 0, w.h - 1);
        e.cells.emplace_back(l, static_cast<std::int64_t>(cy) * w.w + cx);
      }
    }
    return e;
  }

  struct PathNode { int layer, gx, gy; };

  bool search(const Connection& c, const Window& w, std::vector<PathNode>& path) {
    const NetId net = c.net;
    const Coord width = track_width(net);
    const Coord hw = width / 2;
    const Coord vd = via_diameter(net);
    const Coord vdrill = via_drill(net);
    const std::size_t cells = static_cast<std::size_t>(w.w) * static_cast<std::size_t>(w.h);
    const std::size_t states = cells * static_cast<std::size_t>(nl) * 9;
    if (gstamp.size() < states) {
      gstamp.assign(states, 0);
      g.resize(states);
      parent.resize(states);
    }
    if (cstamp.size() < cells * static_cast<std::size_t>(nl)) {
      cstamp.assign(cells * static_cast<std::size_t>(nl), 0);
      cell_state.resize(cells * static_cast<std::size_t>(nl));
    }
    if (vstamp.size() < cells) {
      vstamp.assign(cells, 0);
      via_state.resize(cells);
    }
    if (++gen == 0) {
      std::fill(gstamp.begin(), gstamp.end(), 0u);
      std::fill(cstamp.begin(), cstamp.end(), 0u);
      std::fill(vstamp.begin(), vstamp.end(), 0u);
      gen = 1;
    }
    const Endpoint src = pad_cells(w, c.pad_a);
    const Endpoint dst = c.pad_b >= 0 ? pad_cells(w, c.pad_b) : Endpoint{};
    if (src.cells.empty() || (c.pad_b >= 0 && dst.cells.empty())) return false;
    std::vector<std::uint8_t> is_target(cells * static_cast<std::size_t>(nl), 0);  // 1 target, 2 not (zone checks are lazy)
    for (auto [l, ci] : dst.cells) is_target[static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(ci)] = 1;
    // Zone target: any cell inside the fill on its layer, with room for the track (tested lazily when reached).
    int zone_layer = -1;
    const std::vector<Point>* zone_poly = nullptr;
    if (c.zone_b >= 0) {
      const auto& z = obs->copper().items[static_cast<std::size_t>(c.zone_b)];
      zone_layer = std::countr_zero(z.layers);
      zone_poly = &z.shapes.front().pts;
    }
    auto target = [&](int l, std::int64_t ci) -> bool {
      auto& t = is_target[static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(ci)];
      if (t == 1) return true;
      if (t == 2 || l != zone_layer) return false;
      const int cx = static_cast<int>(ci % w.w), cy = static_cast<int>(ci / w.w);
      const geom::Shape pt = geom::Shape::point(at(w.x0 + cx, w.y0 + cy), hw);
      // Inside the fill and the whole track end disk on copper: no fill edge within hw.
      bool inside = geom::point_in_polygon(pt.pts[0], *zone_poly);
      if (inside) {
        const auto& poly = *zone_poly;
        for (std::size_t i = 0, j = poly.size() - 1; i < poly.size() && inside; j = i++)
          if (geom::point_seg_closer(pt.pts[0], poly[j], poly[i], hw + pitch)) inside = false;
      }
      t = inside ? 1 : 2;
      return inside;
    };
    const Point tp = c.pad_b >= 0 ? b.pads[static_cast<std::size_t>(c.pad_b)].pos : b.pads[static_cast<std::size_t>(c.pad_a)].pos;
    const bool zone_target = c.zone_b >= 0;
    const std::int64_t step = pitch, diag = static_cast<std::int64_t>(std::llround(static_cast<double>(pitch) * std::numbers::sqrt2));
    const std::int64_t via_cost = static_cast<std::int64_t>(opt.via_cost_mm * 1e6);
    auto h = [&](int gx, int gy) -> std::int64_t {
      const Point p = at(gx, gy);
      if (zone_target) return std::int64_t{0};  // plane anywhere nearby: no useful lower bound
      const std::int64_t dx = std::llabs(p.x - tp.x), dy = std::llabs(p.y - tp.y);
      const std::int64_t mn = std::min(dx, dy), mx = std::max(dx, dy);
      return static_cast<std::int64_t>(static_cast<double>((mx - mn) + mn * diag / step) * opt.heuristic_weight);  // octile distance
    };
    auto sidx = [&](int l, std::int64_t ci, int dir) {
      return ((static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(ci)) * 9) + static_cast<std::size_t>(dir);
    };
    using QE = std::pair<std::int64_t, std::size_t>;  // (f, state)
    std::priority_queue<QE, std::vector<QE>, std::greater<>> open;
    const Point sp = b.pads[static_cast<std::size_t>(c.pad_a)].pos;
    for (auto [l, ci] : src.cells) {
      const int cx = static_cast<int>(ci % w.w), cy = static_cast<int>(ci / w.w);
      const Point p = at(w.x0 + cx, w.y0 + cy);
      const std::int64_t g0 = static_cast<std::int64_t>(std::hypot(static_cast<double>(p.x - sp.x), static_cast<double>(p.y - sp.y)));
      const std::size_t s = sidx(l, ci, kNoDir);
      gstamp[s] = gen;
      g[s] = g0;
      parent[s] = -1;
      open.emplace(g0 + h(w.x0 + cx, w.y0 + cy), s);
    }
    long expanded = 0;
    std::size_t goal = SIZE_MAX;
    while (!open.empty()) {
      const auto [f, s] = open.top();
      open.pop();
      const int dir = static_cast<int>(s % 9);
      const std::size_t lc = s / 9;
      const int l = static_cast<int>(lc / cells);
      const std::int64_t ci = static_cast<std::int64_t>(lc % cells);
      const int cx = static_cast<int>(ci % w.w), cy = static_cast<int>(ci / w.w);
      if (f - h(w.x0 + cx, w.y0 + cy) > g[s]) continue;  // stale entry
      if (target(l, ci)) {
        goal = s;
        break;
      }
      if (++expanded > opt.max_expansions) break;
      const std::int64_t gs = g[s];
      // Planar moves.
      for (int d = 0; d < 8; ++d) {
        if (dir != kNoDir) {
          const int turn = std::min((d - dir + 8) % 8, (dir - d + 8) % 8);
          if (turn >= 3) continue;  // no 135°/180° turns
        }
        const int ncx = cx + kDx[d], ncy = cy + kDy[d];
        if (ncx < 0 || ncy < 0 || ncx >= w.w || ncy >= w.h) continue;
        const std::int64_t nci = static_cast<std::int64_t>(ncy) * w.w + ncx;
        const bool tgt = is_target[static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(nci)] == 1;
        std::int64_t extra = 0;
        if (!tgt) {
          extra = cell_cost(w, l, ncx, ncy, net, hw);
          if (extra < 0) continue;
        }
        std::int64_t cost = ((d & 1) ? diag : step) + extra;
        if (dir != kNoDir && d != dir) cost += ((std::min((d - dir + 8) % 8, (dir - d + 8) % 8) == 1) ? step / 2 : 2 * step);
        const std::size_t ns = sidx(l, nci, d);
        if (gstamp[ns] == gen && g[ns] <= gs + cost) continue;
        gstamp[ns] = gen;
        g[ns] = gs + cost;
        parent[ns] = static_cast<std::int32_t>(s);
        open.emplace(gs + cost + h(w.x0 + ncx, w.y0 + ncy), ns);
      }
      // Via: change to every other layer at this cell (through via).
      const std::int64_t vextra = (opt.allow_vias && nl > 1) ? via_cost_at(w, cx, cy, net, vd, vdrill) : -1;
      if (vextra >= 0) {
        for (int l2 = 0; l2 < nl; ++l2) {
          if (l2 == l) continue;
          const std::size_t ns = sidx(l2, ci, kNoDir);
          const std::int64_t ng = gs + via_cost + vextra;
          if (gstamp[ns] == gen && g[ns] <= ng) continue;
          gstamp[ns] = gen;
          g[ns] = ng;
          parent[ns] = static_cast<std::int32_t>(s);
          open.emplace(ng + h(w.x0 + cx, w.y0 + cy), ns);
        }
      }
    }
    res.expansions += expanded;
    if (goal == SIZE_MAX) return false;
    path.clear();
    for (std::int64_t s = static_cast<std::int64_t>(goal); s >= 0; s = parent[static_cast<std::size_t>(s)]) {
      const std::size_t lc = static_cast<std::size_t>(s) / 9;
      const int l = static_cast<int>(lc / cells);
      const std::int64_t ci = static_cast<std::int64_t>(lc % cells);
      path.push_back({l, w.x0 + static_cast<int>(ci % w.w), w.y0 + static_cast<int>(ci / w.w)});
      if (parent[static_cast<std::size_t>(s)] < 0) break;
    }
    std::reverse(path.begin(), path.end());
    return true;
  }

  // Turns a lattice path into tracks and vias, verifies them exactly, and commits them. Returns false (and
  // learns blocked cells) when the exact check fails.
  bool commit(const Connection& c, const std::vector<PathNode>& path) {
    const NetId net = c.net;
    const Coord width = track_width(net);
    const Coord vd = via_diameter(net);
    const Coord vdrill = via_drill(net);
    struct Seg { Point a, b; int layer; };
    std::vector<Seg> segs;
    std::vector<Point> vias;
    const Point pa = b.pads[static_cast<std::size_t>(c.pad_a)].pos;
    const Point pb = c.pad_b >= 0 ? b.pads[static_cast<std::size_t>(c.pad_b)].pos : at(path.back().gx, path.back().gy);
    // Corner points: pad centre, direction changes and layer changes, pad centre.
    Point cur = pa;
    int layer = path.front().layer;
    auto P = [&](const PathNode& n) { return at(n.gx, n.gy); };
    for (std::size_t i = 0; i < path.size(); ++i) {
      const Point p = P(path[i]);
      if (path[i].layer != layer) {  // via at the previous point
        if (!(cur == p)) segs.push_back({cur, p, layer});
        vias.push_back(p);
        cur = p;
        layer = path[i].layer;
        continue;
      }
      const bool last = i + 1 == path.size();
      if (!last && path[i + 1].layer == layer && i > 0 && path[i - 1].layer == layer) {
        const Point prev = P(path[i - 1]), next = P(path[i + 1]);
        const Coord dx1 = p.x - prev.x, dy1 = p.y - prev.y, dx2 = next.x - p.x, dy2 = next.y - p.y;
        if (dx1 * dy2 == dy1 * dx2 && (dx1 * dx2 + dy1 * dy2) > 0) continue;  // collinear: no corner here
      }
      if (!(cur == p)) segs.push_back({cur, p, layer});
      cur = p;
    }
    if (!(cur == pb)) segs.push_back({cur, pb, layer});
    // Merge collinear consecutive segments on the same layer (after adding the pad legs).
    std::vector<Seg> merged;
    for (const auto& s : segs) {
      if (!merged.empty() && merged.back().layer == s.layer && merged.back().b == s.a) {
        const Point a = merged.back().a, m = merged.back().b, e = s.b;
        if (geom::orient(a, m, e) == 0 && ((m.x - a.x) * (e.x - m.x) + (m.y - a.y) * (e.y - m.y)) > 0) {
          merged.back().b = e;
          continue;
        }
      }
      merged.push_back(s);
    }
    // Exact verification. In soft mode, conflicts with other connections' routed copper name the victims.
    bool ok = true;
    std::vector<int> victims;
    for (const auto& s : merged) {
      const int st = obs->segment_state(s.a, s.b, s.layer, width, net, soft, &victims);
      if (st == 2) {
        ok = false;
        learn_block(c, s, width);
      }
    }
    for (const auto& v : vias)
      if (obs->via_state(v, vd, vdrill, net, 0, soft, &victims) == 2) ok = false;
    if (!ok) return false;
    std::sort(victims.begin(), victims.end());
    victims.erase(std::unique(victims.begin(), victims.end()), victims.end());
    for (int v : victims)
      if (cs[static_cast<std::size_t>(v)].rips >= opt.max_rips_per_connection) return false;  // protected: too often ripped
    // Rip up victims, and raise history on the contested cells so later searches avoid them.
    for (int v : victims) rip(v);
    for (const auto& s : merged) bump_history(s, net);
    // Commit.
    auto& st = cs[static_cast<std::size_t>(current)];
    for (const auto& s : merged) {
      model::Track t{s.a, s.b, width, s.layer, net, false, sexpr::kNoNode};
      b.tracks.push_back(t);
      const int id = static_cast<int>(b.tracks.size() - 1);
      st.items.push_back(obs->add_track(id, current));
      if (opt.sink)
        emit("{\"type\":\"track_add\",\"track\":{\"id\":" + std::to_string(id) + ",\"a\":[" + jnum(t.a.x) + "," + jnum(t.a.y) + "],\"b\":[" +
             jnum(t.b.x) + "," + jnum(t.b.y) + "],\"w\":" + jnum(t.width) + ",\"layer\":" + std::to_string(t.layer) + ",\"net\":" +
             std::to_string(t.net) + "}}");
    }
    for (const auto& p : vias) {
      model::Via v{p, vd, vdrill, 0, nl - 1, model::ViaType::Through, net, false, sexpr::kNoNode};
      b.vias.push_back(v);
      const int id = static_cast<int>(b.vias.size() - 1);
      st.items.push_back(obs->add_via(id, current));
      if (opt.sink)
        emit("{\"type\":\"via_add\",\"via\":{\"id\":" + std::to_string(id) + ",\"p\":[" + jnum(p.x) + "," + jnum(p.y) + "],\"d\":" + jnum(vd) +
             ",\"drill\":" + jnum(vdrill) + ",\"net\":" + std::to_string(net) + ",\"top\":0,\"bottom\":" + std::to_string(nl - 1) + "}}");
    }
    return true;
  }

  void learn_block(const Connection& c, const auto& s, Coord width) {
    const int n = std::max<int>(1, static_cast<int>(std::hypot(static_cast<double>(s.b.x - s.a.x), static_cast<double>(s.b.y - s.a.y)) / static_cast<double>(pitch)));
    const auto& ia = obs->copper().items[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(c.pad_a)])].box;
    const auto& ib = c.pad_b >= 0 ? obs->copper().items[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(c.pad_b)])].box : ia;
    for (int k = 0; k <= n; ++k) {
      const Point q{s.a.x + (s.b.x - s.a.x) * k / n, s.a.y + (s.b.y - s.a.y) * k / n};
      const geom::Box qb = geom::Shape::point(q, 0).box;
      if (qb.intersects(ia) || qb.intersects(ib)) continue;  // never block the pads themselves
      if (obs->disk_state(q, s.layer, width / 2, c.net, 0, soft) == 2) learned_block.insert({c.net, cell_key(s.layer, to_ix(q.x), to_iy(q.y))});
    }
  }

  void bump_history(const auto& s, NetId net) {
    if (!soft) return;
    const int n = std::max<int>(1, static_cast<int>(std::hypot(static_cast<double>(s.b.x - s.a.x), static_cast<double>(s.b.y - s.a.y)) / static_cast<double>(pitch)));
    for (int k = 0; k <= n; ++k) {
      const Point q{s.a.x + (s.b.x - s.a.x) * k / n, s.a.y + (s.b.y - s.a.y) * k / n};
      std::vector<int> owners;
      if (obs->disk_state(q, s.layer, track_width(net) / 2, net, 0, true, &owners) == 1) {
        auto& h = history[cell_key(s.layer, to_ix(q.x), to_iy(q.y))];
        if (h < 60000) h = static_cast<std::uint16_t>(h + 1);
      }
    }
  }

  // Removes a connection's copper and marks it (and connections of its net satisfied implicitly) unrouted.
  void rip(int v) {
    auto& st = cs[static_cast<std::size_t>(v)];
    for (int item : st.items) {
      const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
      if (opt.sink) emit(std::string("{\"type\":\"") + (it.kind == drc::ItemKind::Via ? "via_remove" : "track_remove") + "\",\"id\":" + std::to_string(it.index) + "}");
      obs->remove_item(item);
    }
    st.items.clear();
    if (st.routed) --res.routed;
    st.routed = false;
    ++st.rips;
    ++res.rips;
    pending.push_back(v);
    for (std::size_t k = 0; k < cs.size(); ++k)
      if (cs[k].implicit && cs[k].routed && cs[k].c.net == st.c.net) {
        cs[k].routed = cs[k].implicit = false;
        --res.routed;
        pending.push_back(static_cast<int>(k));
      }
  }

  bool search_and_commit(const Connection& c, bool soft_mode) {
    soft = soft_mode;
    const Point a = b.pads[static_cast<std::size_t>(c.pad_a)].pos;
    const Point e = c.pad_b >= 0 ? b.pads[static_cast<std::size_t>(c.pad_b)].pos : a;
    static const Coord margins[] = {2'000'000, 6'000'000, 20'000'000, 1'000'000'000};
    std::vector<PathNode> path;
    for (int attempt = 0; attempt < opt.max_attempts; ++attempt) {
      if (elapsed() > opt.time_limit_s) return false;
      const Coord m = margins[std::min(attempt, 3)] + c.length / 4;
      Window w;
      w.x0 = std::max(0, to_ix(std::min(a.x, e.x) - m));
      w.y0 = std::max(0, to_iy(std::min(a.y, e.y) - m));
      const int x1 = std::min(nx - 1, to_ix(std::max(a.x, e.x) + m)), y1 = std::min(ny - 1, to_iy(std::max(a.y, e.y) + m));
      w.w = x1 - w.x0 + 1;
      w.h = y1 - w.y0 + 1;
      if (!search(c, w, path)) continue;
      if (commit(c, path)) return true;
    }
    return false;
  }

  // Are the connection's pads already joined through fixed copper and other routed connections of its net?
  bool joined(int ci) {
    const auto& c = cs[static_cast<std::size_t>(ci)].c;
    const int ra = init_root[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(c.pad_a)])];
    const int rb = c.pad_b >= 0 ? init_root[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(c.pad_b)])] : init_root[static_cast<std::size_t>(c.zone_b)];
    if (ra == rb) return true;
    std::map<int, int> up;
    auto find = [&](int x) {
      while (up.count(x) && up[x] != x) x = up[x];
      return x;
    };
    for (const auto& o : cs) {
      if (!o.routed || o.implicit || o.c.net != c.net) continue;
      const int x = find(init_root[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(o.c.pad_a)])]);
      const int y = find(o.c.pad_b >= 0 ? init_root[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(o.c.pad_b)])] : init_root[static_cast<std::size_t>(o.c.zone_b)]);
      if (x != y) up[x] = y;
    }
    return find(ra) == find(rb);
  }

  std::deque<int> pending;

  RouteResult run() {
    t0 = std::chrono::steady_clock::now();
    setup();
    emit("{\"type\":\"stage\",\"name\":\"route\",\"state\":\"begin\",\"detail\":\"lattice A* with negotiated rip-up\"}");
    auto con = drc::compute_connectivity(b, obs->copper(), obs->grid());
    init_root = con.root;
    drc::UnionFind uf(obs->copper().items.size());
    for (std::size_t i = 0; i < con.root.size(); ++i) uf.unite(static_cast<int>(i), con.root[i]);
    const auto conns = plan(uf);
    res.connections = static_cast<int>(conns.size());
    for (const auto& c : conns) {
      ConnState st;
      st.c = c;
      cs.push_back(std::move(st));
    }
    for (std::size_t i = 0; i < cs.size(); ++i) pending.push_back(static_cast<int>(i));

    // Best legal state seen (most connections routed): connection -> its tracks/vias.
    int best_routed = -1;
    std::vector<model::Track> best_tracks;
    std::vector<model::Via> best_vias;
    auto snapshot = [&]() {
      best_routed = res.routed;
      best_tracks.clear();
      best_vias.clear();
      for (const auto& st : cs)
        for (int item : st.items) {
          const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
          if (it.kind == drc::ItemKind::Via) best_vias.push_back(b.vias[static_cast<std::size_t>(it.index)]);
          else best_tracks.push_back(b.tracks[static_cast<std::size_t>(it.index)]);
        }
    };

    for (int pass = 0; pass < opt.max_passes && !pending.empty() && elapsed() < opt.time_limit_s; ++pass) {
      res.passes = pass + 1;
      std::vector<int> failed;
      const int routed_before = res.routed;
      while (!pending.empty() && elapsed() < opt.time_limit_s) {
        const int ci = pending.front();
        pending.pop_front();
        auto& st = cs[static_cast<std::size_t>(ci)];
        if (st.routed) continue;
        current = ci;
        if (joined(ci)) {
          st.routed = st.implicit = true;
          ++res.routed;
          continue;
        }
        bool ok = search_and_commit(st.c, false);
        if (!ok && opt.rip_up && pass > 0) ok = search_and_commit(st.c, true);  // pass 0: strict; later: negotiate
        if (ok) {
          cs[static_cast<std::size_t>(ci)].routed = true;
          ++res.routed;
        } else {
          ++cs[static_cast<std::size_t>(ci)].fails;
          failed.push_back(ci);
        }
        emit_stats("route");
        if (res.routed > best_routed) snapshot();
      }
      // Next pass: hardest (most failed) first.
      std::stable_sort(failed.begin(), failed.end(), [&](int x, int y) { return cs[static_cast<std::size_t>(x)].fails > cs[static_cast<std::size_t>(y)].fails; });
      for (int f : failed) pending.push_back(f);
      if (pass > 0 && res.routed <= routed_before && res.rips == 0) break;
    }
    if (res.routed > best_routed) snapshot();
    res.routed = best_routed;
    res.tracks = std::move(best_tracks);
    res.vias = std::move(best_vias);
    // Failures relative to the best state are approximated by the connections unrouted at the end.
    for (const auto& st : cs) {
      if (st.routed) continue;
      const auto& pa = b.pads[static_cast<std::size_t>(st.c.pad_a)];
      std::string to = "zone";
      if (st.c.pad_b >= 0) {
        const auto& pb = b.pads[static_cast<std::size_t>(st.c.pad_b)];
        to = b.footprints[static_cast<std::size_t>(pb.footprint)].reference + "." + pb.number;
      }
      res.failures.push_back(b.nets[static_cast<std::size_t>(st.c.net)].name + ": " + b.footprints[static_cast<std::size_t>(pa.footprint)].reference + "." +
                             pa.number + " -> " + to);
    }
    res.seconds = elapsed();
    std::fprintf(stderr, "legality checks %ld: outside %ld, copper %ld, holes/edges/keepouts %ld; rips %d, passes %d\n", obs->checks,
                 obs->rej_outside, obs->rej_copper, obs->rej_other, res.rips, res.passes);
    emit_stats("done");
    emit("{\"type\":\"stage\",\"name\":\"route\",\"state\":\"end\",\"detail\":\"\"}");
    return std::move(res);
  }
};

Router::Router(const model::Board& board, const model::DesignRules& rules, RouterOptions opt) : in_(board), rules_(rules), opt_(opt) {}

RouteResult Router::run() {
  Impl impl(in_, rules_, opt_);
  return impl.run();
}

}  // namespace tmk::route

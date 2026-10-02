#include "route/router.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <queue>
#include <set>

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
  Coord track_width(NetId net) const { return std::max(netclass(net).track_width, rules.minimums.track_width); }

  // Lattice <-> board coordinates.
  Point at(int ix, int iy) const { return {lat.x0 + static_cast<Coord>(ix) * pitch, lat.y0 + static_cast<Coord>(iy) * pitch}; }
  int to_ix(Coord x) const { return static_cast<int>(std::llround(static_cast<double>(x - lat.x0) / static_cast<double>(pitch))); }
  int to_iy(Coord y) const { return static_cast<int>(std::llround(static_cast<double>(y - lat.y0) / static_cast<double>(pitch))); }

  void setup() {
    obs = std::make_unique<Obstacles>(b, rules);
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
  std::vector<Connection> plan(drc::UnionFind& uf) {
    const auto& cm = obs->copper();
    std::map<NetId, std::map<int, std::vector<int>>> net_clusters;  // net -> root -> pads
    pad_item.assign(b.pads.size(), -1);
    for (std::size_t i = 0; i < cm.items.size(); ++i) {
      const auto& it = cm.items[i];
      if (it.kind != drc::ItemKind::Pad || it.net == 0) continue;
      pad_item[static_cast<std::size_t>(it.index)] = static_cast<int>(i);
      net_clusters[it.net][uf.find(static_cast<int>(i))].push_back(it.index);
    }
    std::vector<Connection> out;
    for (auto& [net, cl] : net_clusters) {
      if (cl.size() < 2) continue;
      std::vector<std::vector<int>> groups;
      for (auto& [r, pads] : cl) groups.push_back(pads);
      // Prim over clusters with pad-to-pad distances.
      const std::size_t k = groups.size();
      std::vector<std::uint8_t> in(k, 0);
      std::vector<double> best(k, 1e300);
      std::vector<std::pair<int, int>> via(k, {-1, -1});
      in[0] = 1;
      auto upd = [&](std::size_t from) {
        for (std::size_t j = 0; j < k; ++j) {
          if (in[j]) continue;
          for (int pa : groups[from])
            for (int pb : groups[j]) {
              const auto& A = b.pads[static_cast<std::size_t>(pa)].pos;
              const auto& B = b.pads[static_cast<std::size_t>(pb)].pos;
              const double d = std::hypot(static_cast<double>(A.x - B.x), static_cast<double>(A.y - B.y));
              if (d < best[j]) {
                best[j] = d;
                via[j] = {pa, pb};
              }
            }
        }
      };
      upd(0);
      for (std::size_t step = 1; step < k; ++step) {
        std::size_t nxt = k;
        for (std::size_t j = 0; j < k; ++j)
          if (!in[j] && (nxt == k || best[j] < best[nxt])) nxt = j;
        in[nxt] = 1;
        out.push_back({net, via[nxt].first, via[nxt].second, static_cast<Coord>(best[nxt])});
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

  bool cell_free(const Window& w, int layer, int cx, int cy, NetId net, Coord hw) {
    const std::size_t idx = (static_cast<std::size_t>(layer) * static_cast<std::size_t>(w.h) + static_cast<std::size_t>(cy)) * static_cast<std::size_t>(w.w) + static_cast<std::size_t>(cx);
    if (cstamp[idx] == gen) return cell_state[idx] == 1;
    cstamp[idx] = gen;
    const int gx = w.x0 + cx, gy = w.y0 + cy;
    const std::int64_t key = (static_cast<std::int64_t>(layer) << 48) | (static_cast<std::int64_t>(gy) << 24) | gx;
    bool ok = !learned_block.count({net, key}) && obs->disk_ok(at(gx, gy), layer, hw, net, pitch * 71 / 100 + 1);
    cell_state[idx] = ok ? 1 : 2;
    return ok;
  }
  bool via_free(const Window& w, int cx, int cy, NetId net, Coord d, Coord drill) {
    const std::size_t idx = static_cast<std::size_t>(cy) * static_cast<std::size_t>(w.w) + static_cast<std::size_t>(cx);
    if (vstamp[idx] == gen) return via_state[idx] == 1;
    vstamp[idx] = gen;
    const bool ok = obs->via_ok(at(w.x0 + cx, w.y0 + cy), d, drill, net, pitch * 71 / 100 + 1);
    via_state[idx] = ok ? 1 : 2;
    return ok;
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
    const auto& nc = netclass(net);
    const Coord vd = std::max(nc.via_diameter, rules.minimums.via_diameter);
    const Coord vdrill = std::max(nc.via_drill, rules.minimums.through_hole_diameter);
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
    const Endpoint src = pad_cells(w, c.pad_a), dst = pad_cells(w, c.pad_b);
    if (src.cells.empty() || dst.cells.empty()) return false;
    std::vector<std::uint32_t> is_target(cells * static_cast<std::size_t>(nl), 0);  // small; window-local
    for (auto [l, ci] : dst.cells) is_target[static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(ci)] = 1;
    const Point tp = b.pads[static_cast<std::size_t>(c.pad_b)].pos;
    const std::int64_t step = pitch, diag = static_cast<std::int64_t>(std::llround(static_cast<double>(pitch) * std::numbers::sqrt2));
    const std::int64_t via_cost = static_cast<std::int64_t>(opt.via_cost_mm * 1e6);
    auto h = [&](int gx, int gy) -> std::int64_t {
      const Point p = at(gx, gy);
      const std::int64_t dx = std::llabs(p.x - tp.x), dy = std::llabs(p.y - tp.y);
      const std::int64_t mn = std::min(dx, dy), mx = std::max(dx, dy);
      return (mx - mn) + mn * diag / step;  // octile distance in nm (admissible)
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
      if (is_target[lc]) {
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
        const bool tgt = is_target[static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(nci)] != 0;
        if (!tgt && !cell_free(w, l, ncx, ncy, net, hw)) continue;
        std::int64_t cost = (d & 1) ? diag : step;
        if (dir != kNoDir && d != dir) cost += ((std::min((d - dir + 8) % 8, (dir - d + 8) % 8) == 1) ? step / 2 : 2 * step);
        const std::size_t ns = sidx(l, nci, d);
        if (gstamp[ns] == gen && g[ns] <= gs + cost) continue;
        gstamp[ns] = gen;
        g[ns] = gs + cost;
        parent[ns] = static_cast<std::int32_t>(s);
        open.emplace(gs + cost + h(w.x0 + ncx, w.y0 + ncy), ns);
      }
      // Via: change to every other layer at this cell (through via).
      if (opt.allow_vias && nl > 1 && via_free(w, cx, cy, net, vd, vdrill)) {
        for (int l2 = 0; l2 < nl; ++l2) {
          if (l2 == l) continue;
          const std::size_t ns = sidx(l2, ci, kNoDir);
          const std::int64_t ng = gs + via_cost;
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
    const auto& nc = netclass(net);
    const Coord vd = std::max(nc.via_diameter, rules.minimums.via_diameter);
    const Coord vdrill = std::max(nc.via_drill, rules.minimums.through_hole_diameter);
    struct Seg { Point a, b; int layer; };
    std::vector<Seg> segs;
    std::vector<Point> vias;
    const Point pa = b.pads[static_cast<std::size_t>(c.pad_a)].pos, pb = b.pads[static_cast<std::size_t>(c.pad_b)].pos;
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
    // Exact verification.
    bool ok = true;
    for (const auto& s : merged)
      if (!obs->segment_ok(s.a, s.b, s.layer, width, net)) {
        ok = false;
        // Learn: block the lattice cells along the failing segment for this net.
        const int n = std::max<int>(1, static_cast<int>(std::hypot(static_cast<double>(s.b.x - s.a.x), static_cast<double>(s.b.y - s.a.y)) / static_cast<double>(pitch)));
        for (int k = 0; k <= n; ++k) {
          const Point q{s.a.x + (s.b.x - s.a.x) * k / n, s.a.y + (s.b.y - s.a.y) * k / n};
          if (geom::Shape::point(q, 0).box.intersects(obs->copper().items[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(c.pad_a)])].box) ||
              geom::Shape::point(q, 0).box.intersects(obs->copper().items[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(c.pad_b)])].box))
            continue;  // never block the pads themselves
          if (!obs->disk_ok(q, s.layer, width / 2, net, 0)) {
            const std::int64_t key = (static_cast<std::int64_t>(s.layer) << 48) | (static_cast<std::int64_t>(to_iy(q.y)) << 24) | to_ix(q.x);
            learned_block.insert({net, key});
          }
        }
      }
    for (const auto& v : vias)
      if (!obs->via_ok(v, vd, vdrill, net, 0)) ok = false;
    if (!ok) return false;
    // Commit.
    for (const auto& s : merged) {
      model::Track t{s.a, s.b, width, s.layer, net, false, sexpr::kNoNode};
      b.tracks.push_back(t);
      obs->add_track(static_cast<int>(b.tracks.size() - 1));
      res.tracks.push_back(t);
      if (opt.sink) {
        const int id = static_cast<int>(b.tracks.size() - 1);
        emit("{\"type\":\"track_add\",\"track\":{\"id\":" + std::to_string(id) + ",\"a\":[" + jnum(t.a.x) + "," + jnum(t.a.y) + "],\"b\":[" +
             jnum(t.b.x) + "," + jnum(t.b.y) + "],\"w\":" + jnum(t.width) + ",\"layer\":" + std::to_string(t.layer) + ",\"net\":" +
             std::to_string(t.net) + "}}");
      }
    }
    for (const auto& p : vias) {
      model::Via v{p, vd, vdrill, 0, nl - 1, model::ViaType::Through, net, false, sexpr::kNoNode};
      b.vias.push_back(v);
      obs->add_via(static_cast<int>(b.vias.size() - 1));
      res.vias.push_back(v);
      if (opt.sink) {
        const int id = static_cast<int>(b.vias.size() - 1);
        emit("{\"type\":\"via_add\",\"via\":{\"id\":" + std::to_string(id) + ",\"p\":[" + jnum(p.x) + "," + jnum(p.y) + "],\"d\":" + jnum(vd) +
             ",\"drill\":" + jnum(vdrill) + ",\"net\":" + std::to_string(net) + ",\"top\":0,\"bottom\":" + std::to_string(nl - 1) + "}}");
      }
    }
    return true;
  }

  bool route_one(const Connection& c) {
    const Point a = b.pads[static_cast<std::size_t>(c.pad_a)].pos, e = b.pads[static_cast<std::size_t>(c.pad_b)].pos;
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

  RouteResult run() {
    t0 = std::chrono::steady_clock::now();
    setup();
    emit("{\"type\":\"stage\",\"name\":\"route\",\"state\":\"begin\",\"detail\":\"lattice A*\"}");
    auto con = drc::compute_connectivity(b, obs->copper(), obs->grid());
    drc::UnionFind uf(obs->copper().items.size());
    for (std::size_t i = 0; i < con.root.size(); ++i) uf.unite(static_cast<int>(i), con.root[i]);
    const auto conns = plan(uf);
    res.connections = static_cast<int>(conns.size());
    // Pads joined during routing: track with a separate union-find on pad items.
    for (const auto& c : conns) {
      const int ia = pad_item[static_cast<std::size_t>(c.pad_a)], ib = pad_item[static_cast<std::size_t>(c.pad_b)];
      if (uf.find(ia) == uf.find(ib)) {  // already joined by an earlier route through a shared cluster
        ++res.routed;
        continue;
      }
      if (route_one(c)) {
        uf.unite(ia, ib);
        ++res.routed;
      } else {
        const auto& pa = b.pads[static_cast<std::size_t>(c.pad_a)];
        const auto& pb = b.pads[static_cast<std::size_t>(c.pad_b)];
        res.failures.push_back(b.nets[static_cast<std::size_t>(c.net)].name + ": " + b.footprints[static_cast<std::size_t>(pa.footprint)].reference + "." + pa.number +
                               " -> " + b.footprints[static_cast<std::size_t>(pb.footprint)].reference + "." + pb.number);
        if (opt.sink)
          emit("{\"type\":\"failure\",\"conn\":0,\"net\":" + std::to_string(c.net) + ",\"rung\":0,\"cause\":\"no path\",\"a\":[" + jnum(pa.pos.x) + "," +
               jnum(pa.pos.y) + "],\"b\":[" + jnum(pb.pos.x) + "," + jnum(pb.pos.y) + "],\"blockers\":[],\"region\":[0,0,0,0]}");
      }
      emit_stats("route");
    }
    res.seconds = elapsed();
    std::fprintf(stderr, "legality checks %ld: outside %ld, copper %ld, holes/edges/keepouts %ld; outline points %zu\n", obs->checks,
                 obs->rej_outside, obs->rej_copper, obs->rej_other, obs->outline().size());
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

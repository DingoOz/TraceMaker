#include "route/router.hpp"
#include "route/escape.hpp"
#include "route/global_router.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <queue>
#include <array>
#include <deque>
#include <set>
#include <thread>
#include <unordered_map>

#include "core/rng.hpp"
#include "drc/connectivity.hpp"
#include "gpu/device.hpp"
#include "gpu/field.hpp"
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
  // One 16-byte record per search state (cost, generation tag with the arrival direction in the top 4 bits,
  // parent), so an expansion touches one cache line instead of four.
  struct SNode {
    std::int64_t g;
    std::uint32_t tag;
    std::int32_t parent;
  };
  static constexpr std::uint32_t kGenMask = 0x0FFFFFFFu;
  std::vector<SNode> sn;
  std::vector<std::uint32_t> cstamp, vstamp;
  std::vector<std::uint8_t> cell_state, via_state;  // 0 unknown, 1 free, 2 blocked (valid when stamp matches)
  std::uint32_t gen = 0;
  // (net and width, layer-cell key) blocked by FIXED copper after exact-check failures (routed copper changes,
  // so conflicts with it are not learned permanently).
  std::set<std::pair<std::int64_t, std::int64_t>> learned_block;
  std::int64_t block_owner(NetId net) const { return static_cast<std::int64_t>(net) * 10'000'000 + track_width(net) / 1000; }

  // ---- negotiation state ----
  struct ConnState {
    Connection c;
    bool routed = false;
    bool implicit = false;           // satisfied through other routes of the same net (no own copper)
    std::vector<int> items;          // copper item indices owned by this connection
    int rips = 0, fails = 0;
    std::string why;                 // last failure explanation
    bool coupled = false;            // routed as half of a differential pair: clean-up leaves it alone
    // Boxed in even by a negotiated search, which may cross every other net's routed copper: only fixed copper
    // encloses the pin (M9). Retrying it in later passes and restarts only spends budget others could use.
    bool dead = false;
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
  std::map<std::pair<const model::NetClass*, Coord>, ClassCache> caches;  // per (net class, track width)
  bool use_cache = true;
  int current = -1;                  // connection being routed
  std::vector<std::int64_t> soft_cells;  // cells of the last soft path that crossed routed copper

  // ---- escape planning (M9, route/escape.hpp) ----
  // Lattice cell -> net it is reserved for (0 none, -1 contested by two plans: reserved for nobody). Another
  // net may not enter a reserved cell in a strict search and pays reserve_pen in a negotiated one.
  std::vector<std::int32_t> reserve;
  std::vector<std::vector<std::size_t>> pad_reserved;  // board pad -> cells it reserved
  std::vector<int> pad_open;                           // board pad -> its connections not yet routed
  std::int64_t reserve_pen = 0;
  std::size_t lat_index(int layer, int gx, int gy) const {
    return (static_cast<std::size_t>(layer) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(gy)) * static_cast<std::size_t>(nx) +
           static_cast<std::size_t>(gx);
  }
  // Extra cost (or -1: blocked) of using a cell reserved for another net.
  std::int64_t reserved_cost(int layer, int gx, int gy, NetId net) const {
    if (reserve.empty()) return 0;
    const std::int32_t r = reserve[lat_index(layer, gx, gy)];
    if (r <= 0 || r == static_cast<std::int32_t>(net)) return 0;
    return soft ? reserve_pen : -1;
  }

  Impl(const model::Board& in, const model::DesignRules& r, const RouterOptions& o) : rules(r), opt(o), b(in) {}

  double elapsed() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
  // Out of budget? The work budget (search expansions) is deterministic; wall time is only a safety net.
  bool out_of_budget() const {
    if (opt.deadline && res.routed < res.connections && elapsed() > opt.deadline->load(std::memory_order_relaxed)) return true;
    if (opt.work_budget > 0 && res.expansions >= opt.work_budget) return true;
    return elapsed() > opt.time_limit_s;
  }
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
  Coord width_override = 0;   // > 0: neck-down width for the current attempt (escalation rung)
  bool force_escapes = false; // escalation rung: off-lattice escapes even when the pad has lattice exits
  Coord class_width(NetId net) const { return std::max(netclass(net).track_width, rules.minimums.track_width); }
  Coord track_width(NetId net) const { return width_override > 0 ? width_override : class_width(net); }
  // Narrowest legal width to fall back to: the board minimum (KiCad's track_width rule), but not below 0.15 mm
  // unless the board minimum itself is smaller and non-zero.
  Coord neck_width(NetId net) const {
    const Coord mn = rules.minimums.track_width;
    const Coord w = mn > 0 ? std::max(mn, std::min<Coord>(class_width(net), 150'000)) : std::min<Coord>(class_width(net), 150'000);
    return w < class_width(net) ? w : 0;
  }

  // Lattice <-> board coordinates.
  Point at(int ix, int iy) const { return {lat.x0 + static_cast<Coord>(ix) * pitch, lat.y0 + static_cast<Coord>(iy) * pitch}; }
  int to_ix(Coord x) const { return static_cast<int>(std::llround(static_cast<double>(x - lat.x0) / static_cast<double>(pitch))); }
  int to_iy(Coord y) const { return static_cast<int>(std::llround(static_cast<double>(y - lat.y0) / static_cast<double>(pitch))); }

  void setup() {
    obs = std::make_unique<Obstacles>(b, rules);
    use_cache = !obs->has_custom_rules();
    nl = b.copper_count();
    blind_ok = opt.blind_vias && rules.minimums.allow_blind_buried_vias && nl > 2;
    // Pitch: a fraction of the smallest (width + clearance) so lattice tracks can pass between fine-pitch pads.
    if (opt.pitch > 0) {
      pitch = opt.pitch;
    } else {
      Coord wc = 1'000'000'000;
      for (const auto& c : rules.classes) wc = std::min(wc, std::max(c.track_width, rules.minimums.track_width) + std::max(c.clearance, rules.minimums.clearance));
      pitch = std::clamp<Coord>(wc / 6 / 5'000 * 5'000, 25'000, 100'000);
      // Large boards are time-limited at the fine pitch: a coarser lattice finishes more passes (P8000: 335
      // instead of 325 of 361 connections in 120 s). Escape stubs still reach fine-pitch pads.
      const auto bb0 = obs->bounds();
      const double pts = static_cast<double>(bb0.x1 - bb0.x0) / static_cast<double>(pitch) * static_cast<double>(bb0.y1 - bb0.y0) / static_cast<double>(pitch);
      if (opt.pitch_scale != 1.0 && pts >= 3e6) pitch = std::clamp<Coord>(static_cast<Coord>(static_cast<double>(pitch) * opt.pitch_scale) / 5'000 * 5'000, 25'000, 200'000);
    }
    res.pitch = pitch;
    const auto bb = obs->bounds();
    lat = geom::Box{bb.x0 / pitch * pitch, bb.y0 / pitch * pitch, bb.x1, bb.y1};
    nx = static_cast<int>((lat.x1 - lat.x0) / pitch) + 1;
    ny = static_cast<int>((lat.y1 - lat.y0) / pitch) + 1;
    // Near-routed raster: how many routed items could conflict with a probe centred on each lattice point.
    // Points with a zero count skip the routed-copper query (a third of the search time on large boards).
    Coord probe = 0;
    for (std::size_t n = 0; n < b.nets.size(); ++n) {
      const auto net = static_cast<NetId>(n);
      probe = std::max({probe, class_width(net) / 2, via_diameter(net) / 2});
    }
    const Coord hc = std::max<Coord>(rules.minimums.hole_clearance, 0);
    const Coord vm = b.vias_tented ? 0 : std::max<Coord>(b.pad_to_mask_clearance, 0);
    near_infl = probe + pitch * 71 / 100 + 1 + std::max({obs->rules().max_clearance(), hc, 2 * vm + 1'000}) + 2 * pitch + 2;
    near_r.assign(static_cast<std::size_t>(nl) * static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), 0);
  }
  std::vector<std::uint16_t> near_r;
  Coord near_infl = 0;
  void near_mark(int item, int delta) {
    const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
    const geom::Box bx = it.box.inflated(near_infl);
    const int x0 = std::max(0, static_cast<int>((bx.x0 - lat.x0) / pitch) - 1), x1 = std::min(nx - 1, static_cast<int>((bx.x1 - lat.x0) / pitch) + 1);
    const int y0 = std::max(0, static_cast<int>((bx.y0 - lat.y0) / pitch) - 1), y1 = std::min(ny - 1, static_cast<int>((bx.y1 - lat.y0) / pitch) + 1);
    for (int l = 0; l < nl; ++l) {
      if (!(it.layers & model::layer_bit(l))) continue;
      for (int y = y0; y <= y1; ++y) {
        std::uint16_t* row = &near_r[(static_cast<std::size_t>(l) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(y)) * static_cast<std::size_t>(nx)];
        for (int x = x0; x <= x1; ++x) row[x] = static_cast<std::uint16_t>(row[x] + delta);
      }
    }
  }
  void remove_routed(int item) {
    if (!obs->copper().items[static_cast<std::size_t>(item)].removed) near_mark(item, -1);
    obs->remove_item(item);
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
      if (!opt.only_net.empty() && b.nets[static_cast<std::size_t>(net)].name != opt.only_net) continue;
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
    if (opt.order == 1) {
      std::stable_sort(out.begin(), out.end(), [](const Connection& a, const Connection& c) { return a.length > c.length; });
    } else if (opt.order == 2) {
      // Shortest first with seeded jitter (up to 2x length), for portfolio diversity.
      const RngStream rng(opt.seed, 0x0D3Du, 0);
      std::vector<std::pair<double, std::size_t>> key;
      for (std::size_t i = 0; i < out.size(); ++i) key.emplace_back(static_cast<double>(out[i].length) * (1.0 + rng.uniform(i)), i);
      std::stable_sort(key.begin(), key.end());
      std::vector<Connection> sorted;
      for (const auto& [k, i] : key) sorted.push_back(out[i]);
      out = std::move(sorted);
    } else {
      std::stable_sort(out.begin(), out.end(), [](const Connection& a, const Connection& c) { return a.length < c.length; });
    }
    if (!opt.priority.empty()) {
      std::set<std::pair<std::string, std::string>> pri;
      for (const auto& [x, y] : opt.priority) {
        pri.insert({x, y});
        pri.insert({y, x});
      }
      auto label = [&](int pad) {
        if (pad < 0) return std::string("zone");
        const auto& p = b.pads[static_cast<std::size_t>(pad)];
        return b.footprints[static_cast<std::size_t>(p.footprint)].reference + "." + p.number;
      };
      std::stable_partition(out.begin(), out.end(), [&](const Connection& c) { return pri.count({label(c.pad_a), label(c.pad_b)}) > 0; });
    }
    return out;
  }

  // ---------------------------------------------------------------------------------------------------
  // A* on (layer, cell, arrival direction).
  // ---------------------------------------------------------------------------------------------------
  struct Endpoint {
    std::vector<std::pair<int, std::int64_t>> cells;  // (layer, cell index in window)
    std::vector<Point> stub;                           // per cell: off-lattice escape point (or the pad centre)
    std::vector<std::int64_t> cost;                    // per cell: cost from the pad centre
  };
  // Off-lattice escapes: straight exits from the pad centre in 8 directions, exactly checked, joining the
  // lattice at the first legal point (fine-pitch pins are often unreachable from lattice points alone).
  struct Escape {
    int layer, gx, gy;
    Point stub;
    std::int64_t cost;
  };
  std::unordered_map<std::int64_t, std::vector<Escape>> escape_cache;  // (pad, width) -> escapes valid against fixed copper

  const std::vector<Escape>& escapes(int pad) {
    const std::int64_t ekey = static_cast<std::int64_t>(pad) * 4'000'000 + track_width(b.pads[static_cast<std::size_t>(pad)].net) / 1000;
    if (auto it = escape_cache.find(ekey); it != escape_cache.end()) return it->second;
    std::vector<Escape> out;
    const auto& p = b.pads[static_cast<std::size_t>(pad)];
    const NetId net = p.net;
    const Coord width = track_width(net);
    const Coord stepl = std::max<Coord>(pitch / 2, 10'000);
    const bool saved_soft = soft;
    soft = false;
    for (int l = 0; l < nl; ++l) {
      if (!(p.copper & model::layer_bit(l))) continue;
      for (int d = 0; d < 8; ++d) {
        const double ux = kDx[d] / ((d & 1) ? std::numbers::sqrt2 : 1.0), uy = kDy[d] / ((d & 1) ? std::numbers::sqrt2 : 1.0);
        for (int k = 1; k <= 40; ++k) {
          const Point E{p.pos.x + static_cast<Coord>(std::llround(ux * static_cast<double>(stepl * k))),
                        p.pos.y + static_cast<Coord>(std::llround(uy * static_cast<double>(stepl * k)))};
          if (obs->segment_state(p.pos, E, l, width, net, true) == 2) break;  // fixed copper blocks this direction
          const int gx = to_ix(E.x), gy = to_iy(E.y);
          if (gx < 0 || gy < 0 || gx >= nx || gy >= ny) break;
          const Point C = at(gx, gy);
          if (fixed_point_blocked(l, gx, gy, net, width / 2)) continue;  // fixed copper only: the cache outlives routes
          if (obs->segment_state(E, C, l, width, net, true) == 2) continue;
          out.push_back({l, gx, gy, E,
                         static_cast<std::int64_t>(std::hypot(static_cast<double>(E.x - p.pos.x), static_cast<double>(E.y - p.pos.y)) +
                                                   std::hypot(static_cast<double>(C.x - E.x), static_cast<double>(C.y - E.y)))});
          break;
        }
      }
    }
    soft = saved_soft;
    return escape_cache.emplace(ekey, std::move(out)).first->second;
  }

  // Off-lattice escapes: straight exits from the pad centre in 8 directions, exactly checked, joining the
  // lattice at the first legal point (fine-pitch pins are often unreachable from lattice points alone).
  // Cached per pad against fixed copper; routed copper is checked by the search and at commit.
  void add_escapes(const Window& w, int pad, Endpoint& e) {
    const auto& p = b.pads[static_cast<std::size_t>(pad)];
    const Coord width = track_width(p.net);
    for (const auto& x : escapes(pad)) {
      const int cx = x.gx - w.x0, cy = x.gy - w.y0;
      if (cx < 0 || cy < 0 || cx >= w.w || cy >= w.h) continue;
      // Against current routed copper too (the cache only knows fixed copper).
      if (point_state(x.layer, x.gx, x.gy, p.net, width / 2) == 2) continue;
      if (obs->segment_state(p.pos, x.stub, x.layer, width, p.net, soft) == 2) continue;
      if (obs->segment_state(x.stub, at(x.gx, x.gy), x.layer, width, p.net, soft) == 2) continue;
      e.cells.emplace_back(x.layer, static_cast<std::int64_t>(cy) * w.w + cx);
      e.stub.push_back(x.stub);
      e.cost.push_back(x.cost);
    }
  }

  static std::int64_t cell_key(int layer, int gx, int gy) {
    return (static_cast<std::int64_t>(layer) << 48) | (static_cast<std::int64_t>(gy) << 24) | gx;
  }
  std::int64_t hist_cost(int layer, int gx, int gy) const {
    if (history.empty()) return 0;
    const auto it = history.find(cell_key(layer, gx, gy));
    return it == history.end() ? 0 : static_cast<std::int64_t>(it->second) * pitch * 2;
  }
  ClassCache& cache_for(NetId net) {
    auto& cc = caches[{&netclass(net), track_width(net)}];
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

  // Blocked by fixed copper alone (legal without the lattice margin otherwise)?
  bool fixed_point_blocked(int layer, int gx, int gy, NetId net, Coord hw) {
    const Point p = at(gx, gy);
    if (!use_cache) return obs->disk_state(p, layer, hw, net, 0, true) == 2;
    auto& cc = cache_for(net);
    const std::size_t gi = (static_cast<std::size_t>(layer) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(gy)) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(gx);
    if (cc.tight[gi] == INT32_MIN) cc.tight[gi] = obs->fixed_code(p, layer, hw, 0, cc.rep);
    return !code_ok(cc.tight[gi], net);
  }

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
    if (near_r[gi] == 0) return st;
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
      cell_state[idx] = static_cast<std::uint8_t>(learned_block.count({block_owner(net), key}) ? 2 : point_state(layer, gx, gy, net, hw));
    }
    const int st = cell_state[idx];
    if (st == 2) return -1;
    const std::int64_t rc = reserved_cost(layer, gx, gy, net);
    if (rc < 0) return -1;
    std::int64_t hc = hist_cost(layer, gx, gy) + rc;
    if (corr) {  // soft guidance: leaving the global corridor (or its layer) costs half a pitch per lattice step
      const Point p = at(gx, gy);
      const int tx = std::clamp(global.tile_of_x(p.x), 0, global.tiles_x - 1), ty = std::clamp(global.tile_of_y(p.y), 0, global.tiles_y - 1);
      if (!(*corr)[(static_cast<std::size_t>(layer) * static_cast<std::size_t>(global.tiles_y) + static_cast<std::size_t>(ty)) * static_cast<std::size_t>(global.tiles_x) + static_cast<std::size_t>(tx)])
        hc += corridor_pen;
    }
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
          if (near_r[(static_cast<std::size_t>(l) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(gy)) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(gx)] == 0) continue;
          const int r = obs->routed_state(geom::Shape::point(p, d / 2 + margin), l, net, drc::ItemKind::Via, soft, nullptr, true, drill / 2 + margin);
          if (r == 2) st = 2;
          else if (r == 1) st = 1;
        }
      }
      via_state[idx] = static_cast<std::uint8_t>(st);
    }
    if (via_state[idx] == 2) return -1;
    std::int64_t extra = 0;
    for (int l = 0; l < nl && !reserve.empty(); ++l) {
      const std::int64_t rc = reserved_cost(l, w.x0 + cx, w.y0 + cy, net);
      if (rc < 0) return -1;
      extra = std::max(extra, rc);
    }
    return extra + (via_state[idx] == 1 ? static_cast<std::int64_t>(opt.soft_cost_mm * 1e6) : 0);
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
          if (in) {
            const Point C = at(w.x0 + cx, w.y0 + cy);
            e.cells.emplace_back(l, static_cast<std::int64_t>(cy) * w.w + cx);
            e.stub.push_back(p.pos);
            e.cost.push_back(static_cast<std::int64_t>(std::hypot(static_cast<double>(C.x - p.pos.x), static_cast<double>(C.y - p.pos.y))));
          }
        }
      if (e.cells.size() == before) {  // tiny pad: nearest lattice point
        const int cx = std::clamp(to_ix(p.pos.x) - w.x0, 0, w.w - 1), cy = std::clamp(to_iy(p.pos.y) - w.y0, 0, w.h - 1);
        const Point C = at(w.x0 + cx, w.y0 + cy);
        e.cells.emplace_back(l, static_cast<std::int64_t>(cy) * w.w + cx);
        e.stub.push_back(p.pos);
        e.cost.push_back(static_cast<std::int64_t>(std::hypot(static_cast<double>(C.x - p.pos.x), static_cast<double>(C.y - p.pos.y))));
      }
    }
    // Escapes only for pads without a usable lattice point of their own (typically fine-pitch pins): elsewhere
    // they steal space other pins need.
    bool any_free = false;
    const Coord width = track_width(p.net);
    for (const auto& [l, ci] : e.cells) {
      const int gx = w.x0 + static_cast<int>(ci % w.w), gy = w.y0 + static_cast<int>(ci / w.w);
      if (point_state(l, gx, gy, p.net, width / 2) != 2) {
        any_free = true;
        break;
      }
    }
    if (!any_free || force_escapes) add_escapes(w, pad, e);
    return e;
  }

  struct PathNode { int layer, gx, gy; };

  // ---- cost-to-go field ----
  static constexpr std::int64_t kUnreachable = std::int64_t{1} << 60;
  std::vector<std::int32_t> field;
  std::vector<std::uint8_t> f_pass, f_via, f_tgt;
  bool field_ok = false;
  long field_runs = 0, field_gpu_fail = 0, field_cpu_runs = 0;
  double field_seconds = 0;
  void build_field(const Window& w, NetId net, Coord hw, const Endpoint& dst, std::int64_t step, std::int64_t diag, std::int64_t viac) {
    (void)hw;
    const auto t0f = std::chrono::steady_clock::now();
    field_ok = false;
    const std::size_t cells = static_cast<std::size_t>(w.w) * static_cast<std::size_t>(w.h);
    auto& cc = cache_for(net);
    f_pass.assign(cells * static_cast<std::size_t>(nl), 1);
    f_via.assign(cells, 1);
    f_tgt.assign(cells * static_cast<std::size_t>(nl), 0);
    for (int l = 0; l < nl; ++l)
      for (int cy = 0; cy < w.h; ++cy) {
        const std::size_t gbase = (static_cast<std::size_t>(l) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(w.y0 + cy)) * static_cast<std::size_t>(nx) +
                                  static_cast<std::size_t>(w.x0);
        std::uint8_t* row = &f_pass[static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(cy) * static_cast<std::size_t>(w.w)];
        for (int cx = 0; cx < w.w; ++cx) {
          const std::int32_t t = cc.tight[gbase + static_cast<std::size_t>(cx)];
          if (t != INT32_MIN && !code_ok(t, net)) row[cx] = 0;  // known blocked by fixed copper even without margin
        }
      }
    for (int cy = 0; cy < w.h; ++cy)
      for (int cx = 0; cx < w.w; ++cx) {
        const std::int32_t v = cc.via[static_cast<std::size_t>(w.y0 + cy) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(w.x0 + cx)];
        if (v != INT32_MIN && !code_ok(v, net)) f_via[static_cast<std::size_t>(cy) * static_cast<std::size_t>(w.w) + static_cast<std::size_t>(cx)] = 0;
      }
    for (const auto& [l, ci] : dst.cells) {
      const std::size_t i = static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(ci);
      f_tgt[i] = 1;
      f_pass[i] = 1;
    }
    gpu::FieldProblem fp{w.w, w.h, nl, static_cast<std::int32_t>(step), static_cast<std::int32_t>(diag), static_cast<std::int32_t>(std::min<std::int64_t>(viac, 1'000'000'000)),
                         f_pass.data(), f_via.data(), f_tgt.data()};
    // Same field on the GPU or the CPU (identical by construction), so results do not depend on GPU availability.
    const auto st = opt.gpu_device >= 0 ? gpu::field_cuda(opt.gpu_device, fp, field) : gpu::GpuStatus{false, "no GPU"};
    if (st.ok) {
      ++field_runs;
    } else {
      gpu::field_cpu(fp, field);
      if (opt.gpu_device >= 0) ++field_gpu_fail;
      ++field_cpu_runs;
    }
    field_ok = true;
    field_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0f).count();
  }

  // Why the last search ended without a path (failure explanation, design doc 06 T0).
  enum class Miss { None, Enclosed, Window, Budget };
  long expansion_cap = 0;  // > 0: per-search expansion limit overriding opt.max_expansions (reverse probes)
  Miss last_miss = Miss::None;

  bool search(const Connection& c, const Window& w, std::vector<PathNode>& path) {
    last_miss = Miss::None;
    bool touched_edge = false;
    const NetId net = c.net;
    const Coord width = track_width(net);
    const Coord hw = width / 2;
    const Coord vd = via_diameter(net);
    const Coord vdrill = via_drill(net);
    const std::size_t cells = static_cast<std::size_t>(w.w) * static_cast<std::size_t>(w.h);
    const std::size_t D = opt.bend_states ? 9 : 1;  // direction states per lattice point
    const std::size_t states = cells * static_cast<std::size_t>(nl) * D;
    if (sn.size() < states) sn.assign(states, SNode{0, 0, -1});
    if (cstamp.size() < cells * static_cast<std::size_t>(nl)) {
      cstamp.assign(cells * static_cast<std::size_t>(nl), 0);
      cell_state.resize(cells * static_cast<std::size_t>(nl));
    }
    if (vstamp.size() < cells) {
      vstamp.assign(cells, 0);
      via_state.resize(cells);
    }
    if (++gen > kGenMask) {
      std::fill(sn.begin(), sn.end(), SNode{0, 0, -1});
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
    const std::int64_t via_cost = static_cast<std::int64_t>(opt.via_cost_mm * via_cost_mult * 1e6);
    // Cost-to-go field (GPU) for large windows: exact distances to the targets through cells not known to be
    // blocked by fixed copper; a lower bound on the true cost, so A* stays optimal while expanding far less.
    const bool use_field = !zone_target && opt.field_heuristic && use_cache && !fields_off &&
                           cells * static_cast<std::size_t>(nl) >= static_cast<std::size_t>(opt.field_min_cells);
    if (use_field) {
      build_field(w, net, hw, dst, step, diag, static_cast<std::int64_t>(opt.via_cost_mm * via_cost_mult * 1e6));
      // Wall-clock mode only (keeps --work runs deterministic): when shared GPUs make fields cost more than
      // 30% of the run, this variant continues with the octile heuristic.
      const double el = elapsed();
      if (opt.work_budget == 0 && el > 10.0 && field_seconds > 0.3 * el) fields_off = true;
    }
    const bool have_field = use_field && field_ok;
    auto h = [&](int fl, int gx, int gy) -> std::int64_t {
      if (have_field) {
        const std::int32_t v = field[static_cast<std::size_t>(fl) * cells + static_cast<std::size_t>(gy - w.y0) * static_cast<std::size_t>(w.w) +
                                     static_cast<std::size_t>(gx - w.x0)];
        // Never prune on the field: where it claims "unreachable" fall back to the octile bound, so the search
        // stays complete even if the field's view of blocked cells is stale or wrong.
        if (v < gpu::kFieldInf) return static_cast<std::int64_t>(static_cast<double>(v) * opt.heuristic_weight);
      }
      const Point p = at(gx, gy);
      if (zone_target) return std::int64_t{0};  // plane anywhere nearby: no useful lower bound
      const std::int64_t dx = std::llabs(p.x - tp.x), dy = std::llabs(p.y - tp.y);
      const std::int64_t mn = std::min(dx, dy), mx = std::max(dx, dy);
      return static_cast<std::int64_t>(static_cast<double>((mx - mn) + mn * diag / step) * opt.heuristic_weight);  // octile distance
    };
    auto sidx = [&](int l, std::int64_t ci, int dir) {
      return ((static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(ci)) * D) + (D == 9 ? static_cast<std::size_t>(dir) : 0);
    };
    using QE = std::pair<std::int64_t, std::size_t>;  // (f, state)
    std::priority_queue<QE, std::vector<QE>, std::greater<>> open;
    const Point sp = b.pads[static_cast<std::size_t>(c.pad_a)].pos;
    start_stub.clear();
    target_stub.clear();
    for (std::size_t k = 0; k < dst.cells.size(); ++k) {
      const auto key = cell_key(dst.cells[k].first, w.x0 + static_cast<int>(dst.cells[k].second % w.w), w.y0 + static_cast<int>(dst.cells[k].second / w.w));
      if (!target_stub.count(key) || dst.stub[k] == tp) target_stub[key] = dst.stub[k];
    }
    (void)sp;
    for (std::size_t k = 0; k < src.cells.size(); ++k) {
      const auto [l, ci] = src.cells[k];
      const int cx = static_cast<int>(ci % w.w), cy = static_cast<int>(ci / w.w);
      const std::int64_t g0 = src.cost[k];
      const std::size_t s = sidx(l, ci, kNoDir);
      if ((sn[s].tag & kGenMask) == gen && sn[s].g <= g0) continue;
      start_stub[cell_key(l, w.x0 + cx, w.y0 + cy)] = src.stub[k];
      sn[s] = SNode{g0, gen | (static_cast<std::uint32_t>(kNoDir) << 28), -1};
      if (h(l, w.x0 + cx, w.y0 + cy) >= kUnreachable) continue;
      open.emplace(g0 + h(l, w.x0 + cx, w.y0 + cy), s);
    }
    long expanded = 0;
    const long exp_cap = expansion_cap > 0 ? expansion_cap : opt.max_expansions;
    std::size_t goal = SIZE_MAX;
    while (!open.empty()) {
      const auto [f, s] = open.top();
      open.pop();
      const int dir = D == 9 ? static_cast<int>(s % 9) : static_cast<int>(sn[s].tag >> 28);
      const std::size_t lc = s / D;
      const int l = static_cast<int>(lc / cells);
      const std::int64_t ci = static_cast<std::int64_t>(lc % cells);
      const int cx = static_cast<int>(ci % w.w), cy = static_cast<int>(ci / w.w);
      if (f - h(l, w.x0 + cx, w.y0 + cy) > sn[s].g) continue;  // stale entry
      if (cx == 0 || cy == 0 || cx == w.w - 1 || cy == w.h - 1) touched_edge = true;
      if (target(l, ci)) {
        goal = s;
        break;
      }
      if (++expanded > exp_cap) break;
      if (live && (expanded & 63) == 0) {
        recent[recent_n++ % recent.size()] = {at(w.x0 + cx, w.y0 + cy), l};
        if ((expanded & 8191) == 0) emit_frontier();
      }
      const std::int64_t gs = sn[s].g;
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
        if ((sn[ns].tag & kGenMask) == gen && sn[ns].g <= gs + cost) continue;
        const std::int64_t hn = h(l, w.x0 + ncx, w.y0 + ncy);
        if (hn >= kUnreachable) continue;  // cannot reach a target from there
        sn[ns] = SNode{gs + cost, gen | (static_cast<std::uint32_t>(d) << 28), static_cast<std::int32_t>(s)};
        open.emplace(gs + cost + hn, ns);
      }
      // Via: change to every other layer at this cell (through via).
      const std::int64_t vextra = (opt.allow_vias && nl > 1) ? via_cost_at(w, cx, cy, net, vd, vdrill) : -1;
      if (vextra < 0 && blind_ok) {
        // Through via blocked: a blind or buried via spanning only the layers between (dearer: costs more to make).
        const Point vp = at(w.x0 + cx, w.y0 + cy);
        const Coord vm = pitch * 71 / 100 + 1;
        for (int l2 = 0; l2 < nl; ++l2) {
          if (l2 == l) continue;
          const int vs = obs->via_state_span(vp, vd, vdrill, net, vm, soft, nullptr, std::min(l, l2), std::max(l, l2));
          if (vs == 2) continue;
          const std::size_t ns = sidx(l2, ci, kNoDir);
          const std::int64_t ng = gs + via_cost * 3 / 2 + (vs == 1 ? static_cast<std::int64_t>(opt.soft_cost_mm * 1e6) : 0);
          if ((sn[ns].tag & kGenMask) == gen && sn[ns].g <= ng) continue;
          const std::int64_t hn = h(l2, w.x0 + cx, w.y0 + cy);
          if (hn >= kUnreachable) continue;
          sn[ns] = SNode{ng, gen | (static_cast<std::uint32_t>(kNoDir) << 28), static_cast<std::int32_t>(s)};
          open.emplace(ng + hn, ns);
        }
      }
      if (vextra >= 0) {
        for (int l2 = 0; l2 < nl; ++l2) {
          if (l2 == l) continue;
          const std::size_t ns = sidx(l2, ci, kNoDir);
          const std::int64_t ng = gs + via_cost + vextra;
          if ((sn[ns].tag & kGenMask) == gen && sn[ns].g <= ng) continue;
          const std::int64_t hn = h(l2, w.x0 + cx, w.y0 + cy);
          if (hn >= kUnreachable) continue;
          sn[ns] = SNode{ng, gen | (static_cast<std::uint32_t>(kNoDir) << 28), static_cast<std::int32_t>(s)};
          open.emplace(ng + hn, ns);
        }
      }
    }
    res.expansions += expanded;
    (goal == SIZE_MAX ? exp_fail : exp_ok) += expanded;
    (goal == SIZE_MAX ? n_fail : n_ok) += 1;
    if (goal == SIZE_MAX) {
      // Open list exhausted without reaching the window edge: the source is boxed in, so a larger window
      // cannot help (Contour's boxed-in terminal test).
      last_miss = expanded > exp_cap ? Miss::Budget : touched_edge ? Miss::Window : Miss::Enclosed;
      if (std::getenv("TM_DEBUG_ENCLOSED") && last_miss == Miss::Enclosed && soft) {
        // Classify the rejected neighbours of every expanded cell (diagnostics).
        long fixed = 0, learned = 0, other = 0, startcells = static_cast<long>(src.cells.size());
        for (std::size_t lc = 0; lc < cells * static_cast<std::size_t>(nl); ++lc) {
          if (cstamp[lc] != gen) continue;
          if (cell_state[lc] != 2) continue;
          const int l = static_cast<int>(lc / cells);
          const std::int64_t ci = static_cast<std::int64_t>(lc % cells);
          const int gx = w.x0 + static_cast<int>(ci % w.w), gy = w.y0 + static_cast<int>(ci / w.w);
          if (learned_block.count({block_owner(net), cell_key(l, gx, gy)})) ++learned;
          else if (obs->disk_state(at(gx, gy), l, hw, net, 0, true) == 2) ++fixed;
          else ++other;
        }
        std::fprintf(stderr, "ENCLOSED %s conn %d: expanded %ld, start cells %ld, blocked neighbours: fixed %ld learned %ld other %ld\n",
                     b.nets[static_cast<std::size_t>(net)].name.c_str(), current, expanded, startcells, fixed, learned, other);
      }
      return false;
    }
    path.clear();
    for (std::int64_t s = static_cast<std::int64_t>(goal); s >= 0; s = sn[static_cast<std::size_t>(s)].parent) {
      const std::size_t lc = static_cast<std::size_t>(s) / D;
      const int l = static_cast<int>(lc / cells);
      const std::int64_t ci = static_cast<std::int64_t>(lc % cells);
      path.push_back({l, w.x0 + static_cast<int>(ci % w.w), w.y0 + static_cast<int>(ci / w.w)});
      if (sn[static_cast<std::size_t>(s)].parent < 0) break;
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
    std::vector<std::pair<int, int>> via_span;  // layers joined by each via
    const Point pa = b.pads[static_cast<std::size_t>(c.pad_a)].pos;
    const Point pb = c.pad_b >= 0 ? b.pads[static_cast<std::size_t>(c.pad_b)].pos : at(path.back().gx, path.back().gy);
    // Corner points: pad centre, direction changes and layer changes, pad centre.
    Point cur = pa;
    int layer = path.front().layer;
    auto P = [&](const PathNode& n) { return at(n.gx, n.gy); };
    // Leading escape stub (pad centre -> escape point) before the first lattice point.
    if (auto it = start_stub.find(cell_key(path.front().layer, path.front().gx, path.front().gy)); it != start_stub.end() && !(it->second == pa)) {
      segs.push_back({pa, it->second, layer});
      cur = it->second;
    }
    for (std::size_t i = 0; i < path.size(); ++i) {
      const Point p = P(path[i]);
      if (path[i].layer != layer) {  // via at the previous point
        if (!(cur == p)) segs.push_back({cur, p, layer});
        vias.push_back(p);
        via_span.emplace_back(std::min(layer, path[i].layer), std::max(layer, path[i].layer));
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
    if (c.pad_b >= 0) {
      if (auto it = target_stub.find(cell_key(path.back().layer, path.back().gx, path.back().gy)); it != target_stub.end() && !(it->second == pb) && !(it->second == cur)) {
        segs.push_back({cur, it->second, layer});
        cur = it->second;
      }
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
    // A pad leg (pad centre <-> first/last lattice point) that fails the exact check is dropped when the
    // lattice point already lies on the pad's copper: KiCad connects a track to any pad it overlaps.
    auto on_pad = [&](Point q, int pad, int lay) { return on_pad_copper(q, pad, lay); };
    if (merged.size() >= 2) {
      const auto& f = merged.front();
      if (f.a == pa && on_pad(f.b, c.pad_a, f.layer) && obs->segment_state(f.a, f.b, f.layer, width, net, true, nullptr) == 2)
        merged.erase(merged.begin());
    }
    if (merged.size() >= 2 && c.pad_b >= 0) {
      const auto& l = merged.back();
      if (l.b == pb && on_pad(l.a, c.pad_b, l.layer) && obs->segment_state(l.a, l.b, l.layer, width, net, true, nullptr) == 2) merged.pop_back();
    }
    // Exact verification. In soft mode, conflicts with other connections' routed copper name the victims.
    bool ok = true;
    std::vector<int> victims;
    for (const auto& s : merged) {
      const int st = obs->segment_state(s.a, s.b, s.layer, width, net, soft, &victims);
      if (st == 2) {
        ok = false;
        if (std::getenv("TM_DEBUG_EXACT"))
          std::fprintf(stderr, "EXACT %s conn %d soft %d: (%.4f,%.4f)-(%.4f,%.4f) L%d w %.3f\n", b.nets[static_cast<std::size_t>(net)].name.c_str(), current,
                       soft ? 1 : 0, nm_to_mm(s.a.x), nm_to_mm(s.a.y), nm_to_mm(s.b.x), nm_to_mm(s.b.y), s.layer, nm_to_mm(width));
        learn_block(c, s, width);
      }
    }
    // Each via is a through via where that is legal, else (boards that allow them) a blind/buried via over its span.
    std::vector<std::pair<int, int>> via_layers(vias.size(), {0, nl - 1});
    for (std::size_t k = 0; k < vias.size(); ++k) {
      if (obs->via_state(vias[k], vd, vdrill, net, 0, soft, &victims) != 2) continue;
      if (blind_ok && obs->via_state_span(vias[k], vd, vdrill, net, 0, soft, &victims, via_span[k].first, via_span[k].second) != 2) {
        via_layers[k] = via_span[k];
        continue;
      }
      ok = false;
    }
    if (!ok) {
      commit_why = "exact check rejected the lattice path";
      return false;
    }
    std::sort(victims.begin(), victims.end());
    victims.erase(std::unique(victims.begin(), victims.end()), victims.end());
    for (int v : victims)
      if (cs[static_cast<std::size_t>(v)].rips >= rip_cap) {
        commit_why = "would rip a connection already ripped too often";
        return false;
      }
    // Rip up victims, and raise history on the contested cells so later searches avoid them.
    for (const auto& s : merged) bump_history(s, net);  // before the rip, while the conflicts still exist
    for (int v : victims) rip(v);
    // Commit.
    auto& st = cs[static_cast<std::size_t>(current)];
    for (const auto& s : merged) {
      model::Track t{s.a, s.b, width, s.layer, net, false, sexpr::kNoNode};
      b.tracks.push_back(t);
      const int id = static_cast<int>(b.tracks.size() - 1);
      st.items.push_back(obs->add_track(id, current));
      near_mark(st.items.back(), +1);
      if (opt.sink)
        emit("{\"type\":\"track_add\",\"track\":{\"id\":" + std::to_string(id) + ",\"a\":[" + jnum(t.a.x) + "," + jnum(t.a.y) + "],\"b\":[" +
             jnum(t.b.x) + "," + jnum(t.b.y) + "],\"w\":" + jnum(t.width) + ",\"layer\":" + std::to_string(t.layer) + ",\"net\":" +
             std::to_string(t.net) + "}}");
    }
    for (std::size_t k = 0; k < vias.size(); ++k) {
      const auto [top, bot] = via_layers[k];
      const bool through = top == 0 && bot == nl - 1;
      model::Via v{vias[k], vd, vdrill, top, bot, through ? model::ViaType::Through : model::ViaType::Blind, net, false, sexpr::kNoNode};
      b.vias.push_back(v);
      const int id = static_cast<int>(b.vias.size() - 1);
      st.items.push_back(obs->add_via(id, current));
      near_mark(st.items.back(), +1);
      if (!through) ++res.blind_vias;
      emit_via_add(id);
    }
    return true;
  }

  bool on_pad_copper(Point q, int pad, int lay) const {
    const auto& it = obs->copper().items[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(pad)])];
    if (!(it.layers & model::layer_bit(lay))) return false;
    for (const auto& sh : it.shapes)
      if (geom::closer_than(geom::Shape::point(q, 1), sh, 0)) return true;
    return false;
  }

  void learn_block(const Connection& c, const auto& s, Coord width) {
    const int n = std::max<int>(1, static_cast<int>(std::hypot(static_cast<double>(s.b.x - s.a.x), static_cast<double>(s.b.y - s.a.y)) / static_cast<double>(pitch)));
    const auto& ia = obs->copper().items[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(c.pad_a)])].box;
    const auto& ib = c.pad_b >= 0 ? obs->copper().items[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(c.pad_b)])].box : ia;
    int learned = 0;
    for (int k = 0; k <= n; ++k) {
      const Point q{s.a.x + (s.b.x - s.a.x) * k / n, s.a.y + (s.b.y - s.a.y) * k / n};
      const geom::Box qb = geom::Shape::point(q, 0).box;
      // Never block the pads themselves; points merely inside a pad's bounding box (beside a fine-pitch pad)
      // can be blocked.
      if ((qb.intersects(ia) && on_pad_copper(q, c.pad_a, s.layer)) || (c.pad_b >= 0 && qb.intersects(ib) && on_pad_copper(q, c.pad_b, s.layer))) continue;
      if (obs->disk_state(q, s.layer, width / 2, c.net, 0, /*ignore_routed=*/true) == 2) {
        learned_block.insert({block_owner(c.net), cell_key(s.layer, to_ix(q.x), to_iy(q.y))});
        ++learned;
      }
    }
    // Every sampled disk legal but the segment not (a diagonal step clipping a fine-pitch pad corner): block
    // the step's lattice points that are off the connection's pads, or the search repeats the same step.
    if (learned == 0 && obs->segment_state(s.a, s.b, s.layer, width, c.net, true, nullptr) == 2) {
      for (int k = 0; k <= n; ++k) {
        const Point q{s.a.x + (s.b.x - s.a.x) * k / n, s.a.y + (s.b.y - s.a.y) * k / n};
        if (on_pad_copper(q, c.pad_a, s.layer) || (c.pad_b >= 0 && on_pad_copper(q, c.pad_b, s.layer))) continue;
        if ((k == 0 || k == n) && n > 1) continue;  // interior points suffice for longer steps
        learned_block.insert({block_owner(c.net), cell_key(s.layer, to_ix(q.x), to_iy(q.y))});
      }
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
      remove_routed(item);
    }
    st.items.clear();
    if (st.routed) --res.routed;
    st.routed = false;
    st.coupled = false;
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

  long exp_ok = 0, exp_fail = 0, n_ok = 0, n_fail = 0;
  std::string why, commit_why;
  bool strict_pass = false;
  // Nogoods (design doc 06 §3.3): (connection, soft, window signature) attempts that already failed. The
  // signature hashes the routed copper inside the window, so any relevant change re-enables the attempt.
  std::unordered_map<std::uint64_t, std::uint8_t> nogoods;
  GlobalResult global;               // corridors from the global router (empty when off)
  const std::vector<std::uint8_t>* corr = nullptr;  // corridor of the connection being searched
  Coord corridor_pen = 0;            // extra cost per lattice step outside the corridor
  bool blind_ok = false;    // blind/buried vias allowed (board setting, more than two layers)
  bool fields_off = false;
  double via_cost_mult = 1.0;   // raised by the clean-up pass
  bool bypass_nogoods = false;  // clean-up re-routes are judged on their own  // set when GPU fields cost too much wall-clock time (see search)
  int rip_cap = 0;  // per-connection rip limit (opt.max_rips_per_connection, raised by diversified restarts)
  long nogood_skips = 0;
  std::uint64_t window_signature(const geom::Box& box) {
    std::uint64_t h = 0x9E3779B97F4A7C15ull;
    std::vector<int> ids;
    obs->routed_items_in(box, ids);
    std::sort(ids.begin(), ids.end());
    for (int id : ids) h = splitmix64(h ^ static_cast<std::uint64_t>(id));
    return h;
  }
  std::unordered_map<std::int64_t, Point> start_stub, target_stub;  // lattice point -> escape point (pad centre if none)
  bool search_and_commit(const Connection& c, bool soft_mode) {
    soft = soft_mode;
    // Skip an attempt that already failed in exactly this situation.
    const Point pa0 = b.pads[static_cast<std::size_t>(c.pad_a)].pos;
    const Point pb0 = c.pad_b >= 0 ? b.pads[static_cast<std::size_t>(c.pad_b)].pos : pa0;
    geom::Box wb;
    wb.add(pa0);
    wb.add(pb0);
    wb = wb.inflated(6'000'000 + c.length / 4);
    const std::uint64_t ng = splitmix64(window_signature(wb) ^ (static_cast<std::uint64_t>(current) << 3) ^ (soft_mode ? 1u : 0u) ^
                                        (force_escapes ? 2u : 0u) ^ (static_cast<std::uint64_t>(width_override) << 20));
    if (!bypass_nogoods && nogoods.count(ng)) {
      ++nogood_skips;
      why = "skipped: identical earlier attempt failed (nogood)";
      return false;
    }
    corr = (!global.corridor.empty() && current >= 0 && static_cast<std::size_t>(current) < global.corridor.size()) ? &global.corridor[static_cast<std::size_t>(current)] : nullptr;
    const bool ok = search_and_commit_inner(c);
    corr = nullptr;
    if (!ok && !bypass_nogoods) nogoods[ng] = 1;
    return ok;
  }

  bool search_and_commit_inner(const Connection& c) {
    const Point a = b.pads[static_cast<std::size_t>(c.pad_a)].pos;
    const Point e = c.pad_b >= 0 ? b.pads[static_cast<std::size_t>(c.pad_b)].pos : a;
    static const Coord margins[] = {2'000'000, 6'000'000, 20'000'000, 1'000'000'000};
    std::vector<PathNode> path;
    // The strict first pass tries two window sizes only; anything harder is left to negotiation.
    // Negotiated searches stop before the whole-board window (they can cross copper, so a reachable target is
    // normally found within 20 mm of the bounding box); strict passes try two sizes.
    const int attempts = strict_pass ? std::min(2, opt.max_attempts) : soft ? std::min(opt.soft_attempts, opt.max_attempts) : opt.max_attempts;
    for (int attempt = 0; attempt < attempts; ++attempt) {
      if (out_of_budget()) {
        why = "out of budget";
        return false;
      }
      const Coord m = margins[std::min(attempt, 3)] + c.length / 4;
      Window w;
      w.x0 = std::max(0, to_ix(std::min(a.x, e.x) - m));
      w.y0 = std::max(0, to_iy(std::min(a.y, e.y) - m));
      const int x1 = std::min(nx - 1, to_ix(std::max(a.x, e.x) + m)), y1 = std::min(ny - 1, to_iy(std::max(a.y, e.y) + m));
      w.w = x1 - w.x0 + 1;
      w.h = y1 - w.y0 + 1;
      if (!search(c, w, path)) {
        why = last_miss == Miss::Enclosed ? "boxed in" : last_miss == Miss::Budget ? "search budget" : "no path in window";
        if (last_miss == Miss::Enclosed) {
          ++res.enclosed;
          return false;  // boxed in: go straight to negotiation (or give up in strict mode)
        }
        // Only the source is tested for being boxed in; a boxed-in target makes the search flood the window
        // instead. A short reverse search detects that (an enclosed pocket exhausts within a few thousand
        // expansions) so the escalation ladder (escapes, neck-down) can run. If it finds a path, use it.
        if (attempt == 0 && c.pad_b >= 0) {
          Connection r = c;
          std::swap(r.pad_a, r.pad_b);
          std::vector<PathNode> rp;
          expansion_cap = 60'000;
          const bool found = search(r, w, rp);
          expansion_cap = 0;
          if (!found && last_miss == Miss::Enclosed) {
            ++res.enclosed;
            why = "boxed in (target)";
            return false;
          }
          if (found && commit(r, rp)) return true;
          last_miss = Miss::Window;
        }
        continue;
      }
      if (commit(c, path)) return true;
      why = commit_why;
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

  // ---- live visualisation helpers ----
  bool live = false;  // a viewer wants transient messages
  std::array<std::pair<Point, int>, 96> recent{};
  std::size_t recent_n = 0;
  void emit_frontier() {
    if (!opt.sink || !opt.sink->wants_transient()) return;
    std::string m = "{\"type\":\"frontier\",\"conn\":" + std::to_string(current) + ",\"layer\":" + std::to_string(recent[0].second) + ",\"pts\":[";
    const std::size_t n = std::min(recent_n, recent.size());
    for (std::size_t i = 0; i < n; ++i) m += (i ? ",[" : "[") + jnum(recent[i].first.x) + "," + jnum(recent[i].first.y) + "]";
    emit(m + "]}");
    recent_n = 0;
  }
  void emit_ratsnest() {
    if (!opt.sink) return;
    std::string m = "{\"type\":\"ratsnest\",\"edges\":[";
    bool first = true;
    for (const auto& st : cs) {
      if (st.routed) continue;
      const Point a = b.pads[static_cast<std::size_t>(st.c.pad_a)].pos;
      const Point e = st.c.pad_b >= 0 ? b.pads[static_cast<std::size_t>(st.c.pad_b)].pos : a;
      m += (first ? "[" : ",[") + jnum(a.x) + "," + jnum(a.y) + "," + jnum(e.x) + "," + jnum(e.y) + "," + std::to_string(st.c.net) + "]";
      first = false;
    }
    emit(m + "]}");
  }

  // Clean-up pass (after routing, while budget remains): re-route each connection that uses vias with vias made
  // dearer, in strict mode (no crossing), and keep the new route only if it is cheaper by length + 2 mm per via;
  // otherwise restore the old copper exactly. Comparable in purpose to Freerouting's optimizer stage.
  void emit_track_add(int index) {
    if (!opt.sink) return;
    const auto& t = b.tracks[static_cast<std::size_t>(index)];
    emit("{\"type\":\"track_add\",\"track\":{\"id\":" + std::to_string(index) + ",\"a\":[" + jnum(t.a.x) + "," + jnum(t.a.y) + "],\"b\":[" +
         jnum(t.b.x) + "," + jnum(t.b.y) + "],\"w\":" + jnum(t.width) + ",\"layer\":" + std::to_string(t.layer) + ",\"net\":" +
         std::to_string(t.net) + "}}");
  }
  void emit_via_add(int index) {
    if (!opt.sink) return;
    const auto& v = b.vias[static_cast<std::size_t>(index)];
    emit("{\"type\":\"via_add\",\"via\":{\"id\":" + std::to_string(index) + ",\"p\":[" + jnum(v.pos.x) + "," + jnum(v.pos.y) + "],\"d\":" +
         jnum(v.size) + ",\"drill\":" + jnum(v.drill) + ",\"net\":" + std::to_string(v.net) + ",\"top\":" + std::to_string(v.layer_top) +
         ",\"bottom\":" + std::to_string(v.layer_bottom) + "}}");
  }

  // ---------------------------------------------------------------------------------------------------
  // Differential pairs (design doc 05 §9 phase 3): the two halves of a P/N pair are routed together as one wide
  // object. A* finds the pair's centreline on one layer with a half-width covering both tracks and the gap; the
  // centreline is then offset left and right into the two tracks, joined to the pads by short legs, verified
  // exactly and committed. Anything that fails falls back to ordinary routing of the two connections.
  // ---------------------------------------------------------------------------------------------------
  struct PairSeg { Point a, b; Coord w; };
  bool commit_segments(int ci, const std::vector<PairSeg>& segs, int layer) {
    auto& st = cs[static_cast<std::size_t>(ci)];
    for (const auto& [a, e, width] : segs) {
      if (a == e) continue;
      b.tracks.push_back(model::Track{a, e, width, layer, st.c.net, false, sexpr::kNoNode});
      const int id = obs->add_track(static_cast<int>(b.tracks.size() - 1), ci);
      near_mark(id, +1);
      st.items.push_back(id);
      emit_track_add(static_cast<int>(b.tracks.size() - 1));
    }
    return true;
  }

  std::string pair_why;
  bool route_pair(int ia, int ib) {
    const Connection& A = cs[static_cast<std::size_t>(ia)].c;
    Connection B = cs[static_cast<std::size_t>(ib)].c;
    const auto& pa1 = b.pads[static_cast<std::size_t>(A.pad_a)];
    const auto& pa2 = b.pads[static_cast<std::size_t>(A.pad_b)];
    auto d = [](Point u, Point v) { return std::hypot(static_cast<double>(u.x - v.x), static_cast<double>(u.y - v.y)); };
    if (d(pa1.pos, b.pads[static_cast<std::size_t>(B.pad_b)].pos) + d(pa2.pos, b.pads[static_cast<std::size_t>(B.pad_a)].pos) <
        d(pa1.pos, b.pads[static_cast<std::size_t>(B.pad_a)].pos) + d(pa2.pos, b.pads[static_cast<std::size_t>(B.pad_b)].pos))
      std::swap(B.pad_a, B.pad_b);
    const auto& pb1 = b.pads[static_cast<std::size_t>(B.pad_a)];
    const auto& pb2 = b.pads[static_cast<std::size_t>(B.pad_b)];
    const model::LayerMask common = pa1.copper & pa2.copper & pb1.copper & pb2.copper;
    if (!common) { pair_why = "no common layer"; return false; }
    const int layer = std::countr_zero(common);
    const auto& nc = netclass(A.net);
    const Coord w = std::max(nc.has_diff_pair_gap ? nc.diff_pair_width : class_width(A.net), rules.minimums.track_width);
    const Coord clr = std::max(nc.clearance, rules.minimums.clearance);
    const Coord gap = (nc.has_diff_pair_gap ? std::min(clr, nc.diff_pair_gap) : clr) + 10'000;  // 10 um margin
    const Coord off = (w + gap) / 2;           // centreline to each track centre
    const Coord hw_env = off + w / 2;          // centreline to the outer track edges
    const Point S{(pa1.pos.x + pb1.pos.x) / 2, (pa1.pos.y + pb1.pos.y) / 2};
    const Point E{(pa2.pos.x + pb2.pos.x) / 2, (pa2.pos.y + pb2.pos.y) / 2};
    const double se = std::hypot(static_cast<double>(E.x - S.x), static_cast<double>(E.y - S.y));
    if (se < 2'000'000) { pair_why = "too short to couple"; return false; }
    const Coord R = std::min<Coord>(2'500'000, static_cast<Coord>(0.3 * se));  // centreline ends this close to the pad midpoints
    const Coord margin = pitch * 71 / 100 + 1;
    geom::Box box;
    box.add(S);
    box.add(E);
    box = box.inflated(6'000'000);
    const int x0 = std::max(0, to_ix(box.x0)), y0 = std::max(0, to_iy(box.y0));
    const int x1 = std::min(nx - 1, to_ix(box.x1)), y1 = std::min(ny - 1, to_iy(box.y1));
    const int W = x1 - x0 + 1, H = y1 - y0 + 1;
    if (W <= 2 || H <= 2) { pair_why = "window"; return false; }
    std::vector<std::int8_t> legal(static_cast<std::size_t>(W) * static_cast<std::size_t>(H), -1);
    auto ok = [&](int cx, int cy) {
      auto& v = legal[static_cast<std::size_t>(cy) * static_cast<std::size_t>(W) + static_cast<std::size_t>(cx)];
      if (v < 0) {
        const Point p = at(x0 + cx, y0 + cy);
        const auto code = obs->fixed_code(p, layer, hw_env, margin, A.net);
        bool good = (code == Obstacles::kFree || code == A.net || code == B.net) && obs->inside_board(p, 0);
        if (good) good = obs->routed_state(geom::Shape::point(p, hw_env + margin), layer, A.net, drc::ItemKind::Track, false, nullptr) == 0;
        v = good ? 1 : 0;
      }
      return v == 1;
    };
    const std::int64_t step = pitch, diag = static_cast<std::int64_t>(std::llround(static_cast<double>(pitch) * std::numbers::sqrt2));
    auto hcost = [&](int cx, int cy) {
      const Point p = at(x0 + cx, y0 + cy);
      const double dd = std::max(0.0, d(p, E) - static_cast<double>(R));
      return static_cast<std::int64_t>(dd);
    };
    const std::size_t N = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
    std::vector<std::int64_t> g(N, std::numeric_limits<std::int64_t>::max());
    std::vector<std::int32_t> par(N, -1);
    using QE = std::pair<std::int64_t, std::int32_t>;
    std::priority_queue<QE, std::vector<QE>, std::greater<>> pq;
    const int r_cells = static_cast<int>(R / pitch);
    const int sx = to_ix(S.x) - x0, sy = to_iy(S.y) - y0;
    for (int dy = -r_cells; dy <= r_cells; ++dy)
      for (int dx = -r_cells; dx <= r_cells; ++dx) {
        const int cx = sx + dx, cy = sy + dy;
        if (cx < 0 || cy < 0 || cx >= W || cy >= H) continue;
        const Point p = at(x0 + cx, y0 + cy);
        if (d(p, S) > static_cast<double>(R) || !ok(cx, cy)) continue;
        const std::size_t id = static_cast<std::size_t>(cy) * static_cast<std::size_t>(W) + static_cast<std::size_t>(cx);
        g[id] = static_cast<std::int64_t>(2.0 * d(p, S));  // legs are dearer than coupled track
        pq.emplace(g[id] + hcost(cx, cy), static_cast<std::int32_t>(id));
      }
    std::int32_t goal = -1;
    long expanded = 0;
    static const int DX[8] = {1, -1, 0, 0, 1, 1, -1, -1}, DY[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (!pq.empty() && expanded < 2'000'000) {
      const auto [f, u] = pq.top();
      pq.pop();
      const int cx = u % W, cy = u / W;
      if (f - hcost(cx, cy) > g[static_cast<std::size_t>(u)]) continue;
      ++expanded;
      const Point p = at(x0 + cx, y0 + cy);
      if (d(p, E) <= static_cast<double>(R)) {
        goal = u;
        break;
      }
      for (int k = 0; k < 8; ++k) {
        const int vx = cx + DX[k], vy = cy + DY[k];
        if (vx < 0 || vy < 0 || vx >= W || vy >= H || !ok(vx, vy)) continue;
        const std::size_t v = static_cast<std::size_t>(vy) * static_cast<std::size_t>(W) + static_cast<std::size_t>(vx);
        std::int64_t c = k < 4 ? step : diag;
        if (par[static_cast<std::size_t>(u)] >= 0) {  // bend penalty keeps the pair straight
          const int pu = par[static_cast<std::size_t>(u)];
          if ((cx - pu % W) != DX[k] || (cy - pu / W) != DY[k]) c += step;
        }
        const std::int64_t ng = g[static_cast<std::size_t>(u)] + c;
        if (ng < g[v]) {
          g[v] = ng;
          par[v] = u;
          pq.emplace(ng + hcost(vx, vy), static_cast<std::int32_t>(v));
        }
      }
    }
    res.expansions += expanded;
    if (goal < 0) { pair_why = "no centreline path"; return false; }
    std::vector<Point> path;
    for (std::int32_t u = goal; u >= 0; u = par[static_cast<std::size_t>(u)]) path.push_back(at(x0 + u % W, y0 + u / W));
    std::reverse(path.begin(), path.end());
    // Keep corners only.
    std::vector<Point> c{path.front()};
    for (std::size_t i = 1; i + 1 < path.size(); ++i) {
      const Point a0 = c.back(), m = path[i], e = path[i + 1];
      if (geom::orient(a0, m, e) != 0) c.push_back(m);
    }
    if (path.size() > 1) c.push_back(path.back());
    if (c.size() < 2) { pair_why = "short"; return false; }
    // Offset the centreline by +-off (miter joins).
    auto offset = [&](double sgn) {
      std::vector<Point> o;
      for (std::size_t i = 0; i < c.size(); ++i) {
        auto nrm = [&](Point u, Point v) {
          const double dx = static_cast<double>(v.x - u.x), dy = static_cast<double>(v.y - u.y), L = std::hypot(dx, dy);
          return std::pair<double, double>{-dy / L, dx / L};
        };
        std::pair<double, double> n;
        if (i == 0) n = nrm(c[0], c[1]);
        else if (i + 1 == c.size()) n = nrm(c[i - 1], c[i]);
        else {
          const auto n1 = nrm(c[i - 1], c[i]), n2 = nrm(c[i], c[i + 1]);
          const double dot = n1.first * n2.first + n1.second * n2.second;
          n = {(n1.first + n2.first) / (1 + dot), (n1.second + n2.second) / (1 + dot)};
        }
        o.push_back({c[i].x + static_cast<Coord>(std::llround(sgn * static_cast<double>(off) * n.first)),
                     c[i].y + static_cast<Coord>(std::llround(sgn * static_cast<double>(off) * n.second))});
      }
      return o;
    };
    auto left = offset(1.0), right = offset(-1.0);
    if (d(left.front(), pa1.pos) + d(right.front(), pb1.pos) > d(right.front(), pa1.pos) + d(left.front(), pb1.pos)) std::swap(left, right);
    if (d(left.back(), pa2.pos) + d(right.back(), pb2.pos) > d(right.back(), pa2.pos) + d(left.back(), pb2.pos)) {
      pair_why = "halves swap sides between the ends (needs a layer change)";
      return false;
    }
    // Legs from the pads to the coupled section: straight if legal, else one of the two octilinear dog-legs.
    // Legs from the pads to the coupled section: straight if legal, else one of the two octilinear dog-legs; at the
    // pair width first, then necked down to the board minimum (fine-pitch connectors).
    const Coord wmin = std::max<Coord>(rules.minimums.track_width, 1);
    auto leg = [&](Point from, Point to, NetId net, Coord& lw) -> std::vector<Point> {
      for (Coord ww : {w, std::min(w, wmin)}) {
        auto seg_ok = [&](Point u, Point v) { return u == v || obs->segment_state(u, v, layer, ww, net, false, nullptr) == 0; };
        lw = ww;
        if (seg_ok(from, to)) return {from, to};
        const Coord dx = to.x - from.x, dy = to.y - from.y, m = std::min(std::llabs(dx), std::llabs(dy));
        const Point c1{from.x + (dx > 0 ? m : -m), from.y + (dy > 0 ? m : -m)};
        if (seg_ok(from, c1) && seg_ok(c1, to)) return {from, c1, to};
        const Point c2{to.x - (dx > 0 ? m : -m), to.y - (dy > 0 ? m : -m)};
        if (seg_ok(from, c2) && seg_ok(c2, to)) return {from, c2, to};
        if (ww == wmin) break;
      }
      return {};
    };
    auto build = [&](Point s0, const std::vector<Point>& mid, Point e0, NetId net, std::vector<PairSeg>& segs) {
      Coord w0 = w, w1 = w;
      const auto l0 = leg(s0, mid.front(), net, w0), l1 = leg(mid.back(), e0, net, w1);
      if (l0.empty() || l1.empty()) {
        if (std::getenv("TM_DEBUG_PAIRS")) {
          const Point f = l0.empty() ? s0 : mid.back(), t = l0.empty() ? mid.front() : e0;
          std::fprintf(stderr, "  leg %s fails: --pts %.4f %.4f %.4f %.4f --layer %d --width %.3f --net '%s'\n", l0.empty() ? "start" : "end", nm_to_mm(f.x),
                       nm_to_mm(f.y), nm_to_mm(t.x), nm_to_mm(t.y), layer, nm_to_mm(wmin), b.nets[static_cast<std::size_t>(net)].name.c_str());
        }
        return false;
      }
      for (std::size_t i = 0; i + 1 < l0.size(); ++i) segs.push_back({l0[i], l0[i + 1], w0});
      for (std::size_t i = 0; i + 1 < mid.size(); ++i) segs.push_back({mid[i], mid[i + 1], w});
      for (std::size_t i = 0; i + 1 < l1.size(); ++i) segs.push_back({l1[i], l1[i + 1], w1});
      return true;
    };
    std::vector<PairSeg> sa, sb;
    if (!build(pa1.pos, left, pa2.pos, A.net, sa) || !build(pb1.pos, right, pb2.pos, B.net, sb)) { pair_why = "legs"; return false; }
    for (const auto& x : sa)
      for (const auto& y : sb)
        if (geom::segments_intersect(x.a, x.b, y.a, y.b)) { pair_why = "halves cross"; return false; }
    for (const auto& x : sa)
      if (!(x.a == x.b) && obs->segment_state(x.a, x.b, layer, x.w, A.net, false, nullptr) != 0) { pair_why = "exact check (P)"; return false; }
    for (const auto& x : sb)
      if (!(x.a == x.b) && obs->segment_state(x.a, x.b, layer, x.w, B.net, false, nullptr) != 0) { pair_why = "exact check (N)"; return false; }
    // Each half against the other (the pair gap rule applies between them).
    commit_segments(ia, sa, layer);
    for (const auto& x : sb)
      if (!(x.a == x.b) && obs->segment_state(x.a, x.b, layer, x.w, B.net, false, nullptr) != 0) {
        for (int item : cs[static_cast<std::size_t>(ia)].items) remove_routed(item);
        cs[static_cast<std::size_t>(ia)].items.clear();
        return false;
      }
    commit_segments(ib, sb, layer);
    for (int ci : {ia, ib}) {
      cs[static_cast<std::size_t>(ci)].routed = true;
      cs[static_cast<std::size_t>(ci)].coupled = true;
      ++res.routed;
    }
    return true;
  }

  int route_diff_pairs() {
    std::map<NetId, std::vector<int>> by_net;
    for (std::size_t ci = 0; ci < cs.size(); ++ci)
      if (!cs[ci].routed && cs[ci].c.pad_b >= 0) by_net[cs[ci].c.net].push_back(static_cast<int>(ci));
    int done = 0;
    const bool dbg = std::getenv("TM_DEBUG_PAIRS") != nullptr;
    auto pos = [&](int pad) { return b.pads[static_cast<std::size_t>(pad)].pos; };
    auto dist = [](Point u, Point v) { return std::hypot(static_cast<double>(u.x - v.x), static_cast<double>(u.y - v.y)); };
    for (const auto& [na, la] : by_net)
      for (const auto& [nb, lb] : by_net) {
        if (na >= nb || !obs->rules().coupled_diff_pair(na, nb)) continue;
        // Pair up connections whose two ends lie close to each other (each half may have several connections).
        std::vector<char> used(lb.size(), 0);
        for (int ca : la) {
          const auto& A = cs[static_cast<std::size_t>(ca)].c;
          int best = -1;
          double bd = 4e6;
          for (std::size_t k = 0; k < lb.size(); ++k) {
            if (used[k]) continue;
            const auto& B = cs[static_cast<std::size_t>(lb[k])].c;
            const double d1 = std::max(dist(pos(A.pad_a), pos(B.pad_a)), dist(pos(A.pad_b), pos(B.pad_b)));
            const double d2 = std::max(dist(pos(A.pad_a), pos(B.pad_b)), dist(pos(A.pad_b), pos(B.pad_a)));
            const double dd = std::min(d1, d2);
            if (dd < bd) { bd = dd; best = static_cast<int>(k); }
          }
          if (best < 0 || out_of_budget()) continue;
          const bool okp = route_pair(ca, lb[static_cast<std::size_t>(best)]);
          if (dbg)
            std::fprintf(stderr, "pair %s / %s: %s\n", b.nets[static_cast<std::size_t>(na)].name.c_str(), b.nets[static_cast<std::size_t>(nb)].name.c_str(),
                         okp ? "coupled" : pair_why.c_str());
          if (okp) {
            used[static_cast<std::size_t>(best)] = 1;
            ++done;
          }
        }
      }
    return done;
  }

  void optimize_vias() {
    via_cost_mult = 10.0;
    bypass_nogoods = true;
    strict_pass = false;
    struct Old { drc::ItemKind kind; int index; };
    auto cost_of = [&](const std::vector<int>& items, int& vias) {
      double len = 0;
      vias = 0;
      for (int item : items) {
        const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
        if (it.kind == drc::ItemKind::Via) {
          ++vias;
        } else {
          const auto& t = b.tracks[static_cast<std::size_t>(it.index)];
          len += std::hypot(static_cast<double>(t.b.x - t.a.x), static_cast<double>(t.b.y - t.a.y));
        }
      }
      return len + 2e6 * vias;
    };
    std::vector<std::pair<int, int>> order;  // (-vias, connection)
    for (std::size_t i = 0; i < cs.size(); ++i) {
      const auto& st = cs[i];
      if (!st.routed || st.implicit || st.coupled || st.items.empty()) continue;
      int v = 0;
      cost_of(st.items, v);
      if (v > 0) order.emplace_back(-v, static_cast<int>(i));
    }
    std::sort(order.begin(), order.end());
    for (const auto& [nv, ci] : order) {
      if (out_of_budget()) break;
      auto& st = cs[static_cast<std::size_t>(ci)];
      int old_v = 0;
      const double old_cost = cost_of(st.items, old_v);
      std::vector<Old> saved;
      for (int item : st.items) {
        const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
        saved.push_back({it.kind, it.index});
        if (opt.sink) emit(std::string("{\"type\":\"") + (it.kind == drc::ItemKind::Via ? "via_remove" : "track_remove") + "\",\"id\":" + std::to_string(it.index) + "}");
        remove_routed(item);
      }
      st.items.clear();
      current = ci;
      soft = false;
      const bool ok = search_and_commit(st.c, false);
      int new_v = 0;
      const double new_cost = ok ? cost_of(st.items, new_v) : 0;
      if (ok && new_cost < old_cost - 1e3) {
        ++res.optimized;
        continue;
      }
      for (int item : st.items) {  // rejected: restore the old copper
        const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
        if (opt.sink) emit(std::string("{\"type\":\"") + (it.kind == drc::ItemKind::Via ? "via_remove" : "track_remove") + "\",\"id\":" + std::to_string(it.index) + "}");
        remove_routed(item);
      }
      st.items.clear();
      for (const auto& o : saved) {
        const int id = o.kind == drc::ItemKind::Via ? obs->add_via(o.index, ci) : obs->add_track(o.index, ci);
        near_mark(id, +1);
        if (o.kind == drc::ItemKind::Via) emit_via_add(o.index);
        else emit_track_add(o.index);
        st.items.push_back(id);
      }
    }
    via_cost_mult = 1.0;
    bypass_nogoods = false;
  }

  // Path smoothing ("pull tight"): within each connection, replace runs of same-layer, same-width segments by the
  // fewest octilinear segments the exact rule check accepts (other nets' copper is a hard obstacle). Pad and via
  // positions stay fixed, so connectivity is unchanged. Returns the number of connections changed.
  // Region rip-up and re-route (large-neighbourhood search) around vias: take the connections with copper within
  // 2.5 mm of a via (at most 8), remove them all, route them again with vias made dearer and other copper as a hard
  // obstacle, and keep the result only if every one routes and length + 2 mm per via goes down; otherwise restore
  // the old copper exactly. Unlike single-connection re-routes this can move a neighbour out of the way.
  int lns_vias(int max_attempts) {
    via_cost_mult = 10.0;
    bypass_nogoods = true;
    strict_pass = false;
    struct Old { drc::ItemKind kind; int index; };
    auto conn_cost = [&](const std::vector<int>& items) {
      double len = 0;
      int vias = 0;
      for (int item : items) {
        const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
        if (it.kind == drc::ItemKind::Via) {
          ++vias;
        } else {
          const auto& t = b.tracks[static_cast<std::size_t>(it.index)];
          len += std::hypot(static_cast<double>(t.b.x - t.a.x), static_cast<double>(t.b.y - t.a.y));
        }
      }
      return len + 2e6 * vias;
    };
    // Via positions at the start, in a fixed order (connection index, then item order): deterministic.
    std::vector<std::pair<int, Point>> targets;
    for (std::size_t ci = 0; ci < cs.size(); ++ci) {
      if (!cs[ci].routed || cs[ci].implicit || cs[ci].coupled) continue;
      for (int item : cs[ci].items) {
        const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
        if (it.kind == drc::ItemKind::Via) targets.emplace_back(static_cast<int>(ci), it.pos);
      }
    }
    int improved = 0, attempts = 0;
    for (const auto& [owner, p] : targets) {
      if (out_of_budget() || attempts >= max_attempts) break;
      // Is the via still there (an earlier move may have removed it)?
      bool present = false;
      for (int item : cs[static_cast<std::size_t>(owner)].items) {
        const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
        if (it.kind == drc::ItemKind::Via && it.pos == p) present = true;
      }
      if (!present) continue;
      ++attempts;
      std::vector<int> ids;
      const geom::Box box = geom::Shape::point(p, 2'500'000).box;
      obs->routed_items_in(box, ids);
      std::vector<int> group{owner};
      for (int id : ids) {
        const int o = obs->copper().items[static_cast<std::size_t>(id)].owner;
        if (o >= 0 && std::find(group.begin(), group.end(), o) == group.end() && group.size() < 8 && cs[static_cast<std::size_t>(o)].routed &&
            !cs[static_cast<std::size_t>(o)].implicit && !cs[static_cast<std::size_t>(o)].coupled)
          group.push_back(o);
      }
      double before = 0;
      std::vector<std::vector<Old>> saved(group.size());
      for (std::size_t g = 0; g < group.size(); ++g) {
        auto& st = cs[static_cast<std::size_t>(group[g])];
        before += conn_cost(st.items);
        for (int item : st.items) {
          const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
          saved[g].push_back({it.kind, it.index});
          if (opt.sink) emit(std::string("{\"type\":\"") + (it.kind == drc::ItemKind::Via ? "via_remove" : "track_remove") + "\",\"id\":" + std::to_string(it.index) + "}");
          remove_routed(item);
        }
        st.items.clear();
      }
      // Route the via's own connection last: its neighbours first get the space the via was avoiding.
      std::vector<std::size_t> order(group.size());
      for (std::size_t g = 0; g < group.size(); ++g) order[g] = g;
      std::stable_sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) { return (group[x] == owner) < (group[y] == owner); });
      bool ok = true;
      for (std::size_t g : order) {
        current = group[g];
        soft = false;
        if (!search_and_commit(cs[static_cast<std::size_t>(group[g])].c, false)) {
          ok = false;
          break;
        }
      }
      double after = 0;
      if (ok)
        for (int c : group) after += conn_cost(cs[static_cast<std::size_t>(c)].items);
      if (ok && after < before - 1e3) {
        ++improved;
        continue;
      }
      for (std::size_t g = 0; g < group.size(); ++g) {  // rejected: restore every connection of the group
        auto& st = cs[static_cast<std::size_t>(group[g])];
        for (int item : st.items) {
          const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
          if (opt.sink) emit(std::string("{\"type\":\"") + (it.kind == drc::ItemKind::Via ? "via_remove" : "track_remove") + "\",\"id\":" + std::to_string(it.index) + "}");
          remove_routed(item);
        }
        st.items.clear();
        for (const auto& o : saved[g]) {
          const int id = o.kind == drc::ItemKind::Via ? obs->add_via(o.index, group[g]) : obs->add_track(o.index, group[g]);
          near_mark(id, +1);
          if (o.kind == drc::ItemKind::Via) emit_via_add(o.index);
          else emit_track_add(o.index);
          st.items.push_back(id);
        }
      }
    }
    via_cost_mult = 1.0;
    bypass_nogoods = false;
    return improved;
  }

  // Length tuning (design doc 05 §9 phase 3): nets with a custom `length` constraint that are routed too short get
  // trombone meanders on their longest straight segments. Each candidate meander is checked exactly; the net never
  // exceeds its maximum. Returns the number of nets brought into range.
  double net_length(const std::vector<int>& list) const {
    double len = 0;
    for (int ci : list)
      for (int item : cs[static_cast<std::size_t>(ci)].items) {
        const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
        if (it.kind == drc::ItemKind::Track) {
          const auto& t = b.tracks[static_cast<std::size_t>(it.index)];
          len += std::hypot(static_cast<double>(t.b.x - t.a.x), static_cast<double>(t.b.y - t.a.y));
        }
      }
    return len;
  }

  // Adds meanders to `net` (its routed connections `list`) until its length is at least `mn` and at most `mx`.
  bool tune_net(NetId net, const std::vector<int>& list, std::optional<Coord> mn, std::optional<Coord> mx) {
    auto seg_len = [](const model::Track& t) { return std::hypot(static_cast<double>(t.b.x - t.a.x), static_cast<double>(t.b.y - t.a.y)); };
      double len = 0;
      for (int ci : list)
        for (int item : cs[static_cast<std::size_t>(ci)].items) {
          const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
          if (it.kind == drc::ItemKind::Track) len += seg_len(b.tracks[static_cast<std::size_t>(it.index)]);
        }
      const double target = mx ? (static_cast<double>(*mn) + static_cast<double>(*mx)) / 2 : static_cast<double>(*mn) * 1.005;
      double deficit = target - len;
      if (static_cast<double>(*mn) <= len) return true;
      const Coord w = class_width(net), clr = std::max(netclass(net).clearance, rules.minimums.clearance);
      const Coord p = w + clr + 20'000;  // spacing between the meander's parallel runs (centre to centre)
      for (int round = 0; round < 50 && deficit > 1'000; ++round) {
        // Longest track of the net first.
        int best_ci = -1, best_item = -1;
        double best_l = 0;
        for (int ci : list)
          for (int item : cs[static_cast<std::size_t>(ci)].items) {
            const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
            if (it.kind != drc::ItemKind::Track) continue;
            const double l = seg_len(b.tracks[static_cast<std::size_t>(it.index)]);
            if (l > best_l && l > static_cast<double>(3 * p)) { best_l = l; best_ci = ci; best_item = item; }
          }
        if (best_item < 0) break;
        const model::Track t = b.tracks[static_cast<std::size_t>(obs->copper().items[static_cast<std::size_t>(best_item)].index)];
        const double ux = static_cast<double>(t.b.x - t.a.x) / best_l, uy = static_cast<double>(t.b.y - t.a.y) / best_l;
        bool placed = false;
        for (double h_mm : {1.5, 1.0, 0.6, 0.4, 0.25}) {
          const double h = h_mm * 1e6;
          int k = static_cast<int>(std::floor((best_l - static_cast<double>(p)) / (2.0 * static_cast<double>(p))));
          k = std::min(k, static_cast<int>(std::ceil(deficit / (2 * h))));
          if (mx) k = std::min(k, static_cast<int>(std::floor((static_cast<double>(*mx) - len) / (2 * h))));
          if (k <= 0) continue;
          for (double side : {1.0, -1.0}) {
            const double nx_ = -uy * side, ny_ = ux * side;
            auto P = [&](double along, double up) {
              return Point{t.a.x + static_cast<Coord>(std::llround(ux * along + nx_ * up)), t.a.y + static_cast<Coord>(std::llround(uy * along + ny_ * up))};
            };
            std::vector<Point> pts{t.a};
            double s0 = (best_l - 2.0 * static_cast<double>(p) * k) / 2;
            for (int i = 0; i < k; ++i) {
              const double x = s0 + 2.0 * static_cast<double>(p) * i;
              pts.push_back(P(x, 0));
              pts.push_back(P(x, h));
              pts.push_back(P(x + static_cast<double>(p), h));
              pts.push_back(P(x + static_cast<double>(p), 0));
            }
            pts.push_back(t.b);
            // Remove the old track, then check the new ones exactly (other copper is a hard obstacle).
            remove_routed(best_item);
            bool good = true;
            for (std::size_t i = 0; i + 1 < pts.size() && good; ++i)
              if (!(pts[i] == pts[i + 1]) && obs->segment_state(pts[i], pts[i + 1], t.layer, t.width, net, false, nullptr) != 0) good = false;
            auto& items = cs[static_cast<std::size_t>(best_ci)].items;
            items.erase(std::remove(items.begin(), items.end(), best_item), items.end());
            if (!good) {  // put the original back
              const int id = obs->add_track(static_cast<int>(obs->copper().items[static_cast<std::size_t>(best_item)].index), best_ci);
              near_mark(id, +1);
              items.push_back(id);
              best_item = id;
              continue;
            }
            if (opt.sink) emit("{\"type\":\"track_remove\",\"id\":" + std::to_string(obs->copper().items[static_cast<std::size_t>(best_item)].index) + "}");
            std::vector<PairSeg> segs;
            for (std::size_t i = 0; i + 1 < pts.size(); ++i) segs.push_back({pts[i], pts[i + 1], t.width});
            commit_segments(best_ci, segs, t.layer);
            len += 2 * h * k;
            deficit -= 2 * h * k;
            placed = true;
            break;
          }
          if (placed) break;
        }
        if (!placed) break;
      }
      return len >= static_cast<double>(*mn) && (!mx || len <= static_cast<double>(*mx));
  }

  std::map<NetId, std::vector<int>> routed_conns_by_net() const {
    std::map<NetId, std::vector<int>> out;
    for (std::size_t ci = 0; ci < cs.size(); ++ci)
      if (cs[ci].routed && !cs[ci].implicit && !cs[ci].items.empty()) out[cs[ci].c.net].push_back(static_cast<int>(ci));
    return out;
  }

  // Length tuning (design doc 05 §9 phase 3): nets with a custom `length` constraint that are routed too short get
  // trombone meanders on their longest straight segments; each candidate is checked exactly and the net never
  // exceeds its maximum. Returns the number of constrained nets brought into range.
  int tune_lengths() {
    int tuned = 0;
    for (const auto& [net, list] : routed_conns_by_net()) {
      const auto [mn, mx] = obs->rules().length_constraint(net);
      if (mn && tune_net(net, list, mn, mx)) ++tuned;
    }
    return tuned;
  }

  // Skew tuning: for a differential pair with a custom `skew` constraint, the shorter half gets meanders until the
  // length difference is within the limit (aiming at half the limit).
  int tune_skew() {
    int tuned = 0;
    const auto by = routed_conns_by_net();
    for (const auto& [na, la] : by)
      for (const auto& [nb, lb] : by) {
        if (na >= nb || !obs->rules().coupled_diff_pair(na, nb)) continue;
        const auto mxs = obs->rules().skew_constraint(na);
        if (std::getenv("TM_DEBUG_TUNE"))
          std::fprintf(stderr, "skew pair %s/%s: rule %s\n", b.nets[static_cast<std::size_t>(na)].name.c_str(), b.nets[static_cast<std::size_t>(nb)].name.c_str(),
                       mxs ? "yes" : "no");
        if (!mxs) continue;
        const double A = net_length(la), B = net_length(lb);
        if (std::getenv("TM_DEBUG_TUNE")) std::fprintf(stderr, "  lengths %.3f / %.3f mm, max skew %.3f\n", A / 1e6, B / 1e6, nm_to_mm(*mxs));
        // KiCad measures a little more than the track sum (vias, length inside pads): aim at half the limit.
        if (std::fabs(A - B) <= static_cast<double>(*mxs) / 2) { ++tuned; continue; }
        const bool a_short = A < B;
        const double longer = std::max(A, B);
        const auto mn = static_cast<Coord>(longer - static_cast<double>(*mxs) / 4), mx = static_cast<Coord>(longer + static_cast<double>(*mxs) / 4);
        if (tune_net(a_short ? na : nb, a_short ? la : lb, mn, mx)) ++tuned;
      }
    return tuned;
  }

  int smooth_paths() {
    int changed = 0;
    auto octi = [](Point a, Point c) {
      const Coord dx = c.x - a.x, dy = c.y - a.y;
      return dx == 0 || dy == 0 || std::llabs(dx) == std::llabs(dy);
    };
    // Up to two octilinear segments from a to c (straight if already octilinear), or empty if none is legal.
    auto shortcut = [&](Point a, Point c, int layer, Coord width, NetId net) -> std::vector<Point> {
      auto legal = [&](Point u, Point v) { return u == v || obs->segment_state(u, v, layer, width, net, false, nullptr) == 0; };
      if (octi(a, c)) return legal(a, c) ? std::vector<Point>{a, c} : std::vector<Point>{};
      const Coord dx = c.x - a.x, dy = c.y - a.y, m = std::min(std::llabs(dx), std::llabs(dy));
      const Point diag{a.x + (dx > 0 ? m : -m), a.y + (dy > 0 ? m : -m)};       // diagonal first
      if (legal(a, diag) && legal(diag, c)) return {a, diag, c};
      const Point straight{c.x - (dx > 0 ? m : -m), c.y - (dy > 0 ? m : -m)};  // straight first
      if (legal(a, straight) && legal(straight, c)) return {a, straight, c};
      return {};
    };
    for (std::size_t ci = 0; ci < cs.size(); ++ci) {
      auto& st = cs[ci];
      if (!st.routed || st.implicit || st.coupled || st.items.empty()) continue;
      std::vector<model::Track> tr;
      std::vector<int> others;  // vias and anything not a track keep their items
      for (int item : st.items) {
        const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
        if (it.kind == drc::ItemKind::Track) tr.push_back(b.tracks[static_cast<std::size_t>(it.index)]);
        else others.push_back(item);
      }
      if (tr.size() < 2) continue;
      std::vector<model::Track> out;
      bool any = false;
      std::size_t k = 0;
      while (k < tr.size()) {
        std::size_t e = k + 1;  // run [k, e): continuous, same layer and width
        while (e < tr.size() && tr[e].layer == tr[k].layer && tr[e].width == tr[k].width && tr[e].a == tr[e - 1].b) ++e;
        std::vector<Point> pts{tr[k].a};
        for (std::size_t q = k; q < e; ++q) pts.push_back(tr[q].b);
        std::vector<Point> npts{pts.front()};
        std::size_t i = 0;
        while (i + 1 < pts.size()) {
          std::size_t best = i + 1;
          std::vector<Point> via_pts;
          for (std::size_t j = pts.size() - 1; j >= i + 2; --j) {
            auto sc = shortcut(pts[i], pts[j], tr[k].layer, tr[k].width, tr[k].net);
            if (!sc.empty() && sc.size() - 1 < j - i) {
              best = j;
              via_pts.assign(sc.begin() + 1, sc.end());
              break;
            }
          }
          if (via_pts.empty()) npts.push_back(pts[best]);
          else {
            npts.insert(npts.end(), via_pts.begin(), via_pts.end());
            any = true;
          }
          i = best;
        }
        for (std::size_t q = 0; q + 1 < npts.size(); ++q)
          if (!(npts[q] == npts[q + 1])) out.push_back(model::Track{npts[q], npts[q + 1], tr[k].width, tr[k].layer, tr[k].net, false, sexpr::kNoNode});
        k = e;
      }
      if (!any) continue;
      // Replace this connection's tracks.
      for (int item : st.items) {
        const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
        if (it.kind == drc::ItemKind::Track) {
          if (opt.sink) emit("{\"type\":\"track_remove\",\"id\":" + std::to_string(it.index) + "}");
          remove_routed(item);
        }
      }
      st.items = others;
      for (const auto& t : out) {
        b.tracks.push_back(t);
        const int id = obs->add_track(static_cast<int>(b.tracks.size() - 1), static_cast<int>(ci));
        near_mark(id, +1);
        st.items.push_back(id);
        emit_track_add(static_cast<int>(b.tracks.size() - 1));
      }
      ++changed;
    }
    return changed;
  }

  // Escape planning (M9): reserve each dense-package pin's escape corridor for its net (route/escape.hpp).
  void plan_escape_reservations() {
    std::vector<char> needs(b.pads.size(), 0);
    pad_open.assign(b.pads.size(), 0);
    for (const auto& st : cs) {
      if (st.routed) continue;
      for (int pad : {st.c.pad_a, st.c.pad_b})
        if (pad >= 0) {
          needs[static_cast<std::size_t>(pad)] = 1;
          ++pad_open[static_cast<std::size_t>(pad)];
        }
    }
    auto keep = [&](NetId n) { return class_width(n) + std::max(netclass(n).clearance, rules.minimums.clearance); };
    EscapeStats es;
    const auto plan = plan_escapes(b, needs, keep, {}, &es);
    reserve.assign(static_cast<std::size_t>(nl) * static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), 0);
    pad_reserved.assign(b.pads.size(), {});
    reserve_pen = static_cast<std::int64_t>(2 * opt.soft_cost_mm * 1e6);
    res.escape_corridors = 0;
    auto mark = [&](std::size_t gi, NetId net, int pad) {
      if (reserve[gi] == 0) {
        reserve[gi] = static_cast<std::int32_t>(net);
        pad_reserved[static_cast<std::size_t>(pad)].push_back(gi);
      } else if (reserve[gi] != static_cast<std::int32_t>(net)) {
        reserve[gi] = -1;  // two plans want it: reserved for nobody
      }
    };
    for (const auto& c : plan) {
      if (c.via) {  // a dog-bone is only worth reserving where that net's via fits among the fixed copper
        const int gx = to_ix(c.b.x), gy = to_iy(c.b.y);
        if (gx < 0 || gy < 0 || gx >= nx || gy >= ny) continue;
        const std::int32_t code = obs->fixed_via_code(at(gx, gy), via_diameter(c.net), via_drill(c.net), 0, cache_for(c.net).rep);
        if (!code_ok(code, c.net)) continue;
      }
      ++res.escape_corridors;
      const Coord r = c.band;
      const int ix0 = std::max(0, to_ix(std::min(c.a.x, c.b.x) - r)), ix1 = std::min(nx - 1, to_ix(std::max(c.a.x, c.b.x) + r));
      const int iy0 = std::max(0, to_iy(std::min(c.a.y, c.b.y) - r)), iy1 = std::min(ny - 1, to_iy(std::max(c.a.y, c.b.y) + r));
      const long double ux = static_cast<long double>(c.b.x - c.a.x), uy = static_cast<long double>(c.b.y - c.a.y);
      const long double len2 = ux * ux + uy * uy;
      const long double r2 = static_cast<long double>(r) * static_cast<long double>(r);
      for (int gy = iy0; gy <= iy1; ++gy)
        for (int gx = ix0; gx <= ix1; ++gx) {
          const Point p = at(gx, gy);
          const long double px = static_cast<long double>(p.x - c.a.x), py = static_cast<long double>(p.y - c.a.y);
          const long double t = len2 > 0 ? std::clamp((px * ux + py * uy) / len2, 0.0L, 1.0L) : 0.0L;
          const long double dx = px - t * ux, dy = py - t * uy;
          if (dx * dx + dy * dy > r2) continue;
          const long double vx = static_cast<long double>(p.x - c.b.x), vy = static_cast<long double>(p.y - c.b.y);
          if (c.via && vx * vx + vy * vy <= r2) {  // the via site: every layer
            for (int l = 0; l < nl; ++l) mark(lat_index(l, gx, gy), c.net, c.pad);
          } else {
            mark(lat_index(c.layer, gx, gy), c.net, c.pad);
          }
        }
    }
  }
  // A pin whose connections are all routed no longer needs its corridor.
  void release_escapes(int ci) {
    if (reserve.empty()) return;
    const auto& c = cs[static_cast<std::size_t>(ci)].c;
    for (int pad : {c.pad_a, c.pad_b}) {
      if (pad < 0) continue;
      auto& open = pad_open[static_cast<std::size_t>(pad)];
      if (open > 0 && --open == 0) {
        for (std::size_t gi : pad_reserved[static_cast<std::size_t>(pad)])
          if (reserve[gi] == static_cast<std::int32_t>(c.net)) reserve[gi] = 0;
        pad_reserved[static_cast<std::size_t>(pad)].clear();
      }
    }
  }

  RouteResult run() {
    t0 = std::chrono::steady_clock::now();
    rip_cap = opt.max_rips_per_connection;
    const bool tdbg = std::getenv("TM_DEBUG_TIMING") != nullptr;
    setup();
    if (tdbg) std::fprintf(stderr, "[%.2f s] setup done: lattice %d x %d x %d, pitch %.3f mm\n", elapsed(), nx, ny, nl, nm_to_mm(pitch));
    emit("{\"type\":\"stage\",\"name\":\"route\",\"state\":\"begin\",\"detail\":\"lattice A* with negotiated rip-up\"}");
    auto con = drc::compute_connectivity(b, obs->copper(), obs->grid());
    if (tdbg) std::fprintf(stderr, "[%.2f s] connectivity done\n", elapsed());
    init_root = con.root;
    drc::UnionFind uf(obs->copper().items.size());
    for (std::size_t i = 0; i < con.root.size(); ++i) uf.unite(static_cast<int>(i), con.root[i]);
    const auto conns = plan(uf);
    if (opt.global_route) {
      std::vector<GlobalNet> gn;
      for (const auto& c : conns) {
        GlobalNet g;
        g.a = b.pads[static_cast<std::size_t>(c.pad_a)].pos;
        g.layers_a = b.pads[static_cast<std::size_t>(c.pad_a)].copper;
        if (c.pad_b >= 0) {
          g.b = b.pads[static_cast<std::size_t>(c.pad_b)].pos;
          g.layers_b = b.pads[static_cast<std::size_t>(c.pad_b)].copper;
        } else {
          g.b = g.a;
          g.layers_b = g.layers_a;
        }
        g.half_width = class_width(c.net) / 2;
        gn.push_back(g);
      }
      GlobalOptions go;
      Coord wc = 1'000'000'000;
      for (const auto& cl : rules.classes) wc = std::min(wc, std::max(cl.track_width, rules.minimums.track_width) + std::max(cl.clearance, rules.minimums.clearance));
      go.pitch = wc;
      go.via_cost_tiles = 2.0;
      global = global_route(*obs, geom::Box{lat.x0, lat.y0, lat.x1, lat.y1}, nl, gn, go);
      const char* cp = std::getenv("TM_CORRIDOR_PEN");  // experiment knob (pitches per step)
      corridor_pen = static_cast<Coord>((cp ? std::atof(cp) : 2.0) * static_cast<double>(pitch));
      if (tdbg)
        std::fprintf(stderr, "[%.2f s] global routing: %d x %d x %d tiles of %.2f mm, %d overflowed edges\n", elapsed(), global.tiles_x, global.tiles_y,
                     global.layers, nm_to_mm(global.tile), global.overflow_edges);
    }
    if (tdbg) std::fprintf(stderr, "[%.2f s] plan done: %zu connections\n", elapsed(), conns.size());
    res.connections = static_cast<int>(conns.size());
    for (const auto& c : conns) {
      ConnState st;
      st.c = c;
      cs.push_back(std::move(st));
    }
    for (std::size_t i = 0; i < cs.size(); ++i) pending.push_back(static_cast<int>(i));
    if (opt.escape_plan) {
      plan_escape_reservations();
      if (tdbg) std::fprintf(stderr, "[%.2f s] escape plan: %d corridors\n", elapsed(), res.escape_corridors);
    }
    live = opt.sink != nullptr;
    if (opt.diff_pairs) {
      const int np = route_diff_pairs();
      if (tdbg) std::fprintf(stderr, "[%.2f s] differential pairs routed coupled: %d\n", elapsed(), np);
      res.pairs = np;
      if (np) {
        pending.clear();
        for (std::size_t i = 0; i < cs.size(); ++i)
          if (!cs[i].routed) pending.push_back(static_cast<int>(i));
      }
    }
    emit_ratsnest();

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

    std::vector<std::uint8_t> best_unrouted;
    auto snapshot_unrouted = [&]() {
      best_unrouted.assign(cs.size(), 0);
      for (std::size_t i = 0; i < cs.size(); ++i) best_unrouted[i] = cs[i].routed ? 0 : 1;
    };
    // Restarts that keep the lessons (design doc 06 §3.6): when negotiation stalls with budget left, rip
    // everything and start again, hardest (most failed) connections first, keeping history, nogoods and the
    // best legal state seen so far.
    // After the planned restarts, keep restarting while budget remains (up to 40 more), diversified: nogoods
    // cleared, rip cap raised, history halved, ties in the hardest-first order shuffled (avr_ledprojector stopped
    // at 43 of 120 s with every remaining attempt a nogood).
    for (int restart = 0; restart <= opt.max_restarts + 40; ++restart) {
    if (restart > 0) {
      if (out_of_budget() || best_routed == res.connections) break;
      for (std::size_t i = 0; i < cs.size(); ++i) {
        auto& st = cs[i];
        for (int item : st.items) {
          const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
          if (opt.sink) emit(std::string("{\"type\":\"") + (it.kind == drc::ItemKind::Via ? "via_remove" : "track_remove") + "\",\"id\":" + std::to_string(it.index) + "}");
          remove_routed(item);
        }
        st.items.clear();
        st.routed = st.implicit = st.coupled = false;
      }
      res.routed = 0;
      if (opt.escape_plan) plan_escape_reservations();  // everything is unrouted again: corridors back
      if (opt.diff_pairs) route_diff_pairs();  // pairs first again, coupled
      std::vector<int> order(cs.size());
      for (std::size_t i = 0; i < cs.size(); ++i) order[i] = static_cast<int>(i);
      if (restart > opt.max_restarts) {
        nogoods.clear();
        rip_cap += 2;
        for (auto it = history.begin(); it != history.end();) {
          it->second = static_cast<std::uint16_t>(it->second / 2);
          it = it->second == 0 ? history.erase(it) : std::next(it);
        }
        const RngStream rr(opt.seed, 0x5E57u, static_cast<std::uint64_t>(restart));
        std::vector<double> tie(cs.size());
        for (std::size_t i = 0; i < cs.size(); ++i) tie[i] = rr.uniform(i);
        std::sort(order.begin(), order.end(), [&](int x, int y) {
          const int fx = cs[static_cast<std::size_t>(x)].fails, fy = cs[static_cast<std::size_t>(y)].fails;
          return fx != fy ? fx > fy : tie[static_cast<std::size_t>(x)] < tie[static_cast<std::size_t>(y)];
        });
      } else {
        std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return cs[static_cast<std::size_t>(x)].fails > cs[static_cast<std::size_t>(y)].fails; });
      }
      pending.assign(order.begin(), order.end());
      pending.erase(std::remove_if(pending.begin(), pending.end(), [&](int c) { return cs[static_cast<std::size_t>(c)].routed || cs[static_cast<std::size_t>(c)].dead; }),
                    pending.end());
      ++res.restarts;
      emit("{\"type\":\"stage\",\"name\":\"restart " + std::to_string(restart) + "\",\"state\":\"begin\",\"detail\":\"hardest connections first, history kept\"}");
    }
    for (int pass = 0; pass < opt.max_passes && !pending.empty() && !out_of_budget(); ++pass) {
      res.passes = pass + 1;
      strict_pass = pass == 0;
      std::vector<int> failed;
      const int routed_before = res.routed;
      while (!pending.empty() && !out_of_budget()) {
        const int ci = pending.front();
        pending.pop_front();
        auto& st = cs[static_cast<std::size_t>(ci)];
        if (st.routed) continue;
        current = ci;
        if (joined(ci)) {
          st.routed = st.implicit = true;
          ++res.routed;
          release_escapes(ci);
          continue;
        }
        bool ok = search_and_commit(st.c, false);
        std::string reason = why;
        // Escalation for pads boxed in by fixed copper: forced off-lattice escapes, then a neck-down to the
        // board's minimum track width (KiCad's track_width rule; the net-class width is only the default).
        if (!ok && (last_miss == Miss::Enclosed || why.starts_with("boxed in") || why.starts_with("exact check"))) {
          force_escapes = true;
          ok = search_and_commit(st.c, false);
          if (!ok && neck_width(st.c.net) > 0) {
            width_override = neck_width(st.c.net);
            ok = search_and_commit(st.c, false);
            if (ok) ++res.necked;
            width_override = 0;
          }
          force_escapes = false;
          if (!ok) reason += "; escapes/neck-down: " + why;
        }
        if (!ok && opt.rip_up && pass > 0) {
          ok = search_and_commit(st.c, true);  // pass 0: strict; later: negotiate
          reason += "; negotiated: " + why;
          if (!ok && why.starts_with("boxed in")) {
            // Last check before giving the pin up: negotiated, with off-lattice escapes and the neck-down width.
            force_escapes = true;
            if (neck_width(st.c.net) > 0) width_override = neck_width(st.c.net);
            ok = search_and_commit(st.c, true);
            if (ok && width_override > 0) ++res.necked;
            width_override = 0;
            force_escapes = false;
            if (!ok && why.starts_with("boxed in")) {
              st.dead = true;
              reason += "; fixed copper encloses the pin";
            }
          }
        }
        cs[static_cast<std::size_t>(ci)].why = reason;
        if (ok) {
          cs[static_cast<std::size_t>(ci)].routed = true;
          ++res.routed;
          release_escapes(ci);
        } else {
          ++cs[static_cast<std::size_t>(ci)].fails;
          if (!st.dead) failed.push_back(ci);
          if (opt.sink) {
            const Point a = b.pads[static_cast<std::size_t>(st.c.pad_a)].pos;
            const Point e = st.c.pad_b >= 0 ? b.pads[static_cast<std::size_t>(st.c.pad_b)].pos : a;
            emit("{\"type\":\"failure\",\"conn\":" + std::to_string(ci) + ",\"net\":" + std::to_string(st.c.net) + ",\"rung\":" +
                 std::to_string(pass > 0 ? 2 : 1) + ",\"cause\":\"no legal path (pass " + std::to_string(pass + 1) + ")\",\"a\":[" + jnum(a.x) + "," +
                 jnum(a.y) + "],\"b\":[" + jnum(e.x) + "," + jnum(e.y) + "],\"blockers\":[],\"region\":[0,0,0,0]}");
          }
        }
        emit_stats("route");
        if (opt.sink && (res.routed % 8 == 0 || pending.empty())) emit_ratsnest();
        if (res.routed > best_routed) {
          snapshot();
          snapshot_unrouted();
        }
      }
      // Next pass: hardest (most failed) first.
      std::stable_sort(failed.begin(), failed.end(), [&](int x, int y) { return cs[static_cast<std::size_t>(x)].fails > cs[static_cast<std::size_t>(y)].fails; });
      for (int f : failed) pending.push_back(f);
      if (pass > 0 && res.routed <= routed_before && res.rips == 0) break;
    }
    if (res.routed > best_routed) {
      snapshot();
      snapshot_unrouted();
    }
    }  // restarts
    if (opt.deadline && best_routed == res.connections && res.connections > 0) {  // first complete variant sets the deadline
      const double d = 2 * elapsed() + 5;
      double cur = opt.deadline->load();
      while (d < cur && !opt.deadline->compare_exchange_weak(cur, d)) {
      }
    }
    // Clean-up only when the live state is a best state (it then stays one: every connection keeps a route).
    // Via-saving re-routes need search budget; smoothing is cheap geometry and always runs (it uses no budget and
    // no randomness, so --work runs stay deterministic).
    if (opt.optimize && best_routed > 0 && res.routed == best_routed) {
      for (int round = 0; round < 4 && !out_of_budget(); ++round) {  // repeat while connections still improve
        const int before = res.optimized;
        optimize_vias();
        if (res.optimized == before) break;
      }
      if (!out_of_budget()) res.optimized += lns_vias(400);
      for (int round = 0; round < 3; ++round) {
        const int n = smooth_paths();
        res.optimized += n;
        if (n == 0) break;
      }
      res.length_tuned = tune_lengths() + tune_skew();
      if (std::getenv("TM_DEBUG_TUNE")) std::fprintf(stderr, "tuned nets: %d\n", res.length_tuned);
      snapshot();
      snapshot_unrouted();
    }
    if (best_unrouted.empty()) snapshot_unrouted();
    res.routed = best_routed;
    res.tracks = std::move(best_tracks);
    res.vias = std::move(best_vias);
    // Failures relative to the best state are approximated by the connections unrouted at the end.
    for (std::size_t ci = 0; ci < cs.size(); ++ci) {
      const auto& st = cs[ci];
      if (!best_unrouted[ci]) continue;
      const auto& pa = b.pads[static_cast<std::size_t>(st.c.pad_a)];
      std::string to = "zone";
      if (st.c.pad_b >= 0) {
        const auto& pb = b.pads[static_cast<std::size_t>(st.c.pad_b)];
        to = b.footprints[static_cast<std::size_t>(pb.footprint)].reference + "." + pb.number;
      }
      res.failures.push_back(b.nets[static_cast<std::size_t>(st.c.net)].name + ": " + b.footprints[static_cast<std::size_t>(pa.footprint)].reference + "." +
                             pa.number + " -> " + to + "  (" + st.why + ")");
      res.unrouted.push_back({b.nets[static_cast<std::size_t>(st.c.net)].name, b.footprints[static_cast<std::size_t>(pa.footprint)].reference + "." + pa.number, to});
    }
    res.seconds = elapsed();
    res.nogood_skips = nogood_skips;
    std::fprintf(stderr, "searches: %ld ok (%ld expansions), %ld failed (%ld expansions); fields %ld GPU + %ld CPU (%.2f s, %ld GPU fallbacks)\n",
                 n_ok, exp_ok, n_fail, exp_fail, field_runs, field_cpu_runs, field_seconds, field_gpu_fail);
    std::fprintf(stderr, "clean-up: %d connections improved\n", res.optimized);
    std::fprintf(stderr, "restarts %d; legality checks %ld; rips %d, passes %d, boxed-in %d, nogood skips %ld, history cells %zu\n", res.restarts, obs->checks, res.rips,
                 res.passes, res.enclosed, nogood_skips, history.size());
    emit_stats("done");
    emit("{\"type\":\"stage\",\"name\":\"route\",\"state\":\"end\",\"detail\":\"\"}");
    return std::move(res);
  }
};

PortfolioResult route_portfolio(const model::Board& board, const model::DesignRules& rules, const RouterOptions& base, int threads,
                                const std::vector<int>& pick) {
  struct Variant {
    std::string name;
    RouterOptions o;
  };
  std::vector<Variant> vs;
  auto add = [&](std::string name, auto tweak) {
    RouterOptions o = base;
    tweak(o);
    if (!vs.empty()) o.sink = nullptr;  // only the first variant streams to the viewer
    vs.push_back({std::move(name), o});
  };
  add("exact bends, shortest first", [](RouterOptions&) {});
  add("fast bends, shortest first", [&](RouterOptions& o) { o.bend_states = false; });
  add("fast bends, longest first (2x pitch on large boards)", [&](RouterOptions& o) { o.bend_states = false; o.order = 1; o.pitch_scale = 2.0; });
  add("fast bends, jittered order", [&](RouterOptions& o) { o.bend_states = false; o.order = 2; o.seed = base.seed + 1; });
  add("exact bends, jittered order", [&](RouterOptions& o) { o.order = 2; o.seed = base.seed + 2; });
  add("fast bends, cheap vias (2x pitch on large boards)", [&](RouterOptions& o) { o.bend_states = false; o.via_cost_mm = base.via_cost_mm * 0.4; o.pitch_scale = 2.0; });
  add("fast bends, cheap crossings", [&](RouterOptions& o) { o.bend_states = false; o.soft_cost_mm = base.soft_cost_mm * 0.5; });
  add("fast bends, dear vias", [&](RouterOptions& o) { o.bend_states = false; o.via_cost_mm = base.via_cost_mm * 2.5; });
  std::vector<int> chosen;
  if (!pick.empty()) {
    for (int i : pick)
      if (i >= 0 && i < static_cast<int>(vs.size())) chosen.push_back(i);
  } else {
    for (int i = 0; i < std::clamp(threads, 1, static_cast<int>(vs.size())); ++i) chosen.push_back(i);
  }
  {
    std::vector<Variant> sel;
    for (std::size_t k = 0; k < chosen.size(); ++k) {
      sel.push_back(vs[static_cast<std::size_t>(chosen[k])]);
      if (k > 0) sel.back().o.sink = nullptr;
      else sel.back().o.sink = base.sink;
    }
    vs = std::move(sel);
  }
  // Spread variants over the visible GPUs (cost-to-go fields); CPU-only when none.
  if (base.gpu_device >= 0) {
    const auto devs = gpu::list_devices();
    for (std::size_t i = 0; i < vs.size(); ++i) vs[i].o.gpu_device = devs.empty() ? -1 : devs[i % devs.size()].cuda_index;
  }
  std::vector<RouteResult> rs(vs.size());
  std::vector<std::thread> pool;
  // Once a variant is complete the others get a short grace period, so the best-quality complete result can be
  // chosen. With a work budget the run must stay deterministic, so this is only used under wall-clock limits.
  std::atomic<double> deadline{1e30};
  for (auto& v : vs)
    if (base.work_budget == 0) v.o.deadline = &deadline;
  // Event buffers per variant (recording mode): the winner's events are replayed into base.sink afterwards.
  struct BufferSink final : events::Sink {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::vector<std::string> msgs;
    void publish(std::string json) override {
      if (json.size() < 2) return;
      const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      msgs.push_back("{\"t\":" + std::to_string(t) + "," + json.substr(1));
    }
    bool wants_transient() const override { return false; }
  };
  std::vector<std::unique_ptr<BufferSink>> bufs;
  if (base.buffer_events && base.sink)
    for (auto& v : vs) {
      bufs.push_back(std::make_unique<BufferSink>());
      v.o.sink = bufs.back().get();
    }
  for (std::size_t i = 0; i < vs.size(); ++i)
    pool.emplace_back([&, i] { rs[i] = Router(board, rules, vs[i].o).run(); });
  for (auto& t : pool) t.join();
  PortfolioResult pr;
  auto length = [](const RouteResult& r) {
    double l = 0;
    for (const auto& t : r.tracks) l += std::hypot(static_cast<double>(t.b.x - t.a.x), static_cast<double>(t.b.y - t.a.y));
    return l;
  };
  std::size_t best = 0;
  pr.indices = chosen;
  for (std::size_t i = 0; i < rs.size(); ++i) {
    pr.variants.push_back(vs[i].name);
    pr.routed.push_back(rs[i].routed);
    const auto& a = rs[i];
    const auto& c = rs[best];
    if (a.routed > c.routed || (a.routed == c.routed && (a.vias.size() < c.vias.size() || (a.vias.size() == c.vias.size() && length(a) < length(c)))))
      best = i;
  }
  pr.best_variant = static_cast<int>(best);
  if (!bufs.empty())
    for (auto& m : bufs[best]->msgs) base.sink->publish(std::move(m));
  pr.best = std::move(rs[best]);
  return pr;
}

int portfolio_size() { return 8; }

Router::Router(const model::Board& board, const model::DesignRules& rules, RouterOptions opt) : in_(board), rules_(rules), opt_(opt) {}

RouteResult Router::run() {
  Impl impl(in_, rules_, opt_);
  return impl.run();
}

}  // namespace tmk::route

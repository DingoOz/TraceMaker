// SPDX-License-Identifier: GPL-3.0-or-later
#include "route/router.hpp"
#include "route/diff_pair.hpp"
#include "route/escape.hpp"
#include "route/escape_flow.hpp"
#include "route/heat_grid.hpp"
#include "route/global_router.hpp"

#include <algorithm>
#include <bit>
#include <numbers>
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
  std::vector<std::int64_t> cell_hist;  // history cost per cell (valid when cstamp matches)
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
  std::set<NetId> first_net_ids;  // --first-nets
  std::unordered_map<std::int64_t, std::uint16_t> history;  // contested lattice cells (PathFinder history cost)
  std::vector<int> init_root;        // copper item -> initial cluster root (fixed copper)
  // With soft zones: routed copper item -> initial clusters of the same-net plane fills it touches, so a via that
  // passes through several planes of its net joins all of them (`joined`). Teardrops are no planes.
  std::vector<std::vector<int>> plane_contacts;
  std::unique_ptr<drc::ZoneFills> soft_fills;
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

  Impl(const model::Board& in, const model::DesignRules& r, const RouterOptions& o) : rules(r), opt(o), b(in) {
    // Name-pattern matching is too costly for per-cell lookups.
    net_class.reserve(b.nets.size());
    for (const auto& n : b.nets) net_class.push_back(&rules.class_for(n.name));
  }

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

  std::vector<const model::NetClass*> net_class;  // by net id
  const model::NetClass& netclass(NetId net) const {
    const auto i = static_cast<std::size_t>(net);
    return i < net_class.size() ? *net_class[i] : rules.class_for(b.nets[i].name);
  }
  // Via drill and diameter for a net: net-class values raised to the board minimums (drill, diameter and
  // annular ring: d >= drill + 2 * min_annular).
  Coord class_via_drill(NetId net) const { return std::max(netclass(net).via_drill, rules.minimums.through_hole_diameter); }
  Coord class_via_diameter(NetId net) const {
    return std::max({netclass(net).via_diameter, rules.minimums.via_diameter, class_via_drill(net) + 2 * rules.minimums.via_annular_width});
  }
  // Via neck-down (M9 escalation rung, with the track neck-down): KiCad's DRC checks vias against the board
  // minimums only (via diameter, drill, annular ring), not the net class, so the smallest via they allow is
  // legal where the class via does not fit (e.g. between BGA balls or in a dense LED matrix). Only used when
  // the class via is blocked; never below a 0.2 mm drill, which every board house drills.
  Coord neck_via_drill(NetId net) const { return std::min(class_via_drill(net), std::max<Coord>(rules.minimums.through_hole_diameter, 200'000)); }
  Coord neck_via_diameter(NetId net) const {
    return std::min(class_via_diameter(net), std::max(rules.minimums.via_diameter, neck_via_drill(net) + 2 * std::max<Coord>(rules.minimums.via_annular_width, 100'000)));
  }
  bool via_override = false;  // escalation rung: the neck-down via
  Coord via_drill(NetId net) const { return via_override ? neck_via_drill(net) : class_via_drill(net); }
  Coord via_diameter(NetId net) const { return via_override ? neck_via_diameter(net) : class_via_diameter(net); }
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
  // Per-net track layers and via permission from custom disallow rules; a net also gets no vias when a
  // physical_hole_clearance rule holds between its vias and its own tracks (doc 05 §27).
  std::vector<model::LayerMask> net_layers;
  std::vector<std::uint8_t> net_vias, net_blind, net_micro;  // through vias; blind/buried and micro vias (when the board has them)
  bool layer_ok(NetId net, int layer) const {
    const auto i = static_cast<std::size_t>(net);
    return i >= net_layers.size() || (net_layers[i] & model::layer_bit(layer));
  }
  bool vias_ok(NetId net) const {
    const auto i = static_cast<std::size_t>(net);
    return i >= net_vias.size() || net_vias[i];
  }
  // Blind/buried and micro vias for this net: the board and options allow them, the net may have vias at all, and
  // no rule disallows that via type for it (`disallow blind_via`, `buried_via`, `micro_via`).
  bool blind_for(NetId net) const {
    const auto i = static_cast<std::size_t>(net);
    return blind_ok && (i >= net_blind.size() || net_blind[i]);
  }
  bool micro_for(NetId net) const {
    const auto i = static_cast<std::size_t>(net);
    return micro_ok && (i >= net_micro.size() || net_micro[i]);
  }

  // Lattice <-> board coordinates.
  Point at(int ix, int iy) const { return {lat.x0 + static_cast<Coord>(ix) * pitch, lat.y0 + static_cast<Coord>(iy) * pitch}; }
  int to_ix(Coord x) const { return static_cast<int>(std::llround(static_cast<double>(x - lat.x0) / static_cast<double>(pitch))); }
  int to_iy(Coord y) const { return static_cast<int>(std::llround(static_cast<double>(y - lat.y0) / static_cast<double>(pitch))); }

  // ---- layer limits (doc 05 §29) ----
  std::vector<std::int64_t> layer_pm;  // per layer: track cost per mille; empty = every layer at the plain cost
  bool tracks_ok(int layer) const { return !(opt.no_track_layers & model::layer_bit(layer)); }
  // Cost of a track of plain cost `v` on `layer` (integer, so the same on every machine).
  std::int64_t on_layer(std::int64_t v, int layer) const { return layer_pm.empty() ? v : v * layer_pm[static_cast<std::size_t>(layer)] / 1000; }

  void setup() {
    obs = std::make_unique<Obstacles>(b, rules);
    obs->set_soft_zones(opt.soft_zones);
    if (opt.soft_zones) soft_fills = std::make_unique<drc::ZoneFills>(obs->copper());
    for (std::size_t n = 0; n < b.nets.size(); ++n)
      if (std::find(opt.first_nets.begin(), opt.first_nets.end(), b.nets[n].name) != opt.first_nets.end()) first_net_ids.insert(static_cast<NetId>(n));
    obs->set_vias_off_pads(opt.vias_off_pads, static_cast<Coord>(opt.vias_off_pads_below_mm * 1e6));
    if (opt.via_in_pad) find_inner_balls();
    use_cache = !obs->needs_exact_routing();
    nl = b.copper_count();
    blind_ok = opt.blind_vias && rules.minimums.allow_blind_buried_vias && nl > 2;
    micro_ok = opt.micro_vias && rules.minimums.allow_microvias && nl > 2;
    net_layers.assign(b.nets.size(), 0);
    net_vias.assign(b.nets.size(), 1);
    net_blind.assign(b.nets.size(), 1);
    net_micro.assign(b.nets.size(), 1);
    for (const auto& n : b.nets) {
      const auto i = static_cast<std::size_t>(n.id);
      if (i >= net_layers.size()) continue;
      for (int l = 0; l < nl; ++l)
        if (tracks_ok(l) && obs->rules().track_allowed(n.id, l)) net_layers[i] |= model::layer_bit(l);
      net_vias[i] = obs->rules().via_allowed(n.id) && !obs->rules().via_hole_rule_hits_own_tracks(n.id) ? 1 : 0;
      net_blind[i] = net_vias[i] && obs->rules().via_allowed(n.id, model::ViaType::Blind) ? 1 : 0;
      net_micro[i] = net_vias[i] && obs->rules().via_allowed(n.id, model::ViaType::Micro) ? 1 : 0;
    }
    // Factors below 1 are raised to 1: the octile and field heuristics assume no track is cheaper than its length.
    for (std::size_t l = 0; l < opt.layer_cost_pm.size() && l < static_cast<std::size_t>(nl); ++l)
      if (opt.layer_cost_pm[l] > 1000) {
        layer_pm.resize(static_cast<std::size_t>(nl), 1000);
        layer_pm[l] = opt.layer_cost_pm[l];
      }
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
      if (opt.pitch_scale_small != 1.0 && pts < 1e6) pitch = std::clamp<Coord>(static_cast<Coord>(static_cast<double>(pitch) * opt.pitch_scale_small) / 5'000 * 5'000, 25'000, 200'000);
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
    if (delta > 0 && soft_fills) note_plane_contacts(item);
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
  // Touching (1 nm) as in the initial connectivity. Items are never reused, so contacts are recorded once and
  // follow the item: `joined` reads them only for live connections, so a rip removes them.
  void note_plane_contacts(int item) {
    const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
    std::vector<int> roots;
    for (const auto& s : it.shapes)
      for (int zi : drc::zones_touching(obs->copper(), *soft_fills, obs->grid(), s, it.layers, 1)) {
        const auto& z = obs->copper().items[static_cast<std::size_t>(zi)];
        if (z.footprint >= 0 || z.net != it.net || b.zones[static_cast<std::size_t>(z.index)].teardrop) continue;
        roots.push_back(init_root[static_cast<std::size_t>(zi)]);
      }
    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    if (roots.empty()) return;
    if (plane_contacts.size() <= static_cast<std::size_t>(item)) plane_contacts.resize(static_cast<std::size_t>(item) + 1);
    plane_contacts[static_cast<std::size_t>(item)] = std::move(roots);
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

  // Gap between the copper of two pads (0 when they touch), by bisection on the exact closer_than predicate, to
  // 1/4096 of the centre distance. `centre` is the distance between the pad positions.
  double pad_gap(int pa, int pb, double centre) const {
    const auto& ia = obs->copper().items[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(pa)])];
    const auto& ib = obs->copper().items[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(pb)])];
    auto closer = [&](Coord t) {
      for (const auto& sa : ia.shapes)
        for (const auto& sb : ib.shapes)
          if (geom::closer_than(sa, sb, t)) return true;
      return false;
    };
    if (closer(1)) return 0;
    Coord lo = 1, hi = static_cast<Coord>(centre) + 2;  // not closer than lo; closer than hi (the centres are copper)
    if (!closer(hi)) return centre;
    for (int it = 0; it < 12 && hi - lo > 1; ++it) {
      const Coord mid = lo + (hi - lo) / 2;
      (closer(mid) ? hi : lo) = mid;
    }
    return static_cast<double>(lo);
  }

  // Can a connection from pad `p` end in zone fill item `z`? By a track on a layer both have copper on and the net
  // may use, or by a via of a type the net may place (a via may end in the net's own fill on any layer, §29).
  bool reaches(int p, int z) const {
    const auto& pad = b.pads[static_cast<std::size_t>(p)];
    if (nl > 1 && (blind_for(pad.net) || micro_for(pad.net) || (opt.allow_vias && vias_ok(pad.net)))) return true;
    const model::LayerMask shared = pad.copper & obs->copper().items[static_cast<std::size_t>(z)].layers;
    for (int l = 0; l < nl; ++l)
      if ((shared & model::layer_bit(l)) && layer_ok(pad.net, l)) return true;
    return false;
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
      } else if (it.kind == drc::ItemKind::Zone && it.footprint < 0 && !b.zones[static_cast<std::size_t>(it.index)].teardrop) {
        // A teardrop is track copper, not a plane: one left behind by deleted routing is no target.
        net_clusters[it.net][uf.find(static_cast<int>(i))].zones.push_back(static_cast<int>(i));
      }
    }
    std::vector<Connection> out;
    // With coupled pair routing the old centre distances stay: the plan of the nets around a pair decides
    // whether negotiation later splits it, and pair routing was tuned and tested on that plan (doc 05 §17).
    const bool by_gap = opt.plan_gap && !opt.diff_pairs && opt.pair_nets.empty();
    for (auto& [net, cl] : net_clusters) {
      if (!opt.only_net.empty() && b.nets[static_cast<std::size_t>(net)].name != opt.only_net) continue;
      if (std::find(opt.skip_nets.begin(), opt.skip_nets.end(), b.nets[static_cast<std::size_t>(net)].name) != opt.skip_nets.end()) continue;
      std::vector<Cluster> groups;
      bool has_pads = false;
      for (auto& [r, c] : cl) has_pads |= !c.pads.empty();
      for (auto& [r, c] : cl) {
        // Zone-only clusters (unused fills) are not targets on their own -- except with soft zones, where an inner
        // plane that no pad touches yet (all-SMD boards) is the net's plane: pads drop vias into it. Only for nets
        // with pads, not for slivers of a stale fill (under 1 mm^2), which a refill may drop or reshape, and only
        // for planes a pad of the net can reach: an unreachable plane would replace pad-to-pad connections.
        const bool plane = opt.soft_zones && has_pads && !c.zones.empty() && zone_fill_area(obs->copper(), c.zones) >= kPlaneTargetArea &&
                           std::any_of(cl.begin(), cl.end(), [&](const auto& o) {
                             return std::any_of(o.second.pads.begin(), o.second.pads.end(), [&](int p) {
                               return std::any_of(c.zones.begin(), c.zones.end(), [&](int z) { return reaches(p, z); });
                             });
                           });
        if (!c.pads.empty() || plane) groups.push_back(c);
      }
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
              const double centre = std::hypot(static_cast<double>(A.x - B.x), static_cast<double>(A.y - B.y));
              if (!by_gap) {
                consider(pa, pb, -1, centre);
                continue;
              }
              // Copper to copper, not centre to centre: a pin beside a large pad of its net (a QFN's exposed pad,
              // a wide power pad) is nearer to it than to the next pin of the row, and the pin-to-pin connection
              // may have to hop over a foreign pin between them. The centre distance breaks ties.
              if (centre * 1e-3 >= best[j]) continue;  // even touching copper would not beat the best so far
              consider(pa, pb, -1, pad_gap(pa, pb, centre) + centre * 1e-3);
            }
            for (int z : groups[j].zones)
              if (reaches(pa, z)) consider(pa, -1, z, zone_dist(A, z));
          }
          // And pads of cluster j into zones of the tree side.
          for (int pb : groups[j].pads)
            for (int z : groups[from].zones)
              if (reaches(pb, z)) consider(pb, -1, z, zone_dist(b.pads[static_cast<std::size_t>(pb)].pos, z));
        }
      };
      upd(0);
      for (std::size_t step = 1; step < k; ++step) {
        std::size_t nxt = k;
        for (std::size_t j = 0; j < k; ++j)
          if (!in[j] && (nxt == k || best[j] < best[nxt])) nxt = j;
        in[nxt] = 1;
        if (how[nxt].pad_a >= 0) out.push_back(how[nxt]);  // two zone-only clusters have nothing to join them directly
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
    if (!first_net_ids.empty())
      std::stable_partition(out.begin(), out.end(), [&](const Connection& c) { return first_net_ids.count(c.net) > 0; });
    return out;
  }

  // ---------------------------------------------------------------------------------------------------
  // A* on (layer, cell, arrival direction).
  // ---------------------------------------------------------------------------------------------------
  struct Endpoint {
    std::vector<std::pair<int, std::int64_t>> cells;  // (layer, cell index in window)
    std::vector<Point> stub;                           // per cell: off-lattice escape point (or the pad centre)
    std::vector<std::int64_t> cost;                    // per cell: cost from the pad centre
    std::size_t in_pad_from = SIZE_MAX;                // cells from here on are via-in-pad endpoints (add_in_pad)
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
      if (!(p.copper & model::layer_bit(l)) || !layer_ok(net, l)) continue;
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
  // Consecutive per-cell lookups usually use the same net and width.
  NetId cc_net = -1;
  Coord cc_key = -1;
  ClassCache* cc_last = nullptr;
  ClassCache& cache_for(NetId net) {
    const Coord key = track_width(net) * 2 + (via_override ? 1 : 0);  // via codes depend on the via size
    if (net == cc_net && key == cc_key) return *cc_last;
    auto& cc = caches[{&netclass(net), key}];
    if (cc.margin.empty()) {
      const std::size_t n = static_cast<std::size_t>(nl) * static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
      cc.margin.assign(n, INT32_MIN);
      cc.tight.assign(n, INT32_MIN);
      cc.via.assign(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), INT32_MIN);
      cc.rep = net;
    }
    cc_net = net;
    cc_key = key;
    cc_last = &cc;
    return cc;
  }
  static bool code_ok(std::int32_t code, NetId net) { return code == Obstacles::kFree || code == net; }

  // Blocked by fixed copper (or a positional disallow rule) alone (legal without the lattice margin otherwise)?
  bool fixed_point_blocked(int layer, int gx, int gy, NetId net, Coord hw) {
    const Point p = at(gx, gy);
    if (!use_cache) return obs->disk_state(p, layer, hw, net, 0, true) == 2;
    auto& cc = cache_for(net);
    const std::size_t gi = (static_cast<std::size_t>(layer) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(gy)) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(gx);
    if (cc.tight[gi] == INT32_MIN) cc.tight[gi] = obs->fixed_code(p, layer, hw, 0, cc.rep);
    return !code_ok(cc.tight[gi], net) || rule_state(p, layer, hw, 0, net) == 2;
  }

  // Positional disallow rules (doc 05 §34) are judged like fixed copper, outside the per-class caches (they may
  // name nets): 2 when they forbid the track's copper at p, 3 when they forbid only the disk with the lattice
  // margin (legal "tight"), else 0.
  geom::Shape rule_disk;
  int rule_state(Point p, int layer, Coord hw, Coord margin, NetId net) {
    if (!obs->positional_rules()) return 0;
    rule_disk.set_point(p, hw);
    if (obs->track_disallowed(rule_disk, layer, 2 * hw, net)) return 2;
    if (margin <= 0) return 0;
    rule_disk.set_point(p, hw + margin);
    return obs->track_disallowed(rule_disk, layer, 2 * hw, net) ? 3 : 0;
  }

  geom::Shape probe_disk;  // reused by point_state and via_cost_at
  // State of a lattice point for the current net: 0 free, 1 crosses routed copper (soft), 2 blocked,
  // 3 legal only without the lattice margin ("tight").
  int point_state(int layer, int gx, int gy, NetId net, Coord hw) {
    if (!layer_ok(net, layer)) return 2;
    const Point p = at(gx, gy);
    const Coord margin = pitch * 71 / 100 + 1;
    const int rs = rule_state(p, layer, hw, margin, net);
    if (rs == 2) return 2;
    if (!use_cache) {
      int st = rs == 3 ? 2 : obs->disk_state(p, layer, hw, net, margin, soft);
      if (st == 2 && obs->disk_state(p, layer, hw, net, 0, soft) != 2) st = 3;
      return st;
    }
    auto& cc = cache_for(net);
    const std::size_t gi = (static_cast<std::size_t>(layer) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(gy)) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(gx);
    if (cc.margin[gi] == INT32_MIN) cc.margin[gi] = obs->fixed_code(p, layer, hw, margin, cc.rep);
    int st = 0;
    if (!code_ok(cc.margin[gi], net) || rs == 3) {
      if (cc.tight[gi] == INT32_MIN) cc.tight[gi] = obs->fixed_code(p, layer, hw, 0, cc.rep);
      if (!code_ok(cc.tight[gi], net)) return 2;
      st = 3;
    }
    if (near_r[gi] == 0) return st;
    probe_disk.set_point(p, hw + (st == 3 ? 0 : margin));
    const int r = obs->routed_state(probe_disk, layer, net, drc::ItemKind::Track, soft, nullptr);
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
      cell_hist[idx] = cell_state[idx] == 2 ? 0 : hist_cost(layer, gx, gy);  // history only changes between searches
    }
    const int st = cell_state[idx];
    if (st == 2) return -1;
    const std::int64_t rc = reserved_cost(layer, gx, gy, net);
    if (rc < 0) return -1;
    std::int64_t hc = cell_hist[idx] + rc;
    if (corr || cong_pen > 0) {
      const Point p = at(gx, gy);
      const int tx = std::clamp(global.tile_of_x(p.x), 0, global.tiles_x - 1), ty = std::clamp(global.tile_of_y(p.y), 0, global.tiles_y - 1);
      const std::size_t ti = (static_cast<std::size_t>(layer) * static_cast<std::size_t>(global.tiles_y) + static_cast<std::size_t>(ty)) * static_cast<std::size_t>(global.tiles_x) + static_cast<std::size_t>(tx);
      if (corr && !(*corr)[ti]) {  // soft guidance: leaving the global corridor (or its layer) costs half a pitch per lattice step
        if (corr_hard) return -1;
        hc += corridor_pen;
      }
      // Congestion map: through traffic pays in tiles the global plan fills; a connection's own end tiles (and
      // their neighbours) are free, since it has to get in and out of them whatever the congestion.
      if (cong_pen > 0 && global.util[ti] > opt.global_congestion_from &&
          std::max(std::abs(tx - cong_end[0]), std::abs(ty - cong_end[1])) > 1 && std::max(std::abs(tx - cong_end[2]), std::abs(ty - cong_end[3])) > 1)
        hc += (global.util[ti] - opt.global_congestion_from) * cong_pen;
    }
    if (st == 1) return static_cast<std::int64_t>(opt.soft_cost_mm * 1e6) + hc * 4;
    if (st == 3) return 3 * pitch + hc;
    return hc;
  }
  std::int64_t via_cost_at(const Window& w, int cx, int cy, NetId net, Coord d, Coord drill) {
    if (!vias_ok(net)) return -1;
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
        // Positional rules are judged on the via itself (vias sit exactly on lattice points), as via_state does.
        st = code_ok(cc.via[gi], net) && !obs->via_disallowed(p, d, net, 0, nl - 1, model::ViaType::Through) ? 0 : 2;
        if (st != 2) {
          model::LayerMask near = 0;  // layers with routed copper nearby
          for (int l = 0; l < nl; ++l)
            if (near_r[(static_cast<std::size_t>(l) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(gy)) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(gx)] != 0)
              near |= model::layer_bit(l);
          probe_disk.set_point(p, d / 2 + margin);
          st = obs->routed_via_state(probe_disk, near, net, soft, drill / 2 + margin);
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
      if (!(p.copper & model::layer_bit(l)) || !layer_ok(p.net, l)) continue;
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
    const std::size_t n_own = e.cells.size();
    if (!any_free || force_escapes) add_escapes(w, pad, e);
    // Round pads (BGA/WLP balls) are offered a via in pad at a high cost: an inner ball can look free locally (a
    // diagonal gap) yet have no way out of the array.
    if (opt.via_in_pad && ((!any_free && e.cells.size() == n_own) || (p.shape == model::PadShape::Circle && inner_ball(pad)))) add_in_pad(w, pad, e);
    return e;
  }

  // Via in pad (--via-in-pad): the minimum via the rules allow, centred in a single-layer pad -- an inner ball of an
  // array, or a pad with no legal exit on its own layer. The pad centre becomes an endpoint on every other layer at
  // four times the via cost (a last resort); commit() adds the via. Needs filled and capped vias at the fab.
  // Sizes come from the board's minimums only; when they could not be read (0), no via in pad is placed (rule 6).
  Coord in_pad_drill() const { return rules.minimums.through_hole_diameter; }
  Coord in_pad_diameter() const { return std::max(rules.minimums.via_diameter, in_pad_drill() + 2 * rules.minimums.via_annular_width); }
  bool in_pad_sizes_known() const { return rules.minimums.through_hole_diameter > 0 && rules.minimums.via_diameter > 0; }
  // Inner balls (--via-in-pad), computed once per board: round single-layer pads with a pad of the same footprint
  // one ball pitch away in each of the four directions of the footprint's frame. The pitch is the footprint's
  // smallest distance between two round pads.
  std::vector<std::uint8_t> inner_ball_;
  void find_inner_balls() {
    inner_ball_.assign(b.pads.size(), 0);
    for (const auto& f : b.footprints) {
      std::vector<int> balls;
      for (int i : f.pads) {
        const auto& p = b.pads[static_cast<std::size_t>(i)];
        if (p.shape == model::PadShape::Circle && std::popcount(p.copper) == 1) balls.push_back(i);
      }
      if (balls.size() < 9) continue;  // a 3 x 3 array is the smallest with an inner ball
      double ball_pitch = 1e300;
      for (std::size_t x = 0; x < balls.size(); ++x)
        for (std::size_t y = x + 1; y < balls.size(); ++y) {
          const Point a = b.pads[static_cast<std::size_t>(balls[x])].pos, c = b.pads[static_cast<std::size_t>(balls[y])].pos;
          ball_pitch = std::min(ball_pitch, std::hypot(static_cast<double>(a.x - c.x), static_cast<double>(a.y - c.y)));
        }
      const double ang = f.angle * std::numbers::pi / 180.0, tol = ball_pitch / 10;
      const double ux = std::cos(ang) * ball_pitch, uy = -std::sin(ang) * ball_pitch;  // KiCad angles turn counter-clockwise, y down
      for (int i : balls) {
        const Point p = b.pads[static_cast<std::size_t>(i)].pos;
        int found = 0;
        for (const auto& [dx, dy] : {std::pair{ux, uy}, std::pair{-ux, -uy}, std::pair{-uy, ux}, std::pair{uy, -ux}})
          for (int o : balls) {
            const Point q = b.pads[static_cast<std::size_t>(o)].pos;
            if (std::hypot(static_cast<double>(q.x - p.x) - dx, static_cast<double>(q.y - p.y) - dy) < tol) {
              ++found;
              break;
            }
          }
        inner_ball_[static_cast<std::size_t>(i)] = found == 4;
      }
    }
  }
  bool inner_ball(int pad) const { return !inner_ball_.empty() && inner_ball_[static_cast<std::size_t>(pad)]; }
  bool in_pad_via_needed(int pad, int layer) const {
    return opt.via_in_pad && !(b.pads[static_cast<std::size_t>(pad)].copper & model::layer_bit(layer));
  }
  void add_in_pad(const Window& w, int pad, Endpoint& e) {
    const auto& p = b.pads[static_cast<std::size_t>(pad)];
    if (std::popcount(p.copper) != 1 || !in_pad_sizes_known()) return;  // through-hole pads already reach every layer
    // One via per pad: a via another connection already placed in this pad is not duplicated (that connection
    // leaves through it; this one must use the pad's own layer).
    std::vector<int> near;
    obs->routed_items_in(geom::Shape::point(p.pos, 1).box, near);
    for (int id : near) {
      const auto& it = obs->copper().items[static_cast<std::size_t>(id)];
      if (it.kind == drc::ItemKind::Via && it.net == p.net && it.pos == p.pos && it.owner != current) return;
    }
    obs->set_pad_via_exempt(true);
    const bool vip_blocked = obs->via_state(p.pos, in_pad_diameter(), in_pad_drill(), p.net, 0, soft) == 2;  // as commit() will check it
    obs->set_pad_via_exempt(false);
    if (std::getenv("TM_DEBUG_INPAD"))
      std::fprintf(stderr, "INPAD %s pad %d: via %.3f/%.3f %s\n", b.nets[static_cast<std::size_t>(p.net)].name.c_str(), pad, nm_to_mm(in_pad_diameter()),
                   nm_to_mm(in_pad_drill()), vip_blocked ? "illegal" : "offered");
    if (vip_blocked) return;
    const int gx = to_ix(p.pos.x), gy = to_iy(p.pos.y);
    if (gx < w.x0 || gy < w.y0 || gx >= w.x0 + w.w || gy >= w.y0 + w.h) return;
    const Point C = at(gx, gy);
    if (e.in_pad_from == SIZE_MAX) e.in_pad_from = e.cells.size();
    // Four vias' worth at the current via price (the clean-up stages raise via_cost_mult; a fixed price would make
    // the in-pad via the cheap option there).
    const std::int64_t cost = static_cast<std::int64_t>(4 * opt.via_cost_mm * via_cost_mult * 1e6) +
                              static_cast<std::int64_t>(std::hypot(static_cast<double>(C.x - p.pos.x), static_cast<double>(C.y - p.pos.y)));
    for (int l = 0; l < nl; ++l) {
      if ((p.copper & model::layer_bit(l)) || !layer_ok(p.net, l)) continue;
      e.cells.emplace_back(l, static_cast<std::int64_t>(gy - w.y0) * w.w + (gx - w.x0));
      e.stub.push_back(p.pos);
      e.cost.push_back(cost);
    }
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
    for (int l = 0; l < nl; ++l) {
      if (!layer_ok(net, l)) {  // disallowed layer: no track may run there
        std::fill_n(f_pass.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(l) * cells), cells, std::uint8_t{0});
        continue;
      }
      for (int cy = 0; cy < w.h; ++cy) {
        const std::size_t gbase = (static_cast<std::size_t>(l) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(w.y0 + cy)) * static_cast<std::size_t>(nx) +
                                  static_cast<std::size_t>(w.x0);
        std::uint8_t* row = &f_pass[static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(cy) * static_cast<std::size_t>(w.w)];
        for (int cx = 0; cx < w.w; ++cx) {
          const std::int32_t t = cc.tight[gbase + static_cast<std::size_t>(cx)];
          if (t != INT32_MIN && !code_ok(t, net)) row[cx] = 0;  // known blocked by fixed copper even without margin
        }
      }
    }
    if (!vias_ok(net)) std::fill(f_via.begin(), f_via.end(), std::uint8_t{0});
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
    const auto st = opt.gpu_device >= 0 ? gpu::field_gpu(opt.gpu_device, fp, field) : gpu::GpuStatus{false, "no GPU"};
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
  bool reach_check_now = false;  // run the reachability pre-check before the next search
  std::vector<std::uint32_t> rstamp;  // flood-fill visited marks (generation stamps)
  std::vector<std::size_t> reach_q;
  long reach_checks = 0, reach_pruned = 0, reach_visits = 0, reach_mismatch = 0;
  bool reach_said_no = false;  // this search's pre-check found no path
  long expansion_cap = 0;  // > 0: per-search expansion limit overriding opt.max_expansions (reverse probes)
  Miss last_miss = Miss::None;

  bool search(const Connection& c, const Window& w, std::vector<PathNode>& path) {
    last_miss = Miss::None;
    reach_said_no = false;
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
      cell_hist.resize(cells * static_cast<std::size_t>(nl));
    }
    if (vstamp.size() < cells) {
      vstamp.assign(cells, 0);
      via_state.resize(cells);
    }
    if (++gen > kGenMask) {
      std::fill(sn.begin(), sn.end(), SNode{0, 0, -1});
      std::fill(cstamp.begin(), cstamp.end(), 0u);
      std::fill(vstamp.begin(), vstamp.end(), 0u);
      std::fill(rstamp.begin(), rstamp.end(), 0u);
      gen = 1;
    }
    const Endpoint src = pad_cells(w, c.pad_a);
    const Endpoint dst = c.pad_b >= 0 ? pad_cells(w, c.pad_b) : Endpoint{};
    if (src.cells.empty() || (c.pad_b >= 0 && dst.cells.empty())) return false;
    std::vector<std::uint8_t> is_target(cells * static_cast<std::size_t>(nl), 0);  // 1 target, 2 not (zone checks are lazy)
    for (auto [l, ci] : dst.cells) is_target[static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(ci)] = 1;
    // Via-in-pad target cells carry their penalty (the source side pays it through src.cost): charged on entry.
    std::unordered_map<std::size_t, std::int64_t> target_extra;
    for (std::size_t k = dst.in_pad_from; k < dst.cells.size(); ++k)
      target_extra[static_cast<std::size_t>(dst.cells[k].first) * cells + static_cast<std::size_t>(dst.cells[k].second)] = dst.cost[k];
    auto textra = [&](std::size_t i) -> std::int64_t {
      if (target_extra.empty()) return 0;
      const auto it = target_extra.find(i);
      return it == target_extra.end() ? 0 : it->second;
    };
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
    // Reachability pre-check (doc 05 §13): a flood fill over the same legality tests as the A* below, but with no
    // bend states, turn limits or costs, so it admits every path the A* could find. "Unreachable" is therefore
    // exact, found with one visit per lattice point instead of up to nine heap-ordered expansions, and the cell
    // cache it fills is reused by the A* when a path exists. Run only where a failure is likely (reach_check_now).
    if (reach_check_now && !soft && !zone_target && c.pad_b >= 0) {
      const std::size_t lcells = cells * static_cast<std::size_t>(nl);
      if (rstamp.size() < lcells) rstamp.assign(lcells, 0);
      const std::size_t cap = static_cast<std::size_t>(expansion_cap > 0 ? expansion_cap : opt.max_expansions);
      reach_q.clear();
      for (const auto& [l, ci] : src.cells) {
        const std::size_t i = static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(ci);
        if (rstamp[i] == gen) continue;
        rstamp[i] = gen;
        reach_q.push_back(i);
      }
      bool found = false, edge = false;
      for (std::size_t head = 0; head < reach_q.size() && reach_q.size() <= cap; ++head) {
        const std::size_t i = reach_q[head];
        if (is_target[i] == 1) {
          found = true;
          break;
        }
        const int l = static_cast<int>(i / cells);
        const std::size_t ci = i % cells;
        const int cx = static_cast<int>(ci % static_cast<std::size_t>(w.w)), cy = static_cast<int>(ci / static_cast<std::size_t>(w.w));
        if (cx == 0 || cy == 0 || cx == w.w - 1 || cy == w.h - 1) edge = true;
        for (int d = 0; d < 8; ++d) {
          const int ncx = cx + kDx[d], ncy = cy + kDy[d];
          if (ncx < 0 || ncy < 0 || ncx >= w.w || ncy >= w.h) continue;
          const std::size_t ni = static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(ncy) * static_cast<std::size_t>(w.w) + static_cast<std::size_t>(ncx);
          if (rstamp[ni] == gen) continue;
          if (is_target[ni] != 1 && cell_cost(w, l, ncx, ncy, net, hw) < 0) continue;
          rstamp[ni] = gen;
          reach_q.push_back(ni);
        }
        // Layer change: a through via, or (optimistically) any blind/buried or micro via when those are allowed.
        auto via_useful = [&] {  // avoid legality checks when every usable layer is reached
          for (int l2 = 0; l2 < nl; ++l2)
            if (l2 != l && rstamp[static_cast<std::size_t>(l2) * cells + ci] != gen && layer_ok(net, l2)) return true;
          return false;
        };
        if (nl > 1 && via_useful() && (blind_for(net) || micro_for(net) || (opt.allow_vias && via_cost_at(w, cx, cy, net, vd, vdrill) >= 0)))
          for (int l2 = 0; l2 < nl; ++l2) {
            const std::size_t ni = static_cast<std::size_t>(l2) * cells + ci;
            if (l2 == l || rstamp[ni] == gen || !layer_ok(net, l2)) continue;
            rstamp[ni] = gen;
            reach_q.push_back(ni);
          }
      }
      const long visited = static_cast<long>(reach_q.size());
      res.expansions += visited;  // counted as work so --work budgets stay deterministic
      reach_visits += visited;
      ++reach_checks;
      if (!found && reach_q.size() <= cap) {
        ++reach_pruned;
        reach_said_no = true;
        // --reach-verify (reference check): run the A* anyway; a path it finds is a mismatch.
        if (!opt.reach_verify) {
          exp_fail += visited;
          ++n_fail;
          last_miss = edge ? Miss::Window : Miss::Enclosed;
          return false;
        }
      }
    }
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
        heat_exp.add(at(w.x0 + cx, w.y0 + cy), 64);  // one sample stands for the 64 expansions since the last
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
        } else {
          extra = textra(static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(nci));
        }
        std::int64_t cost = on_layer((d & 1) ? diag : step, l) + extra;
        if (dir != kNoDir && d != dir) cost += ((std::min((d - dir + 8) % 8, (dir - d + 8) % 8) == 1) ? step / 2 : 2 * step);
        const std::size_t ns = sidx(l, nci, d);
        if ((sn[ns].tag & kGenMask) == gen && sn[ns].g <= gs + cost) continue;
        const std::int64_t hn = h(l, w.x0 + ncx, w.y0 + ncy);
        if (hn >= kUnreachable) continue;  // cannot reach a target from there
        sn[ns] = SNode{gs + cost, gen | (static_cast<std::uint32_t>(d) << 28), static_cast<std::int32_t>(s)};
        open.emplace(gs + cost + hn, ns);
      }
      // Through via: skip legality checks if the bare via cost cannot improve another layer.
      // Extra via costs are nonnegative. With blind or micro vias the check always runs: they are tried only
      // where the through via is found blocked, so a skipped check would change which vias the search considers.
      auto via_useful = [&] {
        for (int l2 = 0; l2 < nl; ++l2) {
          if (l2 == l || (!layer_ok(net, l2) && !target(l2, ci))) continue;
          const std::size_t ns = sidx(l2, ci, kNoDir);
          if ((sn[ns].tag & kGenMask) != gen || sn[ns].g > gs + via_cost) return true;
        }
        return false;
      };
      const std::int64_t vextra = (opt.allow_vias && nl > 1 && (blind_for(net) || micro_for(net) || via_useful())) ? via_cost_at(w, cx, cy, net, vd, vdrill) : -1;
      if (vextra < 0 && (blind_for(net) || micro_for(net))) {
        // Through via blocked: a micro via to the next layer from an outer one, or a blind or buried via spanning
        // only the layers between (dearer: costs more to make).
        const Point vp = at(w.x0 + cx, w.y0 + cy);
        const Coord vm = pitch * 71 / 100 + 1;
        for (int l2 = 0; l2 < nl; ++l2) {
          // A layer without tracks is a landing only where the via itself ends the connection (in a zone fill).
          if (l2 == l || (!layer_ok(net, l2) && !target(l2, ci))) continue;
          int vs = 2;
          if (micro_for(net) && micro_span(l, l2))
            vs = obs->via_state_span(vp, micro_diameter(net), micro_drill(net), net, vm, soft, nullptr, std::min(l, l2), std::max(l, l2), model::ViaType::Micro);
          if (vs == 2 && blind_for(net)) vs = obs->via_state_span(vp, vd, vdrill, net, vm, soft, nullptr, std::min(l, l2), std::max(l, l2));
          if (vs == 2) continue;
          const std::size_t ns = sidx(l2, ci, kNoDir);
          const std::int64_t ng = gs + via_cost * 3 / 2 + (vs == 1 ? static_cast<std::int64_t>(opt.soft_cost_mm * 1e6) : 0) +
                                  textra(static_cast<std::size_t>(l2) * cells + static_cast<std::size_t>(ci));
          if ((sn[ns].tag & kGenMask) == gen && sn[ns].g <= ng) continue;
          const std::int64_t hn = h(l2, w.x0 + cx, w.y0 + cy);
          if (hn >= kUnreachable) continue;
          sn[ns] = SNode{ng, gen | (static_cast<std::uint32_t>(kNoDir) << 28), static_cast<std::int32_t>(s)};
          open.emplace(ng + hn, ns);
        }
      }
      if (vextra >= 0) {
        for (int l2 = 0; l2 < nl; ++l2) {
          // A layer without tracks is a landing only where the via itself ends the connection (in a zone fill).
          if (l2 == l || (!layer_ok(net, l2) && !target(l2, ci))) continue;
          const std::size_t ns = sidx(l2, ci, kNoDir);
          const std::int64_t ng = gs + via_cost + vextra + textra(static_cast<std::size_t>(l2) * cells + static_cast<std::size_t>(ci));
          if ((sn[ns].tag & kGenMask) == gen && sn[ns].g <= ng) continue;
          const std::int64_t hn = h(l2, w.x0 + cx, w.y0 + cy);
          if (hn >= kUnreachable) continue;
          sn[ns] = SNode{ng, gen | (static_cast<std::uint32_t>(kNoDir) << 28), static_cast<std::int32_t>(s)};
          open.emplace(ng + hn, ns);
        }
      }
    }
    if (reach_said_no && goal != SIZE_MAX) ++reach_mismatch;
    res.expansions += expanded;
    (goal == SIZE_MAX ? exp_fail : exp_ok) += expanded;
    if (goal == SIZE_MAX) (soft ? (corr_hard ? xf_soft_conf : xf_soft_wide) : (corr_hard ? xf_strict_conf : xf_strict_wide)) += expanded;
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
    // Vias in pad where the path starts or ends on a layer the pad is not on (only offered by add_in_pad).
    std::vector<Point> pad_vias;
    if (in_pad_via_needed(c.pad_a, path.front().layer)) pad_vias.push_back(pa);
    if (c.pad_b >= 0 && in_pad_via_needed(c.pad_b, path.back().layer)) pad_vias.push_back(pb);
    // The search keeps off layers without tracks; nothing may be committed there whatever produced the path.
    for (const auto& s : merged)
      if (!layer_ok(net, s.layer)) {
        commit_why = "track on a layer that takes no tracks";
        return false;
      }
    // Exact verification. In soft mode, conflicts with other connections' routed copper name the victims.
    bool ok = true;
    std::vector<int> victims;
    obs->set_pad_via_exempt(true);
    for (const Point v : pad_vias)
      if (obs->via_state(v, in_pad_diameter(), in_pad_drill(), net, 0, soft, &victims) == 2) ok = false;
    obs->set_pad_via_exempt(false);
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
    std::vector<std::uint8_t> via_micro(vias.size(), 0);
    for (std::size_t k = 0; k < vias.size(); ++k) {
      if (obs->via_state(vias[k], vd, vdrill, net, 0, soft, &victims) != 2) continue;
      if (micro_for(net) && micro_span(via_span[k].first, via_span[k].second) &&
          obs->via_state_span(vias[k], micro_diameter(net), micro_drill(net), net, 0, soft, &victims, via_span[k].first, via_span[k].second,
                              model::ViaType::Micro) != 2) {
        via_layers[k] = via_span[k];
        via_micro[k] = 1;
        continue;
      }
      if (blind_for(net) && obs->via_state_span(vias[k], vd, vdrill, net, 0, soft, &victims, via_span[k].first, via_span[k].second) != 2) {
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
      if (first_net_ids.count(cs[static_cast<std::size_t>(v)].c.net) && !first_net_ids.count(net)) {
        commit_why = "would rip a --first-nets connection";
        return false;
      }
    for (int v : victims)
      if (cs[static_cast<std::size_t>(v)].rips >= rip_cap) {
        commit_why = "would rip a connection already ripped too often";
        return false;
      }
    // Rip up victims, and raise history on the contested cells so later searches avoid them.
    for (const auto& s : merged) bump_history(s, net);  // before the rip, while the conflicts still exist
    for (int v : victims) {
      rip(v);
      // If the budget ends before the victim is tried again, the report must not show the reason of an attempt
      // that preceded its route.
      cs[static_cast<std::size_t>(v)].why = "ripped up by " + b.nets[static_cast<std::size_t>(net)].name + " and not routed again";
    }
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
      if (via_micro[k]) {
        v.size = micro_diameter(net);
        v.drill = micro_drill(net);
        v.type = model::ViaType::Micro;
        ++res.micro_vias;
      }
      b.vias.push_back(v);
      const int id = static_cast<int>(b.vias.size() - 1);
      st.items.push_back(obs->add_via(id, current));
      near_mark(st.items.back(), +1);
      if (!through && !via_micro[k]) ++res.blind_vias;
      emit_via_add(id);
    }
    for (const Point pv : pad_vias) {
      b.vias.push_back(model::Via{pv, in_pad_diameter(), in_pad_drill(), 0, nl - 1, model::ViaType::Through, net, false, sexpr::kNoNode});
      const int id = static_cast<int>(b.vias.size() - 1);
      st.items.push_back(obs->add_via(id, current));
      near_mark(st.items.back(), +1);
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
    const bool was_coupled = st.coupled;
    st.coupled = false;
    ++st.rips;
    ++res.rips;
    pending.push_back(v);
    if (was_coupled && !pair_partner.empty()) {  // the other half goes too: the pair is re-routed coupled or not at all
      const int p = pair_partner[static_cast<std::size_t>(v)];
      if (p >= 0 && cs[static_cast<std::size_t>(p)].coupled && cs[static_cast<std::size_t>(p)].routed) rip(p);
    }
    for (std::size_t k = 0; k < cs.size(); ++k)
      if (cs[k].implicit && cs[k].routed && cs[k].c.net == st.c.net) {
        cs[k].routed = cs[k].implicit = false;
        --res.routed;
        pending.push_back(static_cast<int>(k));
      }
  }

  long exp_ok = 0, exp_fail = 0, n_ok = 0, n_fail = 0;
  long xf_soft_conf = 0, xf_soft_wide = 0, xf_strict_conf = 0, xf_strict_wide = 0;  // failed-search expansions by kind
  std::string why, commit_why;
  bool strict_pass = false;
  // Nogoods (design doc 06 §3.3): (connection, soft, window signature) attempts that already failed. The
  // signature hashes the routed copper inside the window, so any relevant change re-enables the attempt.
  std::unordered_map<std::uint64_t, std::uint8_t> nogoods;
  GlobalResult global;               // corridors from the global router (empty when off)
  const std::vector<std::uint8_t>* corr = nullptr;  // corridor of the connection being searched
  Coord corridor_pen = 0;            // extra cost per lattice step outside the corridor
  bool corr_hard = false;            // confined attempt: cells outside the corridor are blocked
  Coord cong_pen = 0;                // congestion map: cost per lattice step per eighth of capacity (0 = off)
  std::array<int, 4> cong_end{};     // tiles of the two ends of the connection being searched
  long confined_ok = 0, confined_tried = 0;
  bool micro_ok = false;    // micro vias allowed (board setting, more than two layers)
  // A micro via joins an outer layer and the layer next to it (KiCad's definition).
  bool micro_span(int l0, int l1) const { return std::abs(l0 - l1) == 1 && (std::min(l0, l1) == 0 || std::max(l0, l1) == nl - 1); }
  Coord micro_diameter(NetId net) const { return std::max(netclass(net).uvia_diameter, rules.minimums.microvia_diameter); }
  Coord micro_drill(NetId net) const { return std::max(netclass(net).uvia_drill, rules.minimums.microvia_drill); }
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
    if (cong_pen > 0) {
      const Point pa = b.pads[static_cast<std::size_t>(c.pad_a)].pos, pb = c.pad_b >= 0 ? b.pads[static_cast<std::size_t>(c.pad_b)].pos : pa;
      cong_end = {global.tile_of_x(pa.x), global.tile_of_y(pa.y), global.tile_of_x(pb.x), global.tile_of_y(pb.y)};
    }
    corr = (opt.global_route && !global.corridor.empty() && current >= 0 && static_cast<std::size_t>(current) < global.corridor.size()) ? &global.corridor[static_cast<std::size_t>(current)] : nullptr;
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
    // Global router v2: first a search confined to the connection's corridor (much smaller than the usual
    // window on large boards), then, if it fails, the usual windows with the corridor as soft guidance.
    if (corr && opt.global_confine && current >= 0 && static_cast<std::size_t>(current) < global.corridor_box.size()) {
      const auto& cb = global.corridor_box[static_cast<std::size_t>(current)];
      Window w;
      w.x0 = std::clamp(to_ix(global.origin.x + static_cast<Coord>(cb[0]) * global.tile), 0, nx - 1);
      w.y0 = std::clamp(to_iy(global.origin.y + static_cast<Coord>(cb[1]) * global.tile), 0, ny - 1);
      const int x1 = std::clamp(to_ix(global.origin.x + static_cast<Coord>(cb[2] + 1) * global.tile), 0, nx - 1);
      const int y1 = std::clamp(to_iy(global.origin.y + static_cast<Coord>(cb[3] + 1) * global.tile), 0, ny - 1);
      w.w = x1 - w.x0 + 1;
      w.h = y1 - w.y0 + 1;
      if (w.w > 1 && w.h > 1 && !out_of_budget()) {
        ++confined_tried;
        corr_hard = true;
        // Strict searches in a corridor are capped: successful ones average ~50k expansions on logicbone, and a
        // corridor blocked by other routes is negotiation's job, not worth flooding (doc 05 §13).
        if (!soft && opt.global_strict_corridor_only) expansion_cap = 200'000;
        const bool found = search(c, w, path);
        expansion_cap = 0;
        corr_hard = false;
        if (found && commit(c, path)) {
          ++confined_ok;
          return true;
        }
        last_miss = Miss::None;  // an exhausted corridor says nothing about the pin being boxed in
        if (!soft && opt.global_strict_corridor_only) {
          why = "corridor blocked (left to negotiation)";
          last_miss = Miss::Window;
          return false;
        }
      }
    }
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
      // The cheap reachability check first: on every strict search (mode 2) or only on likely failures (mode 1:
      // a retry with a larger window, or a connection that has failed before).
      reach_check_now = opt.reach_check == 2 || (opt.reach_check == 1 && (attempt > 0 || cs[static_cast<std::size_t>(current)].fails > 0));
      const bool found_path = search(c, w, path);
      reach_check_now = false;
      if (!found_path) {
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
      for (int item : o.items)
        if (static_cast<std::size_t>(item) < plane_contacts.size())
          for (int root : plane_contacts[static_cast<std::size_t>(item)]) {
            const int u = find(x), v = find(root);
            if (u != v) up[u] = v;
          }
    }
    return find(ra) == find(rb);
  }

  std::deque<int> pending;

  // ---- live visualisation helpers ----
  bool live = false;  // a viewer wants transient messages
  std::array<std::pair<Point, int>, 96> recent{};
  std::size_t recent_n = 0;
  // Heatmap overlays (doc 09 §3, doc 13 `heatmap`): where the searches spent their expansions, and the PathFinder
  // history cost per cell. Filled only while a sink is attached and never read by the router.
  HeatGrid heat_exp, heat_hist;
  long heat_last = 0;  // res.expansions at the last heatmap message
  // Resent by work done, not by wall time, so a recording is the same for the same input and seed.
  long heat_every() const { return opt.work_budget > 0 ? std::max(200'000L, opt.work_budget / 40) : 2'000'000L; }
  void emit_heatmaps(bool force) {
    if (!opt.sink || !heat_exp.ready()) return;
    if (!force && res.expansions - heat_last < heat_every()) return;
    heat_last = res.expansions;
    emit(heat_exp.message("expansions", -1));
    heat_hist.clear();
    // Iterating the hash map is fine here: raise() keeps the maximum per cell, which does not depend on order.
    for (const auto& [key, hv] : history) {
      const int gx = static_cast<int>(key & 0xFFFFFF), gy = static_cast<int>((key >> 24) & 0xFFFFFF);
      if (gx < nx && gy < ny) heat_hist.raise(at(gx, gy), hv);
    }
    emit(heat_hist.message("history", -1));
  }
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
  // Differential pairs, version 2 (design doc 05 §9 phase 3 and §15; decision D50). The pair is searched as one
  // object: an A* over centreline states (layer, lattice point, direction, which half is on the left) whose moves
  // add a straight lattice step, a 45-degree turn followed by a straight run long enough for the inner track's miter,
  // or a coupled via pair (both halves jog out to the via spacing, change layer side by side and jog back). Each move
  // is checked exactly on the two offset tracks and vias it adds, so the coupled section keeps the pair's gap by
  // construction. The ends are joined to the pads by short uncoupled legs (straight, octilinear dog-legs, or entering
  // the pair from behind), validated lazily when the A* pops a start or goal candidate, and paid at twice the coupled
  // cost so the coupled section starts as close to the pads as the board allows. The finished pair is verified once
  // more (each half against the board, then the second half against the first) before it is committed; anything
  // that fails leaves both connections to ordinary routing.
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

  struct PairLeg {
    std::vector<Point> pts;  // pad centre -> coupled section end
    Coord w = 0;
    double len = 0;
  };
  struct PairHalf {
    NetId net = 0;
    std::vector<std::pair<int, PairSeg>> segs;  // (layer, segment)
    std::vector<Point> vias;
  };
  std::string pair_why;
  std::string pair_tag;  // TM_DEBUG_PAIRS: which unhandled situation the last pair was in
  std::vector<std::uint8_t> pad_routed_here;  // board pad -> an end of some connection (filled on first use)
  // Is the first unrouted connection at `pad` boxed in? A strict search from the pad (60k expansions at most) that
  // exhausts its open list inside the window proves it; any path, or a search cut short, says no.
  bool pin_boxed_in(int pad) {
    for (std::size_t ci = 0; ci < cs.size(); ++ci) {
      const auto& st = cs[ci];
      if (st.routed || (st.c.pad_a != pad && st.c.pad_b != pad)) continue;
      Connection r = st.c;
      if (r.pad_b == pad) std::swap(r.pad_a, r.pad_b);
      const Point a = b.pads[static_cast<std::size_t>(r.pad_a)].pos;
      const Point e = r.pad_b >= 0 ? b.pads[static_cast<std::size_t>(r.pad_b)].pos : a;
      const Coord m = 2'000'000 + r.length / 4;
      Window w;
      w.x0 = std::max(0, to_ix(std::min(a.x, e.x) - m));
      w.y0 = std::max(0, to_iy(std::min(a.y, e.y) - m));
      w.w = std::min(nx - 1, to_ix(std::max(a.x, e.x) + m)) - w.x0 + 1;
      w.h = std::min(ny - 1, to_iy(std::max(a.y, e.y) + m)) - w.y0 + 1;
      const bool saved_soft = soft;
      soft = false;
      expansion_cap = 60'000;
      std::vector<PathNode> path;
      const bool found = search(r, w, path);
      expansion_cap = 0;
      soft = saved_soft;
      return !found && last_miss == Miss::Enclosed;
    }
    return false;
  }

  // A leg from pad centre `p` to the coupled section's end `X` on `layer` whose last segment arrives along `a` (never
  // doubling back over the pair): straight, the two octilinear dog-legs, or the same to a point behind X followed by
  // a straight entry. Shortest legal candidate first, at the pair width, then at the neck-down width. `checks` counts
  // the exact segment checks made, legal or not: they are the pair search's work units for candidates (rule 5).
  std::optional<PairLeg> pair_leg(Point p, Point X, Dir2 a, Dir2 n, int layer, NetId net, Coord w, long& checks) {
    std::vector<std::vector<Point>> cand;
    auto with_dogs = [&](Point f, Point t, std::optional<Point> tail) {
      auto push = [&](std::vector<Point> v) {
        if (tail) v.push_back(*tail);
        std::vector<Point> u;
        for (const Point q : v)
          if (u.empty() || !(u.back() == q)) u.push_back(q);
        cand.push_back(std::move(u));
      };
      push({f, t});
      const Coord dx = t.x - f.x, dy = t.y - f.y, m = std::min(std::llabs(dx), std::llabs(dy));
      if (m > 0 && std::llabs(dx) != std::llabs(dy)) {
        push({f, Point{f.x + (dx > 0 ? m : -m), f.y + (dy > 0 ? m : -m)}, t});
        push({f, Point{t.x - (dx > 0 ? m : -m), t.y - (dy > 0 ? m : -m)}, t});
      }
    };
    with_dogs(p, X, std::nullopt);
    const double side_off = std::fabs(static_cast<double>(X.x - p.x) * n.x + static_cast<double>(X.y - p.y) * n.y);
    with_dogs(p, offset_point(X, a, -std::max(side_off, static_cast<double>(w))), X);
    auto length = [](const std::vector<Point>& v) {
      double l = 0;
      for (std::size_t i = 0; i + 1 < v.size(); ++i) l += std::hypot(static_cast<double>(v[i + 1].x - v[i].x), static_cast<double>(v[i + 1].y - v[i].y));
      return l;
    };
    std::vector<std::pair<double, std::size_t>> order;
    for (std::size_t i = 0; i < cand.size(); ++i) {
      const auto& v = cand[i];
      if (v.size() >= 2) {  // the last segment must not run against the pair's direction
        const Point q = v[v.size() - 2], e = v.back();
        if (static_cast<double>(e.x - q.x) * a.x + static_cast<double>(e.y - q.y) * a.y < 0) continue;
      }
      order.emplace_back(length(v), i);
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    const Coord neck = neck_width(net);
    for (const Coord ww : {w, neck > 0 && neck < w ? neck : Coord{0}}) {
      if (ww <= 0) continue;
      for (const auto& [len, i] : order) {
        const auto& v = cand[i];
        bool good = true;
        for (std::size_t k = 0; k + 1 < v.size() && good; ++k) {
          ++checks;
          good = obs->segment_state(v[k], v[k + 1], layer, ww, net, false, nullptr) == 0;
        }
        if (good) return PairLeg{v, ww, len};
      }
    }
    return std::nullopt;
  }

  // Do two legs of different halves keep the gap the rules require between the halves?
  bool legs_clear(const PairLeg& x, const PairLeg& y, Coord req) const {
    for (std::size_t i = 0; i + 1 < x.pts.size(); ++i)
      for (std::size_t j = 0; j + 1 < y.pts.size(); ++j)
        if (geom::seg_seg_closer(x.pts[i], x.pts[i + 1], y.pts[j], y.pts[j + 1], (x.w + y.w) / 2 + req)) return false;
    return true;
  }

  // The two halves of a built pair keep the gap the rules require between them (a leg can still cut across the other
  // half's coupled run near the far end of a short pair). Geometry only; the exact check follows.
  // `vm`: mask expansion of untented vias, which Obstacles adds between a via and other copper (once) or another via
  // (twice).
  static bool halves_clear(const PairHalf& x, const PairHalf& y, Coord req, Coord vd, Coord vm) {
    for (const auto& [la, sa] : x.segs)
      for (const auto& [lb, sb] : y.segs)
        if (la == lb && geom::seg_seg_closer(sa.a, sa.b, sb.a, sb.b, (sa.w + sb.w) / 2 + req)) return false;
    const Coord req_vt = vm > 0 ? std::max(req, vm + 1'000) : req, req_vv = vm > 0 ? std::max(req, 2 * vm + 1'000) : req;
    for (const PairHalf* h : {&x, &y}) {
      const PairHalf* o = h == &x ? &y : &x;
      for (const Point v : h->vias) {
        for (const auto& [l, sg] : o->segs)
          if (geom::point_seg_closer(v, sg.a, sg.b, vd / 2 + sg.w / 2 + req_vt)) return false;
        for (const Point w : o->vias)
          if (geom::point_seg_closer(v, w, w, vd + req_vv)) return false;
      }
    }
    return true;
  }

  void commit_half(int ci, const PairHalf& h, Coord vd, Coord vdrill) {
    auto& st = cs[static_cast<std::size_t>(ci)];
    for (const auto& [layer, s] : h.segs) {
      b.tracks.push_back(model::Track{s.a, s.b, s.w, layer, h.net, false, sexpr::kNoNode});
      const int id = obs->add_track(static_cast<int>(b.tracks.size() - 1), ci);
      near_mark(id, +1);
      st.items.push_back(id);
      emit_track_add(static_cast<int>(b.tracks.size() - 1));
    }
    for (const Point v : h.vias) {
      b.vias.push_back(model::Via{v, vd, vdrill, 0, nl - 1, model::ViaType::Through, h.net, false, sexpr::kNoNode});
      const int id = obs->add_via(static_cast<int>(b.vias.size() - 1), ci);
      near_mark(id, +1);
      st.items.push_back(id);
      emit_via_add(static_cast<int>(b.vias.size() - 1));
    }
  }
  bool half_legal(const PairHalf& h, Coord vd, Coord vdrill) const {
    const bool dbg = std::getenv("TM_DEBUG_PAIRS") != nullptr;
    for (const auto& [layer, s] : h.segs)
      if (!tracks_ok(layer) || obs->segment_state(s.a, s.b, layer, s.w, h.net, false, nullptr) != 0) {
        if (dbg)
          std::fprintf(stderr, "   pair half %s: segment fails: --pts %.4f %.4f %.4f %.4f --layer %d --width %.3f\n", b.nets[static_cast<std::size_t>(h.net)].name.c_str(),
                       nm_to_mm(s.a.x), nm_to_mm(s.a.y), nm_to_mm(s.b.x), nm_to_mm(s.b.y), layer, nm_to_mm(s.w));
        return false;
      }
    for (const Point v : h.vias)
      if (obs->via_state(v, vd, vdrill, h.net, 0, false, nullptr) != 0) {
        if (dbg) std::fprintf(stderr, "   pair half %s: via fails at %.4f %.4f\n", b.nets[static_cast<std::size_t>(h.net)].name.c_str(), nm_to_mm(v.x), nm_to_mm(v.y));
        return false;
      }
    return true;
  }

  bool route_pair(int ia, int ib) {
    const Connection& A = cs[static_cast<std::size_t>(ia)].c;
    Connection B = cs[static_cast<std::size_t>(ib)].c;
    auto d = [](Point u, Point v) { return std::hypot(static_cast<double>(u.x - v.x), static_cast<double>(u.y - v.y)); };
    auto pad = [&](int i) -> const model::Pad& { return b.pads[static_cast<std::size_t>(i)]; };
    if (d(pad(A.pad_a).pos, pad(B.pad_b).pos) + d(pad(A.pad_b).pos, pad(B.pad_a).pos) <
        d(pad(A.pad_a).pos, pad(B.pad_a).pos) + d(pad(A.pad_b).pos, pad(B.pad_b).pos))
      std::swap(B.pad_a, B.pad_b);
    const model::Pad &pa1 = pad(A.pad_a), &pa2 = pad(A.pad_b), &pb1 = pad(B.pad_a), &pb2 = pad(B.pad_b);
    model::LayerMask allowed = 0;  // layers both halves may use (custom disallow rules)
    for (int l = 0; l < nl; ++l)
      if (layer_ok(A.net, l) && layer_ok(B.net, l)) allowed |= model::layer_bit(l);
    const model::LayerMask s_layers = pa1.copper & pb1.copper & allowed, e_layers = pa2.copper & pb2.copper & allowed;
    // Diagnostics (TM_DEBUG_PAIRS): the two situations the coupled search does not handle (doc 05 §15, §24).
    pair_tag.clear();
    {
      const Point s{(pa1.pos.x + pb1.pos.x) / 2, (pa1.pos.y + pb1.pos.y) / 2}, e{(pa2.pos.x + pb2.pos.x) / 2, (pa2.pos.y + pb2.pos.y) / 2};
      const auto side = [&](Point p, Point q) { return geom::orient(s, e, Point{e.x + (q.x - p.x), e.y + (q.y - p.y)}); };
      const auto s1 = side(pa1.pos, pb1.pos), s2 = side(pa2.pos, pb2.pos);
      if ((s1 > 0 && s2 < 0) || (s1 < 0 && s2 > 0)) pair_tag += " [pin order mirrored between the ends]";
      for (std::size_t k = 0; k < cs.size(); ++k)
        if (static_cast<int>(k) != ia && static_cast<int>(k) != ib && cs[k].routed && !cs[k].items.empty() && (cs[k].c.net == A.net || cs[k].c.net == B.net) &&
            (cs[k].c.pad_a == A.pad_a || cs[k].c.pad_a == A.pad_b || cs[k].c.pad_b == A.pad_a || cs[k].c.pad_b == A.pad_b || cs[k].c.pad_a == B.pad_a ||
             cs[k].c.pad_a == B.pad_b || cs[k].c.pad_b == B.pad_a || cs[k].c.pad_b == B.pad_b)) {
          pair_tag += " [an end pad already carries routed copper]";
          break;
        }
    }
    if (!s_layers || !e_layers) { pair_why = "no common layer at an end"; return false; }
    // A pin of another net between the two pins of an end (the ground pin between P and N on HDMI connectors and
    // ICs) can be enclosed by the converging legs. Such pins are tested before and after the pair is committed; a
    // pair that boxes in a pin which could escape before is taken back (below).
    std::vector<int> straddled;
    if (pad_routed_here.size() != b.pads.size()) {
      pad_routed_here.assign(b.pads.size(), 0);
      for (const auto& st : cs)
        for (int pd : {st.c.pad_a, st.c.pad_b})
          if (pd >= 0) pad_routed_here[static_cast<std::size_t>(pd)] = 1;
    }
    for (const auto& [p1, p2] : {std::pair{&pa1, &pb1}, std::pair{&pa2, &pb2}}) {
      const double dx = static_cast<double>(p2->pos.x - p1->pos.x), dy = static_cast<double>(p2->pos.y - p1->pos.y), L2 = dx * dx + dy * dy;
      for (const int fp : {p1->footprint, p2->footprint}) {
        if (fp < 0 || L2 <= 0) continue;
        for (const int pi : b.footprints[static_cast<std::size_t>(fp)].pads) {
          const auto& q = b.pads[static_cast<std::size_t>(pi)];
          if (q.net == A.net || q.net == B.net || q.net == 0 || !pad_routed_here[static_cast<std::size_t>(pi)] || !(q.copper & p1->copper & p2->copper)) continue;
          const double qx = static_cast<double>(q.pos.x - p1->pos.x), qy = static_cast<double>(q.pos.y - p1->pos.y);
          const double t = (qx * dx + qy * dy) / L2;
          if (t <= 0.05 || t >= 0.95 || std::fabs(qx * dy - qy * dx) / std::sqrt(L2) > 0.3 * std::sqrt(L2)) continue;
          if (std::find(straddled.begin(), straddled.end(), pi) == straddled.end()) straddled.push_back(pi);
        }
      }
    }
    std::vector<std::uint8_t> boxed_before;
    for (const int pi : straddled) boxed_before.push_back(pin_boxed_in(pi) ? 1 : 0);
    const PairRule pr = pair_rule(b, rules, obs->rules(), A.net, B.net, obs->via_mask());
    const Coord w = pr.width;
    const double off = static_cast<double>(pr.offset);
    const Point S{(pa1.pos.x + pb1.pos.x) / 2, (pa1.pos.y + pb1.pos.y) / 2};
    const Point E{(pa2.pos.x + pb2.pos.x) / 2, (pa2.pos.y + pb2.pos.y) / 2};
    const double se = d(S, E);
    if (se < 1'500'000) { pair_why = "too short to couple"; return false; }
    // Where the coupled section may start and end: within Rs / Re of the pad midpoints (legs are uncoupled length),
    // farther where the two pads lie far apart (pins with another pin between them).
    auto reach = [&](Point p1, Point p2) {
      double r = std::clamp(std::max(0.3 * se, 1.5 * d(p1, p2)), 600'000.0, 2'500'000.0);
      if (pr.max_uncoupled) r = std::min(r, std::max(static_cast<double>(*pr.max_uncoupled) / 2, 2.0 * static_cast<double>(pitch)));
      return r;
    };
    const double Rs = reach(pa1.pos, pb1.pos), Re = reach(pa2.pos, pb2.pos);
    // A turn is followed by a straight run of K lattice steps: the inner track's miter cuts its run back by
    // off * tan(22.5 deg) at each end, so shorter runs would fold the inner track over itself.
    const double fold = 2.0 * 0.41422 * off + static_cast<double>(w);
    const int K = std::max(1, static_cast<int>(std::ceil(fold / static_cast<double>(pitch))));
    const double hj = static_cast<double>(pr.via_offset) - off;  // via jog: lateral (and forward) distance
    const int MV = std::max(1, static_cast<int>(std::ceil((2.0 * hj + fold) / static_cast<double>(pitch))));
    // Twist (doc 05 §24): the halves change layer one after the other and cross in between, while they are on
    // different layers, so the half that was on the left continues on the right. In the frame of the pair's
    // direction (x along, y to the left), with D = off + via_offset and lead-in and lead-out of D / 2:
    //   right half: (0, -off) -> via at (hj, -vo) -> on the new layer (hj + D/2, -vo) -> (hj + 3D/2, +off) -> on
    //   left half:  (0, +off) -> (hj + D/2, +off) -> (hj + 3D/2, -vo) -> via at (hj + 2D, -vo) -> new layer (2hj + 2D, -off)
    // Each track keeps at least D (the distance the coupled via pair keeps) from the other half's via.
    const double twD = off + static_cast<double>(pr.via_offset);
    const double twT = 2.0 * hj + 2.0 * twD;
    const int MT = std::max(1, static_cast<int>(std::ceil((twT + fold) / static_cast<double>(pitch))));
    geom::Box box;
    box.add(S);
    box.add(E);
    box = box.inflated(3'000'000 + static_cast<Coord>(se / 4));
    const int wx0 = std::max(0, to_ix(box.x0)), wy0 = std::max(0, to_iy(box.y0));
    const int wx1 = std::min(nx - 1, to_ix(box.x1)), wy1 = std::min(ny - 1, to_iy(box.y1));
    auto in_win = [&](int gx, int gy) { return gx >= wx0 && gy >= wy0 && gx <= wx1 && gy <= wy1; };
    const std::int64_t step = pitch, diag = static_cast<std::int64_t>(std::llround(static_cast<double>(pitch) * std::numbers::sqrt2));
    auto steplen = [&](int dd, int l) { return on_layer((dd & 1) ? diag : step, l); };
    // A turn costs as much as its straight run again: without it the weighted search prefers staircases of 45-degree
    // turns that follow the straight line to the goal over one diagonal and one straight run.
    const std::int64_t bend_cost = static_cast<std::int64_t>(K) * pitch;
    const std::int64_t via_pair_cost = 2 * static_cast<std::int64_t>(opt.via_cost_mm * 1e6);
    const bool vias_ok = opt.allow_vias && nl > 1 && this->vias_ok(A.net) && this->vias_ok(B.net);
    // Cost-to-go: the centreline still has to get within reach of the end pads (legs cost more than coupled track,
    // so the straight distance is a lower bound), weighted a little for focus (pairs need not be optimal, only legal).
    const double reach_e = std::max(d(E, pa2.pos), d(E, pb2.pos)) + off;
    // Off the end pads' layers a coupled via pair is still to come.
    auto hcost = [&](Point c, int l) {
      return static_cast<std::int64_t>(2.0 * std::max(0.0, d(c, E) - reach_e)) + ((e_layers & model::layer_bit(l)) ? 0 : via_pair_cost);
    };
    // Which net runs on the side `sigma` (+1 left, -1 right) of a state with side bit s (0: A on the left).
    auto net_of = [&](double sigma, int s) { return (sigma > 0) == (s == 0) ? A.net : B.net; };
    auto key = [&](int l, int gx, int gy, int dd, int s) {
      return ((static_cast<std::uint64_t>(l) * static_cast<std::uint64_t>(ny) + static_cast<std::uint64_t>(gy)) * static_cast<std::uint64_t>(nx) +
              static_cast<std::uint64_t>(gx)) * 16u + static_cast<std::uint64_t>(dd * 2 + s);
    };
    struct Dec { int l, gx, gy, dd, s; };
    auto decode = [&](std::uint64_t k) {
      Dec x;
      x.s = static_cast<int>(k & 1u);
      x.dd = static_cast<int>((k >> 1) & 7u);
      std::uint64_t r = k / 16u;
      x.gx = static_cast<int>(r % static_cast<std::uint64_t>(nx));
      r /= static_cast<std::uint64_t>(nx);
      x.gy = static_cast<int>(r % static_cast<std::uint64_t>(ny));
      x.l = static_cast<int>(r / static_cast<std::uint64_t>(ny));
      return x;
    };
    enum : std::uint8_t { kStart = 0, kStraight = 1, kTurn = 2, kVia = 3, kTwist = 4 };
    struct PN {
      std::int64_t g;
      std::uint64_t parent;
      std::int32_t start;
      std::uint8_t move;
      bool closed;
    };
    std::unordered_map<std::uint64_t, PN> nodes;
    nodes.reserve(1u << 15);
    std::unordered_map<std::uint64_t, std::uint8_t> straight_ok;  // (layer, cell reached, dir, side) -> 1 legal, 2 not
    struct Cand {
      int l, gx, gy, dd, s;
      std::int64_t g = 0;   // start: leg cost; goal: estimated total cost
      bool checked = false;  // start legs validated
      PairLeg la, lb;       // legs of net A and net B
      std::uint64_t node = 0;
    };
    std::vector<Cand> starts, goals;
    int dbg_legs = std::getenv("TM_DEBUG_PAIRS") ? 6 : 0;
    struct QE {
      std::int64_t f, seq;
      std::uint8_t kind;  // 0 state, 1 start candidate, 2 goal candidate
      std::uint64_t id;
      bool operator>(const QE& o) const { return f != o.f ? f > o.f : seq > o.seq; }
    };
    std::priority_queue<QE, std::vector<QE>, std::greater<>> pq;
    std::int64_t seq = 0;
    const std::int64_t leg_weight = 2;
    // Start candidates: every lattice point within R of the start pads' midpoint, each direction, the side
    // assignment with the shorter legs (crossing legs are never shorter).
    for (int l = 0; l < nl; ++l) {
      if (!(s_layers & model::layer_bit(l))) continue;
      const int gx0 = std::max(wx0, to_ix(S.x - static_cast<Coord>(Rs))), gx1 = std::min(wx1, to_ix(S.x + static_cast<Coord>(Rs)));
      const int gy0 = std::max(wy0, to_iy(S.y - static_cast<Coord>(Rs))), gy1 = std::min(wy1, to_iy(S.y + static_cast<Coord>(Rs)));
      for (int gy = gy0; gy <= gy1; ++gy)
        for (int gx = gx0; gx <= gx1; ++gx) {
          const Point c = at(gx, gy);
          if (d(c, S) > Rs) continue;
          for (int dd = 0; dd < 8; ++dd) {
            const Dir2 n = left_normal(dd);
            const Point L = offset_point(c, n, off), Rt = offset_point(c, n, -off);
            const double c0 = d(pa1.pos, L) + d(pb1.pos, Rt), c1 = d(pa1.pos, Rt) + d(pb1.pos, L);
            Cand cd;
            cd.l = l, cd.gx = gx, cd.gy = gy, cd.dd = dd, cd.s = c0 <= c1 ? 0 : 1;
            cd.g = leg_weight * static_cast<std::int64_t>(std::min(c0, c1));
            pq.push({cd.g + hcost(c, l), seq++, 1, starts.size()});
            starts.push_back(std::move(cd));
          }
        }
    }
    // Work units of this attempt (rule 5): closed states and exact leg checks of start and goal candidates. Rejected
    // candidates are not free: with every leg blocked by fill polygons an attempt once ran minutes at zero counted work.
    long expanded = 0, leg_checks = 0, charged = 0;
    // Charges the work done since the last call to the router's total, so the work budget sees it during the attempt.
    auto charge = [&] {
      const long now = expanded + leg_checks;
      res.expansions += now - charged;
      res.pair_work += now - charged;
      charged = now;
    };
    // Legs of a candidate: start legs arrive along the pair's direction; end legs leave along it (built pad -> X
    // and arriving against the direction).
    auto make_legs = [&](Cand& cd, bool at_start) {
      const Point c = at(cd.gx, cd.gy);
      const Dir2 u = unit_dir(cd.dd), n = left_normal(cd.dd);
      const Dir2 a = at_start ? u : Dir2{-u.x, -u.y};
      const Point XA = offset_point(c, n, cd.s == 0 ? off : -off), XB = offset_point(c, n, cd.s == 0 ? -off : off);
      // The pads lie behind the start of the coupled section and ahead of its end (or level with it): otherwise the
      // legs would double back along the pair.
      for (const auto& [pad_pt, X] : {std::pair{(at_start ? pa1 : pa2).pos, XA}, std::pair{(at_start ? pb1 : pb2).pos, XB}}) {
        const double ahead = static_cast<double>(pad_pt.x - X.x) * u.x + static_cast<double>(pad_pt.y - X.y) * u.y;
        if ((at_start ? -ahead : ahead) < -static_cast<double>(w) / 2) return false;
      }
      const auto la = pair_leg((at_start ? pa1 : pa2).pos, XA, a, n, cd.l, A.net, w, leg_checks);
      const auto lb = la ? pair_leg((at_start ? pb1 : pb2).pos, XB, a, n, cd.l, B.net, w, leg_checks) : std::nullopt;
      if (dbg_legs > 0 && !at_start && (!la || !lb || !legs_clear(*la, *lb, pr.required_gap))) {
        --dbg_legs;
        const Point pA = (at_start ? pa1 : pa2).pos, pB = (at_start ? pb1 : pb2).pos;
        std::fprintf(stderr, "   end legs fail (%s): dir %d layer %d c (%.3f,%.3f) padA (%.3f,%.3f) XA (%.3f,%.3f) padB (%.3f,%.3f) XB (%.3f,%.3f)\n",
                     !la ? "A" : !lb ? "B" : "clear", cd.dd, cd.l, nm_to_mm(c.x), nm_to_mm(c.y), nm_to_mm(pA.x), nm_to_mm(pA.y), nm_to_mm(XA.x), nm_to_mm(XA.y),
                     nm_to_mm(pB.x), nm_to_mm(pB.y), nm_to_mm(XB.x), nm_to_mm(XB.y));
      }
      if (!la || !lb || !legs_clear(*la, *lb, pr.required_gap)) return false;
      // Each leg against the other half's straight run next to the candidate (K steps are straight at both ends).
      const double run = static_cast<double>(K * pitch) * (at_start ? 1.0 : -1.0);
      if (!legs_clear(*la, PairLeg{{XB, offset_point(XB, u, run)}, w, 0}, pr.required_gap) ||
          !legs_clear(*lb, PairLeg{{XA, offset_point(XA, u, run)}, w, 0}, pr.required_gap))
        return false;
      cd.la = *la;
      cd.lb = *lb;
      return true;
    };
    auto relax = [&](std::uint64_t k, std::int64_t g, std::uint64_t parent, std::int32_t start, std::uint8_t move, Point c, int l) {
      auto [it, fresh] = nodes.try_emplace(k, PN{g, parent, start, move, false});
      if (!fresh) {
        if (it->second.closed || it->second.g <= g) return;
        it->second = PN{g, parent, start, move, false};
      }
      pq.push({g + hcost(c, l), seq++, 0, k});
    };
    auto seg_ok = [&](Point p, Point q, int l, NetId net) { return p == q || (layer_ok(net, l) && obs->segment_state(p, q, l, w, net, false, nullptr) == 0); };
    auto delta = [&](int dd, int k, int& gx, int& gy) {
      gx += k * kDx[dd];
      gy += k * kDy[dd];
    };
    // Reconstructs the two halves of a found pair (each half's tracks per layer and its vias).
    auto build = [&](const Cand& goal, PairHalf& ha, PairHalf& hb) {
      std::vector<std::uint64_t> chain;
      for (std::uint64_t k = goal.node;; k = nodes.at(k).parent) {
        chain.push_back(k);
        if (nodes.at(k).move == kStart) break;
      }
      std::reverse(chain.begin(), chain.end());
      const Cand& st0 = starts[static_cast<std::size_t>(nodes.at(chain.front()).start)];
      for (const double sigma : {1.0, -1.0}) {
        const NetId net = net_of(sigma, st0.s);
        PairHalf& h = net == A.net ? ha : hb;
        h.net = net;
        const PairLeg& ls = net == A.net ? st0.la : st0.lb;
        const PairLeg& le = net == A.net ? goal.la : goal.lb;
        int layer = st0.l;
        double sg = sigma;  // the side this half is on; a twist changes it
        Point cur = ls.pts.front();
        auto to = [&](Point q, Coord ww) {
          if (q == cur) return;
          h.segs.push_back({layer, PairSeg{cur, q, ww}});
          cur = q;
        };
        for (std::size_t i = 1; i < ls.pts.size(); ++i) to(ls.pts[i], ls.w);
        for (std::size_t i = 1; i < chain.size(); ++i) {
          const Dec p = decode(chain[i - 1]), q = decode(chain[i]);
          const Point cp = at(p.gx, p.gy), cq = at(q.gx, q.gy);
          const std::uint8_t mv = nodes.at(chain[i]).move;
          // Corners only: a node's own offset point is emitted where the track leaves the straight line (a via) and
          // at the end; emitting it before an inner miter would fold the track back over itself.
          if (mv == kTurn) to(miter_point(cp, p.dd, q.dd, sg * off), w);
          if (mv == kVia) {
            const Dir2 u = unit_dir(p.dd), n = left_normal(p.dd);
            const Point V = offset_point(offset_point(cp, u, hj), n, sg * static_cast<double>(pr.via_offset));
            to(offset_point(cp, n, sg * off), w);
            to(V, w);
            h.vias.push_back(V);
            layer = q.l;
            to(offset_point(offset_point(cp, u, 2 * hj), n, sg * off), w);
          }
          if (mv == kTwist) {
            const Dir2 u = unit_dir(p.dd), n = left_normal(p.dd);
            auto pt = [&](double x, double y) { return offset_point(offset_point(cp, u, x), n, y); };
            const double vo = static_cast<double>(pr.via_offset);
            if (sg > 0) {  // the left half: crosses on the old layer, changes layer last
              to(pt(0, off), w);
              to(pt(hj + twD / 2, off), w);
              to(pt(hj + 1.5 * twD, -vo), w);
              const Point V = pt(hj + 2.0 * twD, -vo);
              to(V, w);
              h.vias.push_back(V);
              layer = q.l;
              to(pt(twT, -off), w);
            } else {  // the right half: changes layer first, crosses on the new layer
              to(pt(0, -off), w);
              const Point V = pt(hj, -vo);
              to(V, w);
              h.vias.push_back(V);
              layer = q.l;
              to(pt(hj + twD / 2, -vo), w);
              to(pt(hj + 1.5 * twD, off), w);
              to(pt(twT, off), w);
            }
            sg = -sg;
          }
          if (i + 1 == chain.size()) to(offset_point(cq, left_normal(q.dd), sg * off), w);
        }
        for (std::size_t i = le.pts.size(); i-- > 1;) to(le.pts[i - 1], le.w);
        // Merge collinear runs of one layer and width.
        std::vector<std::pair<int, PairSeg>> merged;
        for (const auto& [l, s] : h.segs) {
          if (!merged.empty()) {
            auto& [ml, m] = merged.back();
            if (ml == l && m.w == s.w && m.b == s.a && geom::orient(m.a, m.b, s.b) == 0 &&
                (m.b.x - m.a.x) * (s.b.x - s.a.x) + (m.b.y - m.a.y) * (s.b.y - s.a.y) > 0) {
              m.b = s.b;
              continue;
            }
          }
          merged.push_back({l, s});
        }
        h.segs = std::move(merged);
      }
    };

    // Search budget: in proportion to the pair's length in lattice steps (short pairs that cannot couple fail fast).
    // Leg checks are not capped here (a pair may need many rejected candidates before a legal one; capping them lost
    // coupled pairs at a fixed budget): they are charged to the router's work, so its budget and time limit end them.
    const long cap = std::clamp(static_cast<long>(600.0 * se / static_cast<double>(pitch)), 40'000L, 250'000L);
    long dbg_start_ok = 0, dbg_goal_ok = 0, iterations = 0;
    bool stopped = false;  // the router's work budget or time limit ran out during the attempt
    double dbg_closest = 1e300;
    int finals = 0;
    pair_why = "no coupled path";
    bool done = false;
    PairHalf ha, hb;
    // Goal candidates wait in their own queue (by estimated total cost) and are checked in turn with the states: when
    // the best of them is no dearer than the best state, and at least every 16 expansions, so a found end is used
    // as soon as its legs are legal (the first legal goal is taken: pairs need to be legal, not optimal).
    std::priority_queue<QE, std::vector<QE>, std::greater<>> gq;
    int since_goal = 0;
    while (expanded < cap && !done) {
      // Every 64 steps (one candidate or state each) the attempt's work joins the total and the budget is checked:
      // deterministic under a work budget, and the time limit stops a long attempt instead of waiting for it.
      if ((++iterations & 63) == 0) {
        charge();
        if (out_of_budget()) {
          stopped = true;
          break;
        }
      }
      if (!gq.empty() && (pq.empty() || gq.top().f <= pq.top().f || since_goal >= 16)) {
        since_goal = 0;
        const QE q = gq.top();
        gq.pop();
        Cand& cd = goals[static_cast<std::size_t>(q.id)];
        if (!make_legs(cd, false)) continue;
        ++dbg_goal_ok;
        if (pr.max_uncoupled) {
          std::uint64_t k = cd.node;
          while (nodes.at(k).move != kStart) k = nodes.at(k).parent;
          const Cand& s0 = starts[static_cast<std::size_t>(nodes.at(k).start)];
          const double lim = static_cast<double>(*pr.max_uncoupled);
          if (s0.la.len + cd.la.len > lim || s0.lb.len + cd.lb.len > lim) continue;
        }
        ha = PairHalf{};
        hb = PairHalf{};
        build(cd, ha, hb);
        if (!halves_clear(ha, hb, pr.required_gap, pr.via_diameter, obs->via_mask())) continue;
        if (half_legal(ha, pr.via_diameter, pr.via_drill) && half_legal(hb, pr.via_diameter, pr.via_drill)) {
          commit_half(ia, ha, pr.via_diameter, pr.via_drill);
          if (half_legal(hb, pr.via_diameter, pr.via_drill)) {  // now against the first half too
            commit_half(ib, hb, pr.via_diameter, pr.via_drill);
            done = true;
            break;
          }
          lift_items(ia);  // the second half clashes with the first: take the first back
        }
        pair_why = "exact check of the finished pair";
        if (++finals >= 4) break;
        continue;
      }
      if (pq.empty()) break;
      const QE q = pq.top();
      pq.pop();
      if (q.kind == 1) {  // start candidate: legs checked when first popped
        Cand& cd = starts[static_cast<std::size_t>(q.id)];
        const Point c = at(cd.gx, cd.gy);
        if (!cd.checked) {
          cd.checked = true;
          if (!make_legs(cd, true)) continue;
          ++dbg_start_ok;
          const std::int64_t real = leg_weight * static_cast<std::int64_t>(cd.la.len + cd.lb.len);
          if (real > cd.g) {
            cd.g = real;
            pq.push({cd.g + hcost(c, cd.l), seq++, 1, q.id});
            continue;
          }
        }
        relax(key(cd.l, cd.gx, cd.gy, cd.dd, cd.s), cd.g, 0, static_cast<std::int32_t>(q.id), kStart, c, cd.l);
        continue;
      }
      auto it = nodes.find(q.id);
      PN& nd = it->second;
      const Dec x = decode(q.id);
      const Point c = at(x.gx, x.gy);
      if (nd.closed || q.f - hcost(c, x.l) != nd.g) continue;
      nd.closed = true;
      ++expanded;
      ++since_goal;
      dbg_closest = std::min(dbg_closest, d(c, E));
      const std::int64_t g = nd.g;
      const std::uint8_t mv0 = nd.move;
      const Dir2 u = unit_dir(x.dd), n = left_normal(x.dd);
      // Goal candidate: near the end pads, on a layer both have, after at least one coupled move.
      if (mv0 != kStart && (e_layers & model::layer_bit(x.l)) && d(c, E) <= Re) {
        const Point L = offset_point(c, n, off), Rt = offset_point(c, n, -off);
        const Point la = x.s == 0 ? L : Rt, lb = x.s == 0 ? Rt : L;
        Cand cd;
        cd.l = x.l, cd.gx = x.gx, cd.gy = x.gy, cd.dd = x.dd, cd.s = x.s, cd.node = q.id;
        cd.g = g + leg_weight * static_cast<std::int64_t>(d(la, pa2.pos) + d(lb, pb2.pos));
        gq.push({cd.g, seq++, 2, goals.size()});
        goals.push_back(std::move(cd));
      }
      // Straight: one lattice step (K steps right after the start, so a turn cannot fold back over the legs).
      {
        const int k = mv0 == kStart ? K : 1;
        int gx = x.gx, gy = x.gy;
        delta(x.dd, k, gx, gy);
        if (in_win(gx, gy) && obs->inside_board(at(gx, gy), 0)) {
          const std::uint64_t nk = key(x.l, gx, gy, x.dd, x.s);
          bool good;
          if (k == 1) {
            auto& memo = straight_ok[nk];
            if (memo == 0) {
              const Point c2 = at(gx, gy);
              memo = seg_ok(offset_point(c, n, off), offset_point(c2, n, off), x.l, net_of(1, x.s)) &&
                             seg_ok(offset_point(c, n, -off), offset_point(c2, n, -off), x.l, net_of(-1, x.s))
                         ? 1
                         : 2;
            }
            good = memo == 1;
          } else {
            const Point c2 = at(gx, gy);
            good = seg_ok(offset_point(c, n, off), offset_point(c2, n, off), x.l, net_of(1, x.s)) &&
                   seg_ok(offset_point(c, n, -off), offset_point(c2, n, -off), x.l, net_of(-1, x.s));
          }
          if (good) relax(nk, g + k * steplen(x.dd, x.l), q.id, -1, kStraight, at(gx, gy), x.l);
        }
      }
      // 45-degree turns, each followed by K straight steps.
      if (mv0 != kStart)
        for (const int t : {1, 7}) {
          const int d2 = (x.dd + t) & 7;
          int gx = x.gx, gy = x.gy;
          delta(d2, K, gx, gy);
          if (!in_win(gx, gy) || !obs->inside_board(at(gx, gy), 0)) continue;
          const Point c2 = at(gx, gy);
          const Dir2 n2 = left_normal(d2);
          bool good = true;
          for (const double sg : {1.0, -1.0}) {
            if (!good) break;
            const NetId net = net_of(sg, x.s);
            const Point M = miter_point(c, x.dd, d2, sg * off);
            good = seg_ok(offset_point(c, n, sg * off), M, x.l, net) && seg_ok(M, offset_point(c2, n2, sg * off), x.l, net);
          }
          if (good) relax(key(x.l, gx, gy, d2, x.s), g + K * steplen(d2, x.l) + bend_cost, q.id, -1, kTurn, c2, x.l);
        }
      // Coupled via pair: jog out to the via spacing, change layer, jog back, MV steps in all.
      if (vias_ok) {
        int gx = x.gx, gy = x.gy;
        delta(x.dd, MV, gx, gy);
        if (in_win(gx, gy) && obs->inside_board(at(gx, gy), 0)) {
          const Point c2 = at(gx, gy);
          for (int l2 = 0; l2 < nl; ++l2) {
            if (l2 == x.l) continue;
            bool good = true;
            for (const double sg : {1.0, -1.0}) {
              if (!good) break;
              const NetId net = net_of(sg, x.s);
              const Point P = offset_point(c, n, sg * off);
              const Point V = offset_point(offset_point(c, u, hj), n, sg * static_cast<double>(pr.via_offset));
              const Point Q = offset_point(offset_point(c, u, 2 * hj), n, sg * off);
              good = seg_ok(P, V, x.l, net) && obs->via_state(V, pr.via_diameter, pr.via_drill, net, 0, false, nullptr) == 0 && seg_ok(V, Q, l2, net) &&
                     seg_ok(Q, offset_point(c2, n, sg * off), l2, net);
            }
            if (good) relax(key(l2, gx, gy, x.dd, x.s), g + MV * steplen(x.dd, l2) + via_pair_cost, q.id, -1, kVia, c2, l2);
          }
        }
      }
      // Twist: change layer and swap sides (the geometry is described where MT is defined).
      if (vias_ok && opt.pair_twists && mv0 != kStart) {
        int gx = x.gx, gy = x.gy;
        delta(x.dd, MT, gx, gy);
        if (in_win(gx, gy) && obs->inside_board(at(gx, gy), 0)) {
          const Point c2 = at(gx, gy);
          auto pt = [&](double xx, double yy) { return offset_point(offset_point(c, u, xx), n, yy); };
          const double vo = static_cast<double>(pr.via_offset);
          const NetId nl_ = net_of(1, x.s), nr_ = net_of(-1, x.s);
          for (int l2 = 0; l2 < nl; ++l2) {
            if (l2 == x.l) continue;
            const Point VR = pt(hj, -vo), VL = pt(hj + 2.0 * twD, -vo);
            const bool good =
                // right half
                seg_ok(pt(0, -off), VR, x.l, nr_) && obs->via_state(VR, pr.via_diameter, pr.via_drill, nr_, 0, false, nullptr) == 0 &&
                seg_ok(VR, pt(hj + twD / 2, -vo), l2, nr_) && seg_ok(pt(hj + twD / 2, -vo), pt(hj + 1.5 * twD, off), l2, nr_) &&
                seg_ok(pt(hj + 1.5 * twD, off), pt(twT, off), l2, nr_) && seg_ok(pt(twT, off), offset_point(c2, n, off), l2, nr_) &&
                // left half
                seg_ok(pt(0, off), pt(hj + twD / 2, off), x.l, nl_) && seg_ok(pt(hj + twD / 2, off), pt(hj + 1.5 * twD, -vo), x.l, nl_) &&
                seg_ok(pt(hj + 1.5 * twD, -vo), VL, x.l, nl_) && obs->via_state(VL, pr.via_diameter, pr.via_drill, nl_, 0, false, nullptr) == 0 &&
                seg_ok(VL, pt(twT, -off), l2, nl_) && seg_ok(pt(twT, -off), offset_point(c2, n, -off), l2, nl_);
            // Dearer than a plain via pair by its uncoupled length: used only where the pin order calls for it.
            if (good)
              relax(key(l2, gx, gy, x.dd, 1 - x.s), g + MT * steplen(x.dd, l2) + via_pair_cost + leg_weight * static_cast<std::int64_t>(2.0 * twD), q.id, -1, kTwist, c2, l2);
          }
        }
      }
    }
    charge();
    if (std::getenv("TM_DEBUG_PAIRS")) {
      const std::string who = pad_label(A.pad_a) + "-" + pad_label(A.pad_b) + " / " + pad_label(B.pad_a) + "-" + pad_label(B.pad_b);
      if (expanded == 0)
        std::fprintf(stderr, "  pair search %s: no legal start (%zu tried, %ld leg checks)%s\n", who.c_str(), starts.size(), leg_checks, stopped ? ", out of budget" : "");
      else
        std::fprintf(stderr, "  pair search %s layers %x/%x: gap %.3f w %.3f K %d MV %d, %zu starts (%ld legal), %ld expanded, %ld leg checks, %zu goals (%ld legal), closest %.3f mm, R %.3f/%.3f%s\n",
                     who.c_str(), static_cast<unsigned>(s_layers), static_cast<unsigned>(e_layers), nm_to_mm(pr.gap), nm_to_mm(w), K, MV, starts.size(), dbg_start_ok, expanded,
                     leg_checks, goals.size(), dbg_goal_ok, dbg_closest / 1e6, Rs / 1e6, Re / 1e6, stopped ? ", out of budget" : "");
    }
    if (!done) {
      if (stopped) pair_why = "out of budget";
      else if (expanded >= cap) pair_why = "pair search budget";
      return false;
    }
    for (std::size_t k = 0; k < straddled.size(); ++k)
      if (!boxed_before[k] && pin_boxed_in(straddled[k])) {
        lift_items(ia);
        lift_items(ib);
        pair_why = "would enclose pin " + pad_label(straddled[k]) + " between the halves";
        return false;
      }
    for (int ci : {ia, ib}) {
      cs[static_cast<std::size_t>(ci)].routed = true;
      cs[static_cast<std::size_t>(ci)].coupled = true;
      ++res.routed;
      release_escapes(ci);
    }
    return true;
  }

  // Component-rule hook (doc 15 P3): with --diff-pairs every P/N pair by name; otherwise only RouterOptions::pair_nets.
  bool pair_wanted(NetId a, NetId c) const {
    if (opt.diff_pairs) return obs->rules().coupled_diff_pair(a, c);
    for (const auto& [p, q] : opt.pair_nets)
      if ((p == a && q == c) || (p == c && q == a)) return true;
    return false;
  }

  // Partner connection of each half routed coupled (-1 none); empty unless pairs are routed. A coupled half ripped by
  // negotiation takes its partner with it, and the pair is tried coupled again before single routing (D50).
  std::vector<int> pair_partner;
  std::vector<std::uint8_t> pair_retries;
  // Routes wanted pairs coupled. Before routing (recouple = false): every unrouted pair of connections. In the
  // clean-up (recouple = true): pairs whose halves both ended up routed singly are ripped and routed coupled; the old
  // copper is restored exactly when that fails, so completion never changes (transaction, rule 4).
  struct OldItem { drc::ItemKind kind; int index; };
  void restore_items(int ci, const std::vector<OldItem>& saved) {
    auto& st = cs[static_cast<std::size_t>(ci)];
    for (const auto& o : saved) {
      const int id = o.kind == drc::ItemKind::Via ? obs->add_via(o.index, ci) : obs->add_track(o.index, ci);
      near_mark(id, +1);
      if (o.kind == drc::ItemKind::Via) emit_via_add(o.index);
      else emit_track_add(o.index);
      st.items.push_back(id);
    }
  }
  std::vector<OldItem> lift_items(int ci) {
    auto& st = cs[static_cast<std::size_t>(ci)];
    std::vector<OldItem> saved;
    for (int item : st.items) {
      const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
      saved.push_back({it.kind, it.index});
      if (opt.sink) emit(std::string("{\"type\":\"") + (it.kind == drc::ItemKind::Via ? "via_remove" : "track_remove") + "\",\"id\":" + std::to_string(it.index) + "}");
      remove_routed(item);
    }
    st.items.clear();
    return saved;
  }
  int route_diff_pairs(bool recouple = false) {
    if (!recouple) pair_partner.assign(cs.size(), -1);
    if (pair_retries.size() != cs.size()) pair_retries.assign(cs.size(), 0);
    std::map<NetId, std::vector<int>> by_net;
    for (std::size_t ci = 0; ci < cs.size(); ++ci) {
      const auto& st = cs[ci];
      if (st.c.pad_b < 0) continue;
      if (recouple ? (st.routed && !st.implicit && !st.coupled && !st.items.empty()) : !st.routed) by_net[st.c.net].push_back(static_cast<int>(ci));
    }
    int done = 0;
    const bool dbg = std::getenv("TM_DEBUG_PAIRS") != nullptr;
    auto pos = [&](int pad) { return b.pads[static_cast<std::size_t>(pad)].pos; };
    auto dist = [](Point u, Point v) { return std::hypot(static_cast<double>(u.x - v.x), static_cast<double>(u.y - v.y)); };
    for (const auto& [na, la] : by_net)
      for (const auto& [nb, lb] : by_net) {
        if (na >= nb || !pair_wanted(na, nb)) continue;
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
          const int cb = lb[static_cast<std::size_t>(best)];
          std::vector<OldItem> old_a, old_b;
          if (recouple) {
            old_a = lift_items(ca);
            old_b = lift_items(cb);
            cs[static_cast<std::size_t>(ca)].routed = cs[static_cast<std::size_t>(cb)].routed = false;
            res.routed -= 2;
          }
          const bool okp = route_pair(ca, cb);
          if (recouple && !okp) {
            restore_items(ca, old_a);
            restore_items(cb, old_b);
            cs[static_cast<std::size_t>(ca)].routed = cs[static_cast<std::size_t>(cb)].routed = true;
            res.routed += 2;
          }
          if (dbg)
            std::fprintf(stderr, "pair %s / %s%s: %s\n", b.nets[static_cast<std::size_t>(na)].name.c_str(), b.nets[static_cast<std::size_t>(nb)].name.c_str(),
                         recouple ? " (clean-up)" : "", ((okp ? std::string("coupled") : pair_why) + pair_tag).c_str());
          if (okp) {
            used[static_cast<std::size_t>(best)] = 1;
            pair_partner[static_cast<std::size_t>(ca)] = cb;
            pair_partner[static_cast<std::size_t>(cb)] = ca;
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
            !cs[static_cast<std::size_t>(o)].implicit && !cs[static_cast<std::size_t>(o)].coupled &&
            !(first_net_ids.count(cs[static_cast<std::size_t>(o)].c.net) && !first_net_ids.count(cs[static_cast<std::size_t>(owner)].c.net)))
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
        // KiCad's pairs (by name) with a custom skew rule; with --pair-skew-mm also every pair routed as one (D50).
        if (na >= nb) continue;
        const bool named = obs->rules().coupled_diff_pair(na, nb);
        if (!named && !((opt.pair_skew > 0 || !opt.pair_net_skew.empty()) && pair_wanted(na, nb))) continue;
        auto mxs = named ? obs->rules().skew_constraint(na) : std::nullopt;
        // A custom rule of the board wins; then the pair's own limit (component rules); then --pair-skew-mm.
        if (!mxs)
          for (std::size_t k = 0; k < opt.pair_nets.size() && k < opt.pair_net_skew.size(); ++k)
            if (opt.pair_net_skew[k] > 0 && ((opt.pair_nets[k].first == na && opt.pair_nets[k].second == nb) || (opt.pair_nets[k].first == nb && opt.pair_nets[k].second == na)))
              mxs = opt.pair_net_skew[k];
        if (!mxs && opt.pair_skew > 0 && pair_wanted(na, nb)) mxs = opt.pair_skew;
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
    auto channel = [&](NetId n) {
      const Coord w = neck_width(n) > 0 ? neck_width(n) : class_width(n);
      return w + 2 * std::max(netclass(n).clearance, rules.minimums.clearance);
    };
    EscapeStats es;
    std::vector<EscapeCorridor> plan;
    if (opt.escape_flow) {
      // Version 2: min-cost-flow channels and layers for deep arrays, planned at the router's class rules and
      // checked against fixed copper at the lattice's own legality test (doc 05 §14).
      FlowEscapeInput fin;
      fin.width = [&](NetId n) { return class_width(n); };
      fin.clearance = [&](NetId n) { return std::max(netclass(n).clearance, rules.minimums.clearance); };
      fin.via = [&](NetId n) { return via_diameter(n); };
      fin.keep = keep;
      fin.track_free = [&](int layer, Point p, NetId n) {
        return layer_ok(n, layer) && code_ok(obs->fixed_code(p, layer, class_width(n) / 2, 0, n), n) && rule_state(p, layer, class_width(n) / 2, 0, n) != 2;
      };
      fin.via_free = [&](Point p, NetId n) {
        return vias_ok(n) && code_ok(obs->fixed_via_code(p, via_diameter(n), via_drill(n), 0, n), n) &&
               !obs->via_disallowed(p, via_diameter(n), n, 0, nl - 1, model::ViaType::Through);
      };
      fin.layers = nl;
      FlowEscapeStats fs;
      plan = plan_escapes_flow(b, needs, fin, {}, &fs);
      if (std::getenv("TM_DEBUG_TIMING")) {
        std::fprintf(stderr, "escape flow: %d deep arrays;", fs.arrays);
        for (std::size_t r = 0; r < fs.rings.size(); ++r)
          std::fprintf(stderr, " ring %zu: %d pins %d/%d/%d/%d;", r + 1, fs.rings[r].pins, fs.rings[r].pad_layer, fs.rings[r].other_layer,
                       fs.rings[r].via_only, fs.rings[r].none);
        std::fprintf(stderr, "\n");
      }
    } else {
      // Second-ring channel corridors are off by default: logicbone 964 -> 954, decelerator 479 -> 484 (doc 05 §12).
      plan = opt.escape_second_ring ? plan_escapes(b, needs, keep, {}, &es, channel) : plan_escapes(b, needs, keep, {}, &es);
    }
    reserve.assign(static_cast<std::size_t>(nl) * static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), 0);
    pad_reserved.assign(b.pads.size(), {});
    reserve_pen = static_cast<std::int64_t>(2 * opt.soft_cost_mm * 1e6);
    res.escape_corridors = 0;
    plan_json.clear();
    auto mark = [&](std::size_t gi, NetId net, int pad) {
      if (reserve[gi] == 0) {
        reserve[gi] = static_cast<std::int32_t>(net);
        pad_reserved[static_cast<std::size_t>(pad)].push_back(gi);
      } else if (reserve[gi] != static_cast<std::int32_t>(net)) {
        reserve[gi] = -1;  // two plans want it: reserved for nobody
      }
    };
    for (const auto& c : plan) {
      if (!layer_ok(c.net, c.layer) || (c.via && !vias_ok(c.net))) continue;  // custom disallow rules
      if (c.via) {  // a dog-bone is only worth reserving where that net's via fits among the fixed copper
        const int gx = to_ix(c.b.x), gy = to_iy(c.b.y);
        if (gx < 0 || gy < 0 || gx >= nx || gy >= ny) continue;
        const std::int32_t code = obs->fixed_via_code(at(gx, gy), via_diameter(c.net), via_drill(c.net), 0, cache_for(c.net).rep);
        if (!code_ok(code, c.net) || obs->via_disallowed(at(gx, gy), via_diameter(c.net), c.net, 0, nl - 1, model::ViaType::Through)) continue;
      }
      ++res.escape_corridors;
      const Coord r = c.band;
      auto strip = [&](Point sa, Point sb, bool site_at_end, int layer) {
        const int ix0 = std::max(0, to_ix(std::min(sa.x, sb.x) - r)), ix1 = std::min(nx - 1, to_ix(std::max(sa.x, sb.x) + r));
        const int iy0 = std::max(0, to_iy(std::min(sa.y, sb.y) - r)), iy1 = std::min(ny - 1, to_iy(std::max(sa.y, sb.y) + r));
        const long double ux = static_cast<long double>(sb.x - sa.x), uy = static_cast<long double>(sb.y - sa.y);
        const long double len2 = ux * ux + uy * uy;
        const long double r2 = static_cast<long double>(r) * static_cast<long double>(r);
        for (int gy = iy0; gy <= iy1; ++gy)
          for (int gx = ix0; gx <= ix1; ++gx) {
            const Point p = at(gx, gy);
            const long double px = static_cast<long double>(p.x - sa.x), py = static_cast<long double>(p.y - sa.y);
            const long double t = len2 > 0 ? std::clamp((px * ux + py * uy) / len2, 0.0L, 1.0L) : 0.0L;
            const long double dx = px - t * ux, dy = py - t * uy;
            if (dx * dx + dy * dy > r2) continue;
            const long double vx = static_cast<long double>(p.x - sb.x), vy = static_cast<long double>(p.y - sb.y);
            if (site_at_end && vx * vx + vy * vy <= r2) {  // the via site: every layer
              for (int l = 0; l < nl; ++l) mark(lat_index(l, gx, gy), c.net, c.pad);
            } else {
              mark(lat_index(layer, gx, gy), c.net, c.pad);
            }
          }
      };
      if (c.has_mid) {
        strip(c.a, c.mid, false, c.layer);
        strip(c.mid, c.b, false, c.layer);
      } else {
        strip(c.a, c.b, c.via, c.layer);
      }
      Point prev = c.b;
      for (const Point& q : c.tail) {  // version 2: the planned channels, on the escape's layer
        strip(prev, q, false, c.tail_layer);
        prev = q;
      }
      if (opt.sink) {  // viewer: one corridor per pin (doc 09; CLAUDE.md rule 7)
        std::string pts = "[" + jnum(c.a.x) + "," + jnum(c.a.y) + "]";
        if (c.has_mid) pts += ",[" + jnum(c.mid.x) + "," + jnum(c.mid.y) + "]";
        pts += ",[" + jnum(c.b.x) + "," + jnum(c.b.y) + "]";
        for (const Point& q : c.tail) pts += ",[" + jnum(q.x) + "," + jnum(q.y) + "]";
        if (!plan_json.empty()) plan_json += ",";
        plan_json += "{\"id\":" + std::to_string(c.pad) + ",\"net\":" + std::to_string(c.net) + ",\"pad\":\"" + pad_label(c.pad) +
                     "\",\"via\":" + (c.via ? "true" : "false") + ",\"pts\":[" + pts + "]}";
      }
    }
    if (opt.sink) emit("{\"type\":\"escape_plan\",\"corridors\":[" + plan_json + "]}");
  }
  std::string plan_json;
  std::string pad_label(int pad) const {
    const auto& p = b.pads[static_cast<std::size_t>(pad)];
    std::string ref = p.footprint >= 0 ? b.footprints[static_cast<std::size_t>(p.footprint)].reference : std::string("?");
    std::string out = ref + "." + p.number;
    std::string esc;
    for (char ch : out) {
      if (ch == '"' || ch == '\\') esc += '\\';
      esc += ch;
    }
    return esc;
  }
  void release_pad(int pad, NetId net) {
    if (reserve.empty() || pad < 0) return;
    if (opt.sink && !pad_reserved[static_cast<std::size_t>(pad)].empty()) emit("{\"type\":\"escape_release\",\"id\":" + std::to_string(pad) + "}");
    for (std::size_t gi : pad_reserved[static_cast<std::size_t>(pad)])
      if (reserve[gi] == static_cast<std::int32_t>(net)) reserve[gi] = 0;
    pad_reserved[static_cast<std::size_t>(pad)].clear();
  }
  // A pin whose connections are all routed no longer needs its corridor.
  void release_escapes(int ci) {
    if (reserve.empty()) return;
    const auto& c = cs[static_cast<std::size_t>(ci)].c;
    for (int pad : {c.pad_a, c.pad_b}) {
      if (pad < 0) continue;
      auto& open = pad_open[static_cast<std::size_t>(pad)];
      if (open > 0 && --open == 0) release_pad(pad, c.net);
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
    if (opt.global_route || opt.global_congestion || opt.cut_report) {
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
        g.net = static_cast<int>(c.net);
        g.box_a = obs->copper().items[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(c.pad_a)])].box;
        g.box_b = c.pad_b >= 0 ? obs->copper().items[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(c.pad_b)])].box : g.box_a;
        gn.push_back(g);
      }
      GlobalOptions go;
      Coord wc = 1'000'000'000;
      for (const auto& cl : rules.classes) wc = std::min(wc, std::max(cl.track_width, rules.minimums.track_width) + std::max(cl.clearance, rules.minimums.clearance));
      go.pitch = wc;
      go.via_cost_tiles = 2.0;
      for (const auto& it : obs->copper().items)
        if (it.kind == drc::ItemKind::Pad && it.net != 0) go.pads.emplace_back(static_cast<int>(it.net), it.box);
      go.via_diameter = go.via_drill = 1'000'000'000;  // the smallest class via: capacity is what could fit
      Coord vclear = 1'000'000'000;
      for (const auto& cl : rules.classes) {
        const Coord drill = std::max(cl.via_drill, rules.minimums.through_hole_diameter);
        const Coord d = std::max({cl.via_diameter, rules.minimums.via_diameter, drill + 2 * rules.minimums.via_annular_width});
        if (d < go.via_diameter) go.via_diameter = d, go.via_drill = drill;
        vclear = std::min(vclear, std::max(cl.clearance, rules.minimums.clearance));
      }
      go.via_pitch = go.via_diameter + vclear;
      global = global_route(*obs, geom::Box{lat.x0, lat.y0, lat.x1, lat.y1}, nl, gn, go);
      const char* cp = std::getenv("TM_CORRIDOR_PEN");  // experiment knob (pitches per step)
      corridor_pen = static_cast<Coord>((cp ? std::atof(cp) : 2.0) * static_cast<double>(pitch));
      res.over_cuts = global.over_cuts;
      res.tightest_cut = global.tightest;
      if (opt.global_congestion) cong_pen = std::max<Coord>(1, static_cast<Coord>(opt.global_congestion_pen * static_cast<double>(pitch)));
      if (tdbg)
        std::fprintf(stderr, "[%.2f s] global routing: %d x %d x %d tiles of %.2f mm, %d overflowed edges, %d overflowed via tiles; set-up %.3f s, routing %.3f s\n",
                     elapsed(), global.tiles_x, global.tiles_y, global.layers, nm_to_mm(global.tile), global.overflow_edges, global.overflow_via_tiles,
                     global.seconds_capacity, global.seconds_route);
      if (tdbg) {
        std::array<long, 5> hist{};  // tiles by utilisation: < 4, 4-5, 6-7, 8-11, >= 12 eighths
        for (const auto u : global.util) ++hist[u < 4 ? 0 : u < 6 ? 1 : u < 8 ? 2 : u < 12 ? 3 : 4];
        std::fprintf(stderr, "global utilisation (eighths) <4: %ld, 4-5: %ld, 6-7: %ld, 8-11: %ld, >=12: %ld\n", hist[0], hist[1], hist[2], hist[3], hist[4]);
      }
    }
    if (tdbg) std::fprintf(stderr, "[%.2f s] plan done: %zu connections\n", elapsed(), conns.size());
    res.connections = static_cast<int>(conns.size());
    for (const auto& c : conns) {
      ConnState st;
      st.c = c;
      cs.push_back(std::move(st));
    }
    for (std::size_t i = 0; i < cs.size(); ++i) pending.push_back(static_cast<int>(i));
    if (opt.escape_plan || opt.escape_flow) {
      plan_escape_reservations();
      if (tdbg) std::fprintf(stderr, "[%.2f s] escape plan: %d corridors\n", elapsed(), res.escape_corridors);
    }
    live = opt.sink != nullptr;
    if (live) {
      heat_exp.init(lat, pitch);
      heat_hist.init(lat, pitch);
    }
    if (opt.diff_pairs || !opt.pair_nets.empty()) {
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
    std::vector<std::string> best_why;  // reasons as they stood at the best state; later restarts overwrite cs[].why
    auto snapshot_unrouted = [&]() {
      best_unrouted.assign(cs.size(), 0);
      best_why.assign(cs.size(), {});
      for (std::size_t i = 0; i < cs.size(); ++i) {
        best_unrouted[i] = cs[i].routed ? 0 : 1;
        if (!cs[i].routed) best_why[i] = cs[i].why;
      }
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
      if (opt.escape_plan || opt.escape_flow) plan_escape_reservations();  // everything is unrouted again: corridors back
      if (opt.diff_pairs || !opt.pair_nets.empty()) route_diff_pairs();  // pairs first again, coupled
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
      // --first-nets go first again: a restart rips everything, including them.
      if (!first_net_ids.empty())
        std::stable_partition(order.begin(), order.end(), [&](int c) { return first_net_ids.count(cs[static_cast<std::size_t>(c)].c.net) > 0; });
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
        // Half of a pair whose coupled route was ripped: both halves coupled again first (twice per pair at most).
        if (!pair_partner.empty() && pair_partner[static_cast<std::size_t>(ci)] >= 0) {
          const int p = pair_partner[static_cast<std::size_t>(ci)];
          if (!cs[static_cast<std::size_t>(p)].routed && pair_retries[static_cast<std::size_t>(ci)] < 2) {
            ++pair_retries[static_cast<std::size_t>(ci)];
            ++pair_retries[static_cast<std::size_t>(p)];
            if (route_pair(ci, p)) {
              emit_stats("route");
              if (res.routed > best_routed) {
                snapshot();
                snapshot_unrouted();
              }
              continue;
            }
          }
        }
        bool ok = search_and_commit(st.c, false);
        std::string reason = why;
        // Escalation for pads boxed in by fixed copper: forced off-lattice escapes, then a neck-down to the
        // board's minimum track width (KiCad's track_width rule; the net-class width is only the default).
        if (!ok && (last_miss == Miss::Enclosed || why.starts_with("boxed in") || why.starts_with("exact check"))) {
          force_escapes = true;
          ok = search_and_commit(st.c, false);
          if (!ok && (neck_width(st.c.net) > 0 || neck_via_diameter(st.c.net) < class_via_diameter(st.c.net))) {
            width_override = neck_width(st.c.net);
            via_override = true;
            ok = search_and_commit(st.c, false);
            if (ok) ++res.necked;
            width_override = 0;
            via_override = false;
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
            via_override = true;
            ok = search_and_commit(st.c, true);
            if (ok) ++res.necked;
            width_override = 0;
            via_override = false;
            force_escapes = false;
            if (!ok && why.starts_with("boxed in")) {
              st.dead = true;
              if (opt.sink) {
                const int dp = why.find("target") != std::string::npos && st.c.pad_b >= 0 ? st.c.pad_b : st.c.pad_a;
                const Point q = b.pads[static_cast<std::size_t>(dp)].pos;
                emit("{\"type\":\"escape_dead\",\"id\":" + std::to_string(dp) + ",\"net\":" + std::to_string(st.c.net) + ",\"pad\":\"" + pad_label(dp) +
                     "\",\"p\":[" + jnum(q.x) + "," + jnum(q.y) + "],\"why\":\"fixed copper encloses the pin\"}");
              }
              release_pad(st.c.pad_a, st.c.net);  // a sealed pin's corridor only blocks others
              release_pad(st.c.pad_b, st.c.net);
              reason += "; fixed copper encloses the pin";
            } else if (!ok) {
              reason += "; negotiated escapes/neck-down: " + why;
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
        emit_heatmaps(false);
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
      // Pairs split by negotiation: coupled again where they fit now (each attempt restores the old copper on failure).
      if ((opt.diff_pairs || !opt.pair_nets.empty()) && !out_of_budget()) res.pairs += route_diff_pairs(true);
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
                             pa.number + " -> " + to + "  (" + best_why[ci] + ")");
      res.unrouted.push_back({b.nets[static_cast<std::size_t>(st.c.net)].name, b.footprints[static_cast<std::size_t>(pa.footprint)].reference + "." + pa.number, to});
    }
    res.seconds = elapsed();
    res.nogood_skips = nogood_skips;
    std::fprintf(stderr, "searches: %ld ok (%ld expansions), %ld failed (%ld expansions); fields %ld GPU + %ld CPU (%.2f s, %ld GPU fallbacks)\n",
                 n_ok, exp_ok, n_fail, exp_fail, field_runs, field_cpu_runs, field_seconds, field_gpu_fail);
    std::fprintf(stderr, "clean-up: %d connections improved\n", res.optimized);
    std::fprintf(stderr, "restarts %d; legality checks %ld; rips %d, passes %d, boxed-in %d, nogood skips %ld, history cells %zu\n", res.restarts, obs->checks, res.rips,
                 res.passes, res.enclosed, nogood_skips, history.size());
    if (confined_tried) std::fprintf(stderr, "global corridors: %ld of %ld confined searches committed\n", confined_ok, confined_tried);
    if (reach_checks) std::fprintf(stderr, "reachability checks: %ld, %ld proved unreachable, %ld cells visited%s\n", reach_checks, reach_pruned, reach_visits,
                                  opt.reach_verify ? (reach_mismatch ? ", MISMATCHES" : ", verified (0 mismatches)") : "");
    if (reach_mismatch) std::fprintf(stderr, "reachability mismatches: %ld\n", reach_mismatch);
    std::fprintf(stderr, "failed-search expansions: strict confined %ld, strict wide %ld, negotiated confined %ld, negotiated wide %ld\n", xf_strict_conf,
                 xf_strict_wide, xf_soft_conf, xf_soft_wide);
    if (opt.escape_report) {  // pins of deep arrays per ring: to route, and with every connection routed
      std::vector<char> needs(b.pads.size(), 0), open(b.pads.size(), 0);
      for (const auto& st : cs)
        for (int pad : {st.c.pad_a, st.c.pad_b})
          if (pad >= 0) {
            needs[static_cast<std::size_t>(pad)] = 1;
            if (!st.routed) open[static_cast<std::size_t>(pad)] = 1;
          }
      const auto ring = array_rings(b, needs);
      res.escape_rings.clear();
      for (std::size_t pi = 0; pi < ring.size(); ++pi) {
        if (ring[pi] <= 0 || !needs[pi]) continue;
        if (res.escape_rings.size() < static_cast<std::size_t>(ring[pi])) res.escape_rings.resize(static_cast<std::size_t>(ring[pi]));
        auto& e = res.escape_rings[static_cast<std::size_t>(ring[pi] - 1)];
        ++e.first;
        if (!open[pi]) ++e.second;
      }
    }
    emit_heatmaps(true);
    emit_stats("done");
    emit("{\"type\":\"stage\",\"name\":\"route\",\"state\":\"end\",\"detail\":\"\"}");
    return std::move(res);
  }
};

PortfolioResult route_portfolio(const model::Board& board, const model::DesignRules& rules, const RouterOptions& base, int variants,
                                const std::vector<int>& pick, int threads) {
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
  // Escape planning (M9) as a portfolio arm: on in two variants, so boards where it helps get it while the others
  // keep their configurations (all variants on: tier B +8 connections, tier C -7; doc 05 §12).
  add("fast bends, jittered order, escape plan", [&](RouterOptions& o) { o.bend_states = false; o.order = 2; o.seed = base.seed + 1; o.escape_plan = true; });
  add("exact bends, jittered order", [&](RouterOptions& o) { o.order = 2; o.seed = base.seed + 2; });
  add("fast bends, cheap vias (2x pitch on large boards, half pitch on small ones)",
      [&](RouterOptions& o) { o.bend_states = false; o.via_cost_mm = base.via_cost_mm * 0.4; o.pitch_scale = 2.0; if (base.fine_variant) o.pitch_scale_small = 0.5; });
  add("fast bends, cheap crossings, escape plan", [&](RouterOptions& o) { o.bend_states = false; o.soft_cost_mm = base.soft_cost_mm * 0.5; o.escape_plan = true; });
  add("fast bends, dear vias", [&](RouterOptions& o) { o.bend_states = false; o.via_cost_mm = base.via_cost_mm * 2.5; });
  std::vector<int> chosen;
  if (!pick.empty()) {
    for (int i : pick)
      if (i >= 0 && i < static_cast<int>(vs.size())) chosen.push_back(i);
  } else {
    for (int i = 0; i < std::clamp(variants, 1, static_cast<int>(vs.size())); ++i) chosen.push_back(i);
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
    for (std::size_t i = 0; i < vs.size(); ++i) vs[i].o.gpu_device = devs.empty() ? -1 : devs[i % devs.size()].index;
  }
  // Copper length is summed in commit order, so it is the same value whichever thread ran the variant.
  auto length = [](const RouteResult& r) {
    double l = 0;
    for (const auto& t : r.tracks) l += std::hypot(static_cast<double>(t.b.x - t.a.x), static_cast<double>(t.b.y - t.a.y));
    return l;
  };
  if (base.halving && threads >= 1 && static_cast<int>(vs.size()) > threads) {
    // Rung sizes n, n/2, ... down to `threads`.
    std::vector<std::size_t> sizes{vs.size()};
    while (sizes.back() > static_cast<std::size_t>(threads)) sizes.push_back(std::max(static_cast<std::size_t>(threads), sizes.back() / 2));
    const double rungs = static_cast<double>(sizes.size());
    std::vector<std::size_t> alive(vs.size());
    for (std::size_t i = 0; i < alive.size(); ++i) alive[i] = i;
    std::vector<RouteResult> last(vs.size());      // each variant's result at its last rung
    std::vector<std::uint8_t> ran(vs.size(), 0);
    auto better_r = [&](const RouteResult& a, std::size_t ia, const RouteResult& c, std::size_t ic) {
      if (a.routed != c.routed) return a.routed > c.routed;
      if (a.vias.size() != c.vias.size()) return a.vias.size() < c.vias.size();
      const double la = length(a), lc = length(c);
      if (la != lc) return la < lc;
      return chosen[ia] < chosen[ic];
    };
    RouteResult best_res;
    std::size_t best_i = 0;
    bool have_best = false;
    for (std::size_t r = 0; r < sizes.size(); ++r) {
      // An equal share of the budget per rung, split over the variants of the rung that share a thread.
      const double share = static_cast<double>(threads) / (rungs * static_cast<double>(alive.size()));
      std::atomic<std::size_t> next{0};
      std::vector<std::thread> pool;
      std::vector<RouteResult> out(alive.size());
      for (int w = 0; w < threads; ++w)
        pool.emplace_back([&] {
          for (std::size_t k; (k = next.fetch_add(1)) < alive.size();) {
            RouterOptions o = vs[alive[k]].o;
            o.sink = nullptr;  // restarts would replay the board in the viewer
            o.deadline = nullptr;
            if (base.work_budget > 0) o.work_budget = std::max<long>(1, static_cast<long>(static_cast<double>(base.work_budget) * share));
            else o.time_limit_s = base.time_limit_s * share;
            out[k] = Router(board, rules, o).run();
          }
        });
      for (auto& t : pool) t.join();
      for (std::size_t k = 0; k < alive.size(); ++k) {
        if (!have_best || better_r(out[k], alive[k], best_res, best_i)) best_res = out[k], best_i = alive[k], have_best = true;
        last[alive[k]] = std::move(out[k]);
        ran[alive[k]] = 1;
      }
      if (best_res.connections > 0 && best_res.routed == best_res.connections) break;  // complete: nothing left to select
      if (r + 1 < sizes.size()) {
        std::stable_sort(alive.begin(), alive.end(), [&](std::size_t p, std::size_t q) { return better_r(last[p], p, last[q], q); });
        alive.resize(sizes[r + 1]);
        std::sort(alive.begin(), alive.end());
      }
    }
    PortfolioResult pr;
    pr.indices = chosen;
    for (std::size_t i = 0; i < vs.size(); ++i) {
      pr.variants.push_back(vs[i].name);
      pr.routed.push_back(ran[i] ? last[i].routed : 0);
    }
    pr.best_variant = static_cast<int>(best_i);
    pr.best = std::move(best_res);
    return pr;
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
  // Variants are taken in index order by `threads` workers. Which thread runs a variant, and when, cannot change its
  // result: each Router owns all of its state, its clock starts when it starts, and in work-budget mode nothing is
  // shared between variants (the shared deadline is wall-clock mode only).
  const std::size_t workers = threads <= 0 ? vs.size() : std::min(vs.size(), static_cast<std::size_t>(threads));
  std::atomic<std::size_t> next{0};
  for (std::size_t w = 0; w < workers; ++w)
    pool.emplace_back([&] {
      for (std::size_t i; (i = next.fetch_add(1)) < vs.size();) rs[i] = Router(board, rules, vs[i].o).run();
    });
  for (auto& t : pool) t.join();
  PortfolioResult pr;
  // Total order on (routed desc, vias asc, length asc, variant index asc): the winner depends neither on the order
  // in which variants finished nor on their positions in `pick`.
  auto better = [&](std::size_t i, std::size_t j) {
    const auto& a = rs[i];
    const auto& c = rs[j];
    if (a.routed != c.routed) return a.routed > c.routed;
    if (a.vias.size() != c.vias.size()) return a.vias.size() < c.vias.size();
    const double la = length(a), lc = length(c);
    if (la != lc) return la < lc;
    return chosen[i] < chosen[j];
  };
  std::size_t best = 0;
  pr.indices = chosen;
  for (std::size_t i = 0; i < rs.size(); ++i) {
    pr.variants.push_back(vs[i].name);
    pr.routed.push_back(rs[i].routed);
    pr.seconds.push_back(rs[i].seconds);
    if (i > 0 && better(i, best)) best = i;
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

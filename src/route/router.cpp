// SPDX-License-Identifier: GPL-3.0-or-later
#include "route/router.hpp"

#include "drc/refill.hpp"
#include "route/bundle.hpp"
#include "route/fanout_heads.hpp"
#include "route/bus.hpp"
#include "route/diff_pair.hpp"
#include "route/escape.hpp"
#include "route/escape_flow.hpp"
#include "route/heat_grid.hpp"
#include "route/global_router.hpp"

#include <functional>
#include <algorithm>
#include <bit>
#include <numbers>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <queue>
#include <array>
#include <cstring>
#include <fstream>
#include <sstream>
#include <deque>
#include <set>
#include <thread>
#include <unordered_map>

#include "core/rng.hpp"
#include "drc/connectivity.hpp"
#include "gpu/device.hpp"
#include "gpu/field.hpp"
#include "route/lazy_codes.hpp"
#include "route/legalize.hpp"
#include "route/ref_map.hpp"
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

// "[3,5]" for the failure event's blocker nets (net ids, as the other events name nets).
std::string blocker_nets_json(const std::vector<NetId>& nets) {
  std::string s = "[";
  for (std::size_t i = 0; i < nets.size(); ++i) s += (i ? "," : "") + std::to_string(nets[i]);
  return s + "]";
}

}  // namespace

std::string failure_reason_code(const std::string& why) {
  // The explanations nest ("exact check ...; negotiated: would rip too many connections at once"), the pin's own state first:
  // a boxed-in pin is boxed in whatever the later attempts said, and a rip limit says more than the exact check before it.
  const auto has = [&](const char* needle) { return why.find(needle) != std::string::npos; };
  if (why.empty()) return "not_attempted";  // the budget ended before the connection was tried again
  if (has("fixed copper encloses") || has("boxed in")) return "boxed_in";
  if (has("track on a layer that takes no tracks")) return "layer_limit";
  if (has("would rip a --first-nets") || has("would rip a bus") || has("would rip too many") || has("already ripped too often") ||
      has("would rip a connection of the running"))
    return "rip_limit";
  if (has("exact check")) return "exact_check";
  if (has("ripped up by")) return "ripped_not_rerouted";
  if (has("out of budget") || has("search budget")) return "out_of_budget";
  if (has("transaction rolled back")) return "rolled_back";
  return "no_legal_path";
}

struct Router::Impl {
  const model::DesignRules& rules;
  RouterOptions opt;  // a copy: the breakout profile (package_boxes) sets some options for the board
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
    int rescues = 0;                 // pin rescues tried for this connection (D155)
    std::string why;                 // last failure explanation
    std::vector<NetId> blockers;     // nets of the connections the last rejected route would have had to rip (stuck report, doc 17)
    bool coupled = false;            // routed as half of a differential pair: clean-up leaves it alone
    bool bus_lane = false;           // routed as a lane of its bus (leads its bundle or followed a lane): via clean-up leaves it alone
    bool bundle_lane = false;        // placed as a lane of a bundle escape (--bundle-escape): nothing else may rip it; clean-up leaves it alone (bus_lane is set too)
    // Boxed in even by a negotiated search, which may cross every other net's routed copper: only fixed copper
    // encloses the pin (M9). Retrying it in later passes and restarts only spends budget others could use.
    bool dead = false;
  };
  std::vector<ConnState> cs;
  std::set<NetId> first_net_ids;  // --first-nets
  std::map<NetId, int> first_rank;  // --first-nets-ordered: position of each net in the --first-nets list
  // Critical nets (issue #24, doc 05 §44): the order rank of each net (0 = not critical, else its tier), the via price factor and whether the
  // split-crossing cost applies to it; the reference map is built in setup() when that cost is on.
  std::vector<std::int32_t> crit_rank;
  std::vector<double> net_via_mult;
  std::vector<std::uint8_t> crit_split;
  ReferenceMap refmap;
  std::int64_t split_pen = 0;
  bool split_on = false;
  bool crit_order_on = false;
  // Order of routing: --first-nets, then critical tiers, then the rest.
  // Position in the --first-nets list when --first-nets-ordered is on (the nets of the list are then routed in that order, e.g. the pins of a package round its
  // perimeter, so that no escape has to cross its neighbour's), else 0 for all of them (the router's own order within the group).
  int first_pos(NetId n) const {
    if (!opt.first_nets_ordered) return 0;
    const auto it = first_rank.find(n);
    return it == first_rank.end() ? 0 : it->second;
  }
  int order_key(NetId n) const {
    if (first_net_ids.count(n)) return 0;
    return n >= 0 && static_cast<std::size_t>(n) < crit_rank.size() && crit_rank[static_cast<std::size_t>(n)] > 0 ? crit_rank[static_cast<std::size_t>(n)] : 1000;
  }
  double via_mult_of(NetId n) const { return n >= 0 && static_cast<std::size_t>(n) < net_via_mult.size() ? net_via_mult[static_cast<std::size_t>(n)] : 1.0; }
  std::unordered_map<std::int64_t, std::uint16_t> history;  // contested lattice cells (PathFinder history cost)
  std::vector<int> init_root;        // copper item -> initial cluster root (fixed copper)
  // With soft zones: routed copper item -> initial clusters of the same-net plane fills it touches, so a via that
  // passes through several planes of its net joins all of them (`joined`). Teardrops are no planes.
  std::vector<std::vector<int>> plane_contacts;
  std::unique_ptr<drc::ZoneFills> soft_fills;
  bool soft = false;                 // current search may cross routed copper
  // Persistent fixed-obstacle caches per net class (codes from Obstacles::fixed_code), lattice-indexed.
  struct ClassCache {
    LazyCodes margin, tight, via;  // INT32_MIN = not computed yet (allocated in chunks as cells are reached)
    model::NetId rep = 0;
    bool prefilled = false;
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
  std::vector<std::vector<std::size_t>> pad_plan;      // board pad -> cells its corridor reserved at planning (kept after a release, for escape_persist)
  // --escape-via-keep (D156): lattice point (x, y) -> net whose corridor a via centred there would cover (0 none, -1 two
  // corridors: no net's via). The corridor's own band is a track's keep distance; a via needs its radius more.
  std::vector<std::int32_t> vreserve;
  std::vector<std::vector<std::size_t>> pad_vreserved;  // board pad -> via-keep points it holds
  std::unordered_map<int, std::pair<Point, int>> pad_stub;  // board pad -> (end of its straight corridor, layer): --escape-stubs
  std::vector<int> hold_item;  // board pad -> copper item of its --fanout-hold placeholder (-1 none)
  std::vector<char> esc_released;                      // connection -> its pins' corridor counts were decremented (release_escapes) and not yet restored
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
    // Advice regions and other in-memory rule areas: part of the working copy only, so every obstacle test sees them.
    b.zones.insert(b.zones.end(), opt.extra_zones.begin(), opt.extra_zones.end());
    // Name-pattern matching is too costly for per-cell lookups.
    net_class.reserve(b.nets.size());
    for (const auto& n : b.nets) net_class.push_back(&rules.class_for(n.name));
  }

  double elapsed() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
  // Out of budget? The work budget (search expansions) is deterministic; wall time is only a safety net.
  double budget_scale = 1.0;  // < 1 while a share of the budget is reserved for the polish phase (opt.polish_fraction)
  // Set when wall time, not work, ended the search: the result then depends on machine speed (reported, never read here).
  mutable bool time_stopped = false;
  bool out_of_budget() const {
    if (opt.deadline && res.routed < res.connections && elapsed() > opt.deadline->load(std::memory_order_relaxed)) return time_stopped = true;
    if (opt.work_budget > 0 && static_cast<double>(res.expansions) >= static_cast<double>(opt.work_budget) * budget_scale) return true;
    if (elapsed() > opt.time_limit_s * budget_scale) return time_stopped = true;
    return false;
  }
  // Work profile (D88): the phase now running and its counters.
  WorkPhase phase = WorkPhase::Setup;
  WorkCounts& wc() { return res.work.phase[static_cast<std::size_t>(phase)]; }
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
  std::vector<std::uint8_t> no_neck;  // by net id: RouterOptions::no_neck_nets
  bool necking_allowed(NetId net) const { const auto i = static_cast<std::size_t>(net); return i >= no_neck.size() || !no_neck[i]; }
  Coord neck_via_drill(NetId net) const { return !necking_allowed(net) ? class_via_drill(net) : std::min(class_via_drill(net), std::max<Coord>(rules.minimums.through_hole_diameter, 200'000)); }
  Coord neck_via_diameter(NetId net) const {
    if (!necking_allowed(net)) return class_via_diameter(net);
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
    if (!necking_allowed(net)) return 0;
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
    b.infer_graphic_nets = opt.soft_copper_graphics;
    if (opt.fanout_heads_mm > 0) place_fanout_heads();
    obs = std::make_unique<Obstacles>(b, rules);
    obs->set_soft_zones(opt.soft_zones);
    obs->set_soft_mask_graphics(opt.soft_mask_graphics);
    obs->set_soft_copper_graphics(opt.soft_copper_graphics);
    if (opt.soft_zones) soft_fills = std::make_unique<drc::ZoneFills>(obs->copper());
    for (std::size_t n = 0; n < b.nets.size(); ++n)
      if (const auto it = std::find(opt.first_nets.begin(), opt.first_nets.end(), b.nets[n].name); it != opt.first_nets.end()) {
        first_net_ids.insert(static_cast<NetId>(n));
        first_rank[static_cast<NetId>(n)] = static_cast<int>(it - opt.first_nets.begin());
      }
    if (opt.supply_first) {  // doc 05 §51
      const auto perim = perimeter_directions(b, {});
      std::set<NetId> planes;
      for (const auto& zn : b.zones) planes.insert(zn.net);
      std::map<std::pair<NetId, int>, int> pins;  // (net, footprint) -> perimeter pins
      for (std::size_t i = 0; i < b.pads.size(); ++i)
        if (perim[i] >= 0 && b.pads[i].net > 0 && b.pads[i].footprint >= 0) ++pins[{b.pads[i].net, b.pads[i].footprint}];
      for (const auto& [key, n] : pins)
        if (n >= 2 && !planes.count(key.first) && first_net_ids.insert(key.first).second) {
          first_rank[key.first] = static_cast<int>(first_rank.size());
          if (std::getenv("TM_DEBUG_BUNDLE")) std::fprintf(stderr, "supply first: %s (%d perimeter pins)\n", b.nets[static_cast<std::size_t>(key.first)].name.c_str(), n);
        }
    }
    if (!opt.no_neck_nets.empty()) {
      no_neck.assign(b.nets.size(), 0);
      for (std::size_t n = 0; n < b.nets.size(); ++n)
        if (std::find(opt.no_neck_nets.begin(), opt.no_neck_nets.end(), b.nets[n].name) != opt.no_neck_nets.end()) no_neck[n] = 1;
    }
    if (!opt.critical.empty()) {
      crit_rank.assign(b.nets.size(), 0);
      net_via_mult.assign(b.nets.size(), 1.0);
      crit_split.assign(b.nets.size(), 0);
      std::unordered_map<std::string, int> tiers;  // a board can have thousands of critical nets: look names up, do not scan the list per net
      for (const auto& [name, tier] : opt.critical) {
        const auto it = tiers.find(name);
        if (it == tiers.end() || tier < it->second) tiers[name] = tier;
      }
      for (std::size_t n = 0; n < b.nets.size(); ++n) {
        if (b.nets[n].name.empty()) continue;
        const auto it = tiers.find(b.nets[n].name);
        if (it == tiers.end()) continue;
        crit_rank[n] = std::max(1, it->second);
        net_via_mult[n] = std::max(1.0, opt.critical_via_mult);
        crit_split[n] = 1;
      }
      crit_order_on = opt.critical_first;
    }
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
    const Coord hc = obs->rules().router_hole_clearance();
    const Coord vm = b.vias_tented ? 0 : std::max<Coord>(b.pad_to_mask_clearance, 0);
    near_infl = probe + pitch * 71 / 100 + 1 + std::max({obs->rules().max_clearance(), hc, 2 * vm + 1'000}) + 2 * pitch + 2;
    near_r.assign(static_cast<std::size_t>(nl) * static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), 0);
    if (!opt.critical.empty() && (opt.split_cost > 0 || opt.split_hard) && nl > 1) build_split_map();
  }
  // The cells of each layer over a void of the adjacent reference plane, from the zone fills (refilled by the engine where the file has none).
  void build_split_map() {
    bool unfilled = false, any = false;
    for (const auto& z : b.zones)
      if (!z.rule_area && z.net > 0) {
        any = true;
        unfilled = unfilled || z.fills.empty();
      }
    if (!any) return;
    if (opt.reference_fills) {
      refmap = build_reference_map(*opt.reference_fills, lat, pitch, nx, ny, opt.split_min_coverage);
    } else if (unfilled) {
      const auto rf = drc::refill_zones(b, rules);
      refmap = build_reference_map(rf.board, lat, pitch, nx, ny, opt.split_min_coverage);
    } else {
      refmap = build_reference_map(b, lat, pitch, nx, ny, opt.split_min_coverage);
    }
    split_on = refmap.any();
    split_pen = static_cast<std::int64_t>(opt.split_cost * static_cast<double>(pitch));
    if (!split_on) return;
    // The plane has an antipad round every through-hole pad (and its clearance is several lattice cells), so the cells next to a critical net's own pad are
    // voids by construction. A net must be able to leave its pad: within the pad's radius plus 1 mm nothing counts (on every layer). Without this
    // --split-hard closed a through-hole pad in.
    const std::size_t layer_cells = static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
    for (const auto& p : b.pads) {
      if (p.net <= 0 || static_cast<std::size_t>(p.net) >= crit_split.size() || !crit_split[static_cast<std::size_t>(p.net)]) continue;
      const double r = static_cast<double>(std::max(p.size_x, p.size_y)) / 2.0 + 1'000'000.0;
      const int cx = to_ix(p.pos.x), cy = to_iy(p.pos.y), span = static_cast<int>(r / static_cast<double>(pitch)) + 1;
      for (int y = std::max(0, cy - span); y <= std::min(ny - 1, cy + span); ++y)
        for (int x = std::max(0, cx - span); x <= std::min(nx - 1, cx + span); ++x) {
          const double dx = static_cast<double>(x - cx) * static_cast<double>(pitch), dy = static_cast<double>(y - cy) * static_cast<double>(pitch);
          if (dx * dx + dy * dy > r * r) continue;
          for (int l = 0; l < nl; ++l) refmap.over_void[static_cast<std::size_t>(l) * layer_cells + static_cast<std::size_t>(y) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(x)] = 0;
        }
    }
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

  // Dense package, as the escape planner sees it (route/escape.hpp EscapeOptions): 8+ copper pads, pitch up to 1.3 mm. The smallest centre distance of its pads.
  std::map<int, Coord> fp_pitch_cache;
  Coord footprint_pitch(int fp) {
    if (const auto it = fp_pitch_cache.find(fp); it != fp_pitch_cache.end()) return it->second;
    std::vector<int> pads;
    for (int pi : b.footprints[static_cast<std::size_t>(fp)].pads) {
      const auto& p = b.pads[static_cast<std::size_t>(pi)];
      if (p.copper != 0 && p.type != model::PadType::NpThruHole) pads.push_back(pi);
    }
    Coord pt = 0;
    if (static_cast<int>(pads.size()) >= EscapeOptions{}.min_pads) {
      pt = std::numeric_limits<Coord>::max();
      for (std::size_t i = 0; i < pads.size(); ++i)
        for (std::size_t j = i + 1; j < pads.size(); ++j) {
          const Point d = b.pads[static_cast<std::size_t>(pads[j])].pos - b.pads[static_cast<std::size_t>(pads[i])].pos;
          const Coord dist = static_cast<Coord>(std::llround(std::hypot(static_cast<double>(d.x), static_cast<double>(d.y))));
          if (dist > 0) pt = std::min(pt, dist);
        }
      if (pt == std::numeric_limits<Coord>::max() || pt > EscapeOptions{}.max_pitch) pt = 0;
    }
    return fp_pitch_cache.emplace(fp, pt).first->second;
  }
  bool dense_footprint(int fp) { return footprint_pitch(fp) > 0; }

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
      // --radial-supply: pins of this net on one dense package. An edge between two of them costs radial_penalty, so each pin goes to a capacitor or
      // via beyond the package and the trunk forms outside the escape band (a designer's supply pins; ethercat_qfn_analysis.md, §3).
      std::map<int, int> pins_on;
      if (opt.radial_supply)
        for (const auto& g : groups)
          for (int pa : g.pads)
            if (const int fp = b.pads[static_cast<std::size_t>(pa)].footprint; fp >= 0 && dense_footprint(fp)) ++pins_on[fp];
      auto radial_cost = [&](int pa, int pb) -> double {
        if (!opt.radial_supply) return 0;
        const auto& A = b.pads[static_cast<std::size_t>(pa)];
        const auto& B = b.pads[static_cast<std::size_t>(pb)];
        if (A.footprint < 0 || A.footprint != B.footprint) return 0;
        const auto it = pins_on.find(A.footprint);
        if (it == pins_on.end() || it->second < 3) return 0;
        const Coord pt = footprint_pitch(A.footprint);
        if (std::min(A.size_x, A.size_y) > pt || std::min(B.size_x, B.size_y) > pt) return 0;  // an exposed pad is the net's own plane, not a pin
        return opt.radial_penalty_mm * 1e6;
      };
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
              const double rpen = radial_cost(pa, pb);
              if (!by_gap) {
                consider(pa, pb, -1, centre + rpen);
                continue;
              }
              // Copper to copper, not centre to centre: a pin beside a large pad of its net (a QFN's exposed pad,
              // a wide power pad) is nearer to it than to the next pin of the row, and the pin-to-pin connection
              // may have to hop over a foreign pin between them. The centre distance breaks ties.
              if (centre * 1e-3 + rpen >= best[j]) continue;  // even touching copper would not beat the best so far
              consider(pa, pb, -1, pad_gap(pa, pb, centre) + centre * 1e-3 + rpen);
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
        if (how[nxt].pad_a >= 0) {
          out.push_back(how[nxt]);  // two zone-only clusters have nothing to join them directly
          if (how[nxt].pad_b >= 0 && b.pads[static_cast<std::size_t>(how[nxt].pad_a)].footprint >= 0 &&
              b.pads[static_cast<std::size_t>(how[nxt].pad_a)].footprint == b.pads[static_cast<std::size_t>(how[nxt].pad_b)].footprint) {
            std::map<int, int> n_on;
            for (const auto& g : groups)
              for (int pa : g.pads) ++n_on[b.pads[static_cast<std::size_t>(pa)].footprint];
            if (dense_footprint(b.pads[static_cast<std::size_t>(how[nxt].pad_a)].footprint) && n_on[b.pads[static_cast<std::size_t>(how[nxt].pad_a)].footprint] >= 3) ++res.package_pin_edges;
          }
        }
        upd(nxt);
      }
    }
    if (opt.order == 3) {
      // Deep pins of dense packages first (doc 16): a ball in the third ring needs the channels that outer balls'
      // escapes would otherwise take. Depth = distance of the pad centre to the edge of its footprint's pad cloud,
      // in 1/4 pitches of the footprint's pads, so balls of one ring tie and then go shortest first.
      std::map<int, geom::Box> cloud;
      std::map<int, int> count;
      for (const auto& pd : b.pads) {
        if (pd.footprint < 0) continue;
        cloud[pd.footprint].add(pd.pos);
        ++count[pd.footprint];
      }
      auto depth = [&](int pad) -> Coord {
        if (pad < 0) return 0;
        const auto& pd = b.pads[static_cast<std::size_t>(pad)];
        if (pd.footprint < 0 || count[pd.footprint] < 8) return 0;
        const auto& bx = cloud[pd.footprint];
        return std::min({pd.pos.x - bx.x0, bx.x1 - pd.pos.x, pd.pos.y - bx.y0, bx.y1 - pd.pos.y});
      };
      const Coord q = std::max<Coord>(pitch, 1) * 10;  // ring quantum: ball pitch is ~20 lattice steps
      std::stable_sort(out.begin(), out.end(), [&](const Connection& a, const Connection& c) {
        const Coord da = std::max(depth(a.pad_a), depth(a.pad_b)) / q, dc = std::max(depth(c.pad_a), depth(c.pad_b)) / q;
        if (da != dc) return da > dc;
        return a.length < c.length;
      });
    } else if (opt.order == 1) {
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
    if (crit_order_on)
      std::stable_sort(out.begin(), out.end(), [&](const Connection& a, const Connection& c) { return order_key(a.net) < order_key(c.net); });
    else if (!first_net_ids.empty())
    {
      const auto mid = std::stable_partition(out.begin(), out.end(), [&](const Connection& c) { return first_net_ids.count(c.net) > 0; });
      if (opt.first_nets_ordered) std::stable_sort(out.begin(), mid, [&](const Connection& a, const Connection& c) { return first_pos(a.net) < first_pos(c.net); });
    }
    if (!opt.buses.empty()) order_buses(out);
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
    std::vector<int> drop_layer;                       // per cell (may be shorter): the pad's layer when the cell is reached through a via drop (stub = the via), else -1
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
    return it == history.end() ? 0 : static_cast<std::int64_t>(static_cast<double>(it->second) * static_cast<double>(pitch) * opt.history_weight);
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
      cc.margin.init(n);
      cc.tight.init(n);
      cc.via.init(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny));
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
    if (!use_cache) {
      ++wc().fixed_checks;
      return obs->disk_state(p, layer, hw, net, 0, true) == 2;
    }
    auto& cc = cache_for(net);
    const std::size_t gi = (static_cast<std::size_t>(layer) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(gy)) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(gx);
    if (cc.tight[gi] == INT32_MIN) {
      cc.tight[gi] = obs->fixed_code(p, layer, hw, 0, cc.rep);
      ++wc().fixed_checks;
    }
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
      ++wc().fixed_checks;
      int st = rs == 3 ? 2 : obs->disk_state(p, layer, hw, net, margin, soft);
      if (st == 2 && obs->disk_state(p, layer, hw, net, 0, soft) != 2) st = 3;
      return st;
    }
    auto& cc = cache_for(net);
    const std::size_t gi = (static_cast<std::size_t>(layer) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(gy)) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(gx);
    if (cc.margin[gi] == INT32_MIN) {
      cc.margin[gi] = obs->fixed_code(p, layer, hw, margin, cc.rep);
      ++wc().fixed_checks;
    }
    int st = 0;
    if (!code_ok(cc.margin[gi], net) || rs == 3) {
      if (cc.tight[gi] == INT32_MIN) {
        cc.tight[gi] = obs->fixed_code(p, layer, hw, 0, cc.rep);
        ++wc().fixed_checks;
      }
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
      ++wc().cell_checks;
      cell_state[idx] = static_cast<std::uint8_t>(!learned_block.empty() && learned_block.count({block_owner(net), key}) ? 2 : point_state(layer, gx, gy, net, hw));
      cell_hist[idx] = cell_state[idx] == 2 ? 0 : hist_cost(layer, gx, gy);  // history only changes between searches
    }
    const int st = cell_state[idx];
    if (st == 2) return -1;
    const std::int64_t rc = reserved_cost(layer, gx, gy, net);
    if (rc < 0) return -1;
    std::int64_t hc = cell_hist[idx] + rc;
    if (bus_mode != 0 && !bus_guide[lat_index(layer, gx, gy)]) {  // bus lane: off the neighbour lane's guide (doc 05 §38)
      if (bus_mode == 2) return -1;
      hc += bus_pen;
    }
    if (pkg_on) {  // inside a ball-grid package, tracks on its pad layer cost more: escapes take a dog-bone via early (doc 16 F3)
      const Point p = at(gx, gy);
      for (const auto& bx : pkg_boxes)
        if (layer == bx.layer && p.x >= bx.box.x0 && p.x <= bx.box.x1 && p.y >= bx.box.y0 && p.y <= bx.box.y1) {
          hc += pkg_pen;
          break;
        }
    }
    if (lp_on && layer != lp_layer) hc += lp_pen;  // layer assignment: tracks off the preferred layer (doc 16 F2)
    if (split_on && crit_split[static_cast<std::size_t>(net)] && refmap.over_void[lat_index(layer, gx, gy)]) {  // a critical net over a void of its reference plane (issue #24)
      if (opt.split_hard) return -1;
      hc += split_pen;
    }
    if (gd_on && !gd_mask[lat_index(layer, gx, gy)]) {  // preferred path of the connection (doc 16 F1)
      if (gd_hard) return -1;
      hc += gd_pen;
    }
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
    if (st == 1) return soft_unit + hc * 4;
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
        ++wc().fixed_checks;
        st = obs->via_state(p, d, drill, net, margin, soft);
      } else {
        auto& cc = cache_for(net);
        const std::size_t gi = static_cast<std::size_t>(gy) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(gx);
        if (cc.via[gi] == INT32_MIN) {
          cc.via[gi] = obs->fixed_via_code(p, d, drill, margin, cc.rep);
          ++wc().fixed_checks;
        }
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
    if (!vreserve.empty()) {
      const std::int32_t r = vreserve[static_cast<std::size_t>(w.y0 + cy) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(w.x0 + cx)];
      if (r != 0 && r != static_cast<std::int32_t>(net)) {
        if (!soft) return -1;
        extra = std::max(extra, reserve_pen);
      }
    }
    if (!pinvia.empty()) {  // --pin-via-pen: a via in another net's pin fan-out region costs more
      const std::int32_t r = pinvia[static_cast<std::size_t>(w.y0 + cy) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(w.x0 + cx)];
      if (r != 0 && r != static_cast<std::int32_t>(net)) extra += pin_via_cost;
    }
    return extra + (via_state[idx] == 1 ? soft_unit : 0);
  }
  // --pin-via-pen (D156, M22 item 6): lattice point -> net of the dense-package pin whose fan-out region (pin_via_mm past its copper plus a
  // via's radius and clearance) holds it, -1 for two nets' regions, 0 none. Vias of other nets there pay pin_via_cost: on the failing boards
  // other nets' copper within 2 mm of a pin is the main blocker (m22-research), and a via there blocks every layer.
  std::vector<std::int32_t> pinvia;
  std::int64_t pin_via_cost = 0;
  void build_pin_via() {
    pinvia.assign(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), 0);
    pin_via_cost = static_cast<std::int64_t>(opt.pin_via_pen_mm * 1e6);
    for (std::size_t pad = 0; pad < b.pads.size(); ++pad) {
      const auto& p = b.pads[pad];
      if (p.net <= 0 || p.footprint < 0 || p.type != model::PadType::Smd || !dense_footprint(p.footprint) || pad_item[pad] < 0) continue;
      const Coord r = mm_to_nm(opt.pin_via_mm) + class_via_diameter(p.net) / 2 + std::max(netclass(p.net).clearance, rules.minimums.clearance);
      const auto bx = obs->copper().items[static_cast<std::size_t>(pad_item[pad])].box.inflated(r);
      for (int gy = std::max(0, to_iy(bx.y0)); gy <= std::min(ny - 1, to_iy(bx.y1)); ++gy)
        for (int gx = std::max(0, to_ix(bx.x0)); gx <= std::min(nx - 1, to_ix(bx.x1)); ++gx) {
          auto& v = pinvia[static_cast<std::size_t>(gy) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(gx)];
          if (v == 0) v = static_cast<std::int32_t>(p.net);
          else if (v != static_cast<std::int32_t>(p.net)) v = -1;
        }
    }
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
    if (opt.escape_stubs || opt.fanout_hold) add_long_stub(w, pad, e);
    if (opt.via_drops && nl > 1 && p.footprint >= 0 && dense_footprint(p.footprint) && p.type == model::PadType::Smd) add_via_drops(w, pad, e);
    // Round pads (BGA/WLP balls) are offered a via in pad at a high cost: an inner ball can look free locally (a
    // diagonal gap) yet have no way out of the array.
    if (opt.via_in_pad && ((!any_free && e.cells.size() == n_own) || (p.shape == model::PadShape::Circle && inner_ball(pad)))) add_in_pad(w, pad, e);
    return e;
  }

  // --escape-stubs: the end of the pin's straight corridor as a start point. One exact segment from the pad centre, off the lattice, so the pins of a side leave
  // in parallel at the pad pitch whatever the lattice pitch is; then one short hop to a lattice point. Offered at a discount (a quarter of its length), so the
  // search prefers it while it is legal; when routed copper or the fixed copper has closed it, the usual start points are still there.
  void add_long_stub(const Window& w, int pad, Endpoint& e) {
    const auto it = pad_stub.find(pad);
    if (it == pad_stub.end()) return;
    const auto& p = b.pads[static_cast<std::size_t>(pad)];
    const Point V = it->second.first;
    const int l = it->second.second;
    const Coord width = track_width(p.net);
    if (!layer_ok(p.net, l) || !(p.copper & model::layer_bit(l))) return;
    if (obs->segment_state(p.pos, V, l, width, p.net, soft) == 2) return;
    for (int oy = 0; oy <= 1; ++oy)
      for (int ox = 0; ox <= 1; ++ox) {
        const int gx = to_ix(V.x) + ox, gy = to_iy(V.y) + oy;
        const int cx = gx - w.x0, cy = gy - w.y0;
        if (gx < 0 || gy < 0 || gx >= nx || gy >= ny || cx < 0 || cy < 0 || cx >= w.w || cy >= w.h) continue;
        if (point_state(l, gx, gy, p.net, width / 2) == 2) continue;
        const Point C = at(gx, gy);
        if (obs->segment_state(V, C, l, width, p.net, soft) == 2) continue;
        e.cells.emplace_back(l, static_cast<std::int64_t>(cy) * w.w + cx);
        e.stub.push_back(V);
        e.cost.push_back(static_cast<std::int64_t>(std::hypot(static_cast<double>(V.x - p.pos.x), static_cast<double>(V.y - p.pos.y)) / 4.0 +
                                                   std::hypot(static_cast<double>(C.x - V.x), static_cast<double>(C.y - V.y))));
        return;
      }
  }

  // Short pin-to-via drops (--via-drops): from the pad centre straight out in one of 8 directions to the first point where a via of the net fits against
  // the fixed copper, then a short track on another layer to the lattice. The via sits off the lattice, between the neighbours' tracks, like a designer's
  // supply via 0.7-1.5 mm beyond the pad; the lattice has no point there. Cached per pad against fixed copper; routed copper is checked per search.
  struct ViaDrop {
    int pad_layer, layer, gx, gy;
    Point via;
    std::int64_t cost;
  };
  std::unordered_map<std::int64_t, std::vector<ViaDrop>> drop_cache;
  const std::vector<ViaDrop>& via_drops(int pad) {
    const std::int64_t dkey = static_cast<std::int64_t>(pad) * 4'000'000 + track_width(b.pads[static_cast<std::size_t>(pad)].net) / 1000;
    if (auto it = drop_cache.find(dkey); it != drop_cache.end()) return it->second;
    std::vector<ViaDrop> out;
    const auto& p = b.pads[static_cast<std::size_t>(pad)];
    const NetId net = p.net;
    const Coord width = track_width(net), vd = via_diameter(net), vdrill = via_drill(net);
    const Coord stepl = std::max<Coord>(pitch / 2, 10'000);
    const bool saved_soft = soft;
    soft = false;
    if (vias_ok(net) && vd > 0)
      for (int l = 0; l < nl; ++l) {
        if (!(p.copper & model::layer_bit(l)) || !layer_ok(net, l)) continue;
        for (int d = 0; d < 8; ++d) {
          const double ux = kDx[d] / ((d & 1) ? std::numbers::sqrt2 : 1.0), uy = kDy[d] / ((d & 1) ? std::numbers::sqrt2 : 1.0);
          std::vector<char> found(static_cast<std::size_t>(nl), 0);
          for (int k = 1; k <= 40; ++k) {
            const Point V{p.pos.x + static_cast<Coord>(std::llround(ux * static_cast<double>(stepl * k))), p.pos.y + static_cast<Coord>(std::llround(uy * static_cast<double>(stepl * k)))};
            if (obs->segment_state(p.pos, V, l, width, net, true) == 2) break;  // the leg on the pad's layer is blocked, and so is everything beyond
            if (obs->via_state(V, vd, vdrill, net, 0, true) == 2 || obs->via_disallowed(V, vd, net, 0, nl - 1, model::ViaType::Through)) continue;
            const int vx = to_ix(V.x), vy = to_iy(V.y);
            for (int l2 = 0; l2 < nl; ++l2) {
              if (l2 == l || found[static_cast<std::size_t>(l2)] || !layer_ok(net, l2)) continue;
              for (int oy = 0; oy <= 1 && !found[static_cast<std::size_t>(l2)]; ++oy)
                for (int ox = 0; ox <= 1; ++ox) {
                  const int gx = vx + ox, gy = vy + oy;
                  if (gx < 0 || gy < 0 || gx >= nx || gy >= ny || fixed_point_blocked(l2, gx, gy, net, width / 2)) continue;
                  const Point C = at(gx, gy);
                  if (obs->segment_state(V, C, l2, width, net, true) == 2) continue;
                  out.push_back({l, l2, gx, gy, V,
                                 static_cast<std::int64_t>(std::hypot(static_cast<double>(V.x - p.pos.x), static_cast<double>(V.y - p.pos.y)) +
                                                           std::hypot(static_cast<double>(C.x - V.x), static_cast<double>(C.y - V.y)))});
                  found[static_cast<std::size_t>(l2)] = 1;
                  break;
                }
            }
            bool all = true;
            for (int l2 = 0; l2 < nl; ++l2)
              if (l2 != l && layer_ok(net, l2) && !found[static_cast<std::size_t>(l2)]) all = false;
            if (all) break;  // a drop to every other layer found in this direction
          }
        }
      }
    soft = saved_soft;
    return drop_cache.emplace(dkey, std::move(out)).first->second;
  }
  void add_via_drops(const Window& w, int pad, Endpoint& e) {
    const auto& p = b.pads[static_cast<std::size_t>(pad)];
    const Coord width = track_width(p.net), vd = via_diameter(p.net), vdrill = via_drill(p.net);
    const std::int64_t via_price = static_cast<std::int64_t>(opt.via_cost_mm * via_cost_mult * via_mult_of(p.net) * 1e6);
    for (const auto& x : via_drops(pad)) {
      const int cx = x.gx - w.x0, cy = x.gy - w.y0;
      if (cx < 0 || cy < 0 || cx >= w.w || cy >= w.h) continue;
      if (point_state(x.layer, x.gx, x.gy, p.net, width / 2) == 2) continue;
      if (obs->segment_state(p.pos, x.via, x.pad_layer, width, p.net, soft) == 2) continue;
      if (obs->via_state(x.via, vd, vdrill, p.net, 0, soft) == 2) continue;
      if (obs->segment_state(x.via, at(x.gx, x.gy), x.layer, width, p.net, soft) == 2) continue;
      e.drop_layer.resize(e.cells.size(), -1);
      e.cells.emplace_back(x.layer, static_cast<std::int64_t>(cy) * w.w + cx);
      e.stub.push_back(x.via);
      e.cost.push_back(x.cost + via_price);
      e.drop_layer.push_back(x.pad_layer);
    }
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
    const std::int64_t cost = static_cast<std::int64_t>(4 * opt.via_cost_mm * via_cost_mult * via_mult_of(p.net) * 1e6) +
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
  long field_runs = 0, field_gpu_fail = 0, field_cpu_runs = 0, field_hits = 0;
  struct FieldEntry {
    std::int64_t dims[6];
    std::vector<std::uint8_t> pass, via, tgt;
    std::vector<std::int32_t> field;
  };
  std::unordered_map<std::uint64_t, FieldEntry> fcache;
  std::deque<std::uint64_t> fcache_order;
  std::size_t fcache_bytes = 0;
  double field_seconds = 0;
  void build_field(const Window& w, NetId net, Coord hw, const Endpoint& dst, std::int64_t step, std::int64_t diag, std::int64_t viac) {
    (void)hw;
    const auto t0f = std::chrono::steady_clock::now();
    field_ok = false;
    const std::size_t cells = static_cast<std::size_t>(w.w) * static_cast<std::size_t>(w.h);
    auto& cc = cache_for(net);
    if (opt.field_prefill && !cc.prefilled && static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny) * static_cast<std::size_t>(nl) <= 4'000'000) {
      // The same codes the searches would compute one cell at a time (same functions, same arguments), computed up front.
      cc.prefilled = true;
      const Coord twidth = track_width(net) / 2;
      const Coord vmargin = pitch * 71 / 100 + 1;
      for (int gy = 0; gy < ny; ++gy)
        for (int gx = 0; gx < nx; ++gx) {
          const Point p = at(gx, gy);
          const std::size_t vi = static_cast<std::size_t>(gy) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(gx);
          if (cc.via[vi] == INT32_MIN) {
            cc.via[vi] = obs->fixed_via_code(p, via_diameter(net), via_drill(net), vmargin, cc.rep);
            ++wc().fixed_checks;
          }
          for (int l = 0; l < nl; ++l) {
            const std::size_t gi = (static_cast<std::size_t>(l) * static_cast<std::size_t>(ny) + static_cast<std::size_t>(gy)) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(gx);
            if (cc.tight[gi] == INT32_MIN) {
              cc.tight[gi] = obs->fixed_code(p, l, twidth, 0, cc.rep);
              ++wc().fixed_checks;
            }
          }
        }
    }
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
          const std::int32_t t = cc.tight.get(gbase + static_cast<std::size_t>(cx));
          if (t != INT32_MIN && !code_ok(t, net)) row[cx] = 0;  // known blocked by fixed copper even without margin
        }
      }
    }
    if (!vias_ok(net)) std::fill(f_via.begin(), f_via.end(), std::uint8_t{0});
    for (int cy = 0; cy < w.h; ++cy)
      for (int cx = 0; cx < w.w; ++cx) {
        const std::int32_t v = cc.via.get(static_cast<std::size_t>(w.y0 + cy) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(w.x0 + cx));
        if (v != INT32_MIN && !code_ok(v, net)) f_via[static_cast<std::size_t>(cy) * static_cast<std::size_t>(w.w) + static_cast<std::size_t>(cx)] = 0;
      }
    for (const auto& [l, ci] : dst.cells) {
      const std::size_t i = static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(ci);
      f_tgt[i] = 1;
      f_pass[i] = 1;
    }
    gpu::FieldProblem fp{w.w, w.h, nl, static_cast<std::int32_t>(step), static_cast<std::int32_t>(diag), static_cast<std::int32_t>(std::min<std::int64_t>(viac, 1'000'000'000)),
                         f_pass.data(), f_via.data(), f_tgt.data()};
    // Field cache: the field is a pure function of (window, costs, pass/via/target masks), so a stored field with identical
    // inputs is the field this call would compute. Hash to find, byte comparison to confirm: results cannot change.
    std::uint64_t fkey = 0;
    if (opt.field_cache_mb > 0) {
      std::uint64_t h = 1469598103934665603ull;
      auto mix = [&](const void* d, std::size_t n) {
        const auto* q = static_cast<const std::uint8_t*>(d);
        for (std::size_t i = 0; i < n; ++i) h = (h ^ q[i]) * 1099511628211ull;
      };
      const std::int64_t dims[6] = {w.w, w.h, nl, step, diag, std::min<std::int64_t>(viac, 1'000'000'000)};
      mix(dims, sizeof dims);
      mix(f_pass.data(), f_pass.size());
      mix(f_via.data(), f_via.size());
      mix(f_tgt.data(), f_tgt.size());
      fkey = h;
      const auto it = fcache.find(fkey);
      if (it != fcache.end()) {
        const auto& e = it->second;
        if (std::memcmp(e.dims, dims, sizeof dims) == 0 && e.pass == f_pass && e.via == f_via && e.tgt == f_tgt) {
          field = e.field;
          field_ok = true;
          ++field_hits;
          field_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0f).count();
          return;
        }
      }
    }
    // Same field on the GPU or the CPU (identical by construction), so results do not depend on GPU availability.
    // A small field is computed on the CPU: the GPU's fixed cost per field (allocation, copies, a sync per round) is more than
    // the CPU needs for it.
    const bool small = static_cast<long>(w.w) * w.h * nl < opt.gpu_field_min_cells;
    const auto st = opt.gpu_device >= 0 && !small ? gpu::field_gpu(opt.gpu_device, fp, field) : gpu::GpuStatus{false, "no GPU"};
    if (st.ok) {
      ++field_runs;
    } else {
      gpu::field_cpu(fp, field);
      if (opt.gpu_device >= 0 && !small) ++field_gpu_fail;
      ++field_cpu_runs;
    }
    field_ok = true;
    if (opt.field_cache_mb > 0) {
      const std::size_t bytes = field.size() * sizeof(std::int32_t) + f_pass.size() + f_via.size() + f_tgt.size();
      while (!fcache_order.empty() && fcache_bytes + bytes > static_cast<std::size_t>(opt.field_cache_mb) * 1'000'000) {
        const auto old = fcache.find(fcache_order.front());
        if (old != fcache.end()) {
          fcache_bytes -= old->second.field.size() * sizeof(std::int32_t) + old->second.pass.size() + old->second.via.size() + old->second.tgt.size();
          fcache.erase(old);
        }
        fcache_order.pop_front();
      }
      if (bytes <= static_cast<std::size_t>(opt.field_cache_mb) * 1'000'000) {
        FieldEntry e;
        const std::int64_t dims[6] = {w.w, w.h, nl, step, diag, std::min<std::int64_t>(viac, 1'000'000'000)};
        std::memcpy(e.dims, dims, sizeof dims);
        e.pass = f_pass;
        e.via = f_via;
        e.tgt = f_tgt;
        e.field = field;
        fcache_bytes += bytes;
        fcache_order.push_back(fkey);
        fcache[fkey] = std::move(e);
      }
    }
    ++wc().fields;
    wc().field_cells += static_cast<long>(cells) * nl;
    field_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0f).count();
  }

  // Why the last search ended without a path (failure explanation, design doc 06 T0).
  enum class Miss { None, Enclosed, Window, Budget };
  bool reach_check_now = false;  // run the reachability pre-check before the next search
  std::vector<std::uint32_t> rstamp;  // flood-fill visited marks (generation stamps)
  std::vector<std::size_t> reach_q;
  // Late reachability proof (RouterOptions::reach_after): marks and queue of the flood from the targets; and the target marks
  // of a search, in a buffer reused between searches.
  std::vector<std::uint32_t> bstamp;
  std::vector<std::uint8_t> is_target;  // per lattice cell of the search window: 1 target, 2 not (zone checks are lazy), 0 otherwise
  std::vector<std::size_t> reach_bq;
  long late_checks = 0, late_proved = 0, late_cells = 0;
  bool miss_target = false;  // the last search's miss was proved at its target end (the target is boxed in)
  long field_late = 0;       // fields built during a search (RouterOptions::field_after)
  long reach_checks = 0, reach_pruned = 0, reach_visits = 0, reach_mismatch = 0;
  bool reach_said_no = false;  // this search's pre-check found no path
  long expansion_cap = 0;  // > 0: per-search expansion limit overriding opt.max_expansions (reverse probes)
  // Per-search expansion limit: expansion_cap, else opt.max_expansions; --neg-strict-cap (D156) bounds the strict searches of the negotiation
  // passes, which retry what failed strictly in the first pass before the negotiated search that can cross copper.
  long search_cap() const {
    if (expansion_cap > 0) return expansion_cap;
    if (opt.neg_strict_cap > 0 && !soft && !strict_pass && phase == WorkPhase::Negotiation) return std::min(opt.neg_strict_cap, opt.max_expansions);
    return opt.max_expansions;
  }
  Miss last_miss = Miss::None;

  // ---- stall diagnostics (issue #8, doc 09 section 8) ----
  // With a sink attached, a failed forward search at the connection's own width keeps where it got closest to the target and the path
  // that led there; the `failure` event is built from it (stall_fields). Read-only: nothing here feeds back into a search, so a run
  // with a viewer routes exactly as one without.
  struct StallProbe {
    bool valid = false;
    int conn = -1;                    // the connection whose attempt this is
    bool soft = false;                // the search could cross routed copper
    bool edge = false;                // the reachable region touched the window's edge: a larger window might still find a way
    int layer = 0;
    Point closest{};                  // the expanded lattice point nearest the target
    std::vector<std::array<std::int64_t, 3>> path;  // x, y, layer from the source to `closest`, corners only
  } stall_probe;

  // Late reachability proof (see search, RouterOptions::reach_after): 1 when the sources' region runs out before the floods
  // meet, 2 when the targets' does, 0 when they meet or the cap is reached; `proved_edge` says whether that region touched the
  // window edge. Uses the search's generation, target marks and legality caches.
  // A pad-to-plane search (zone_tgt set, --plane-informed) has no target cells to flood back from: only the forward flood runs, and it
  // meets the target when it enters a cell the search would accept as the plane (the same lazy test). Its running out is the proof.
  int late_reach_proof(const Connection& c, const Window& w, const Endpoint& src, const Endpoint& dst, Coord hw, Coord vd, Coord vdrill, bool& proved_edge,
                       const std::function<bool(int, std::int64_t)>& zone_tgt = {}) {
    const bool zmode = static_cast<bool>(zone_tgt);
    const NetId net = c.net;
    const std::size_t cells = static_cast<std::size_t>(w.w) * static_cast<std::size_t>(w.h);
    auto tgt_of = [&](std::size_t i) -> std::uint8_t { return is_target[i]; };
    int proved = 0;
    proved_edge = false;
    const std::size_t lcells = cells * static_cast<std::size_t>(nl);
    if (rstamp.size() < lcells) rstamp.assign(lcells, 0);
    if (bstamp.size() < lcells) bstamp.assign(lcells, 0);
    ++late_checks;
    reach_q.clear();
    reach_bq.clear();
    for (const auto& [l, ci] : src.cells) {
      const std::size_t i = static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(ci);
      if (rstamp[i] != gen) rstamp[i] = gen, reach_q.push_back(i);
    }
    for (const auto& [l, ci] : dst.cells) {
      const std::size_t i = static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(ci);
      if (bstamp[i] != gen) bstamp[i] = gen, reach_bq.push_back(i);
    }
    const bool blind_micro = blind_for(net) || micro_for(net);
    auto via_ok = [&](int cx, int cy) { return blind_micro || (opt.allow_vias && via_cost_at(w, cx, cy, net, vd, vdrill) >= 0); };
    const std::size_t cap = opt.reach_after_cap > 0 ? static_cast<std::size_t>(opt.reach_after_cap) : SIZE_MAX;
    std::size_t fh = 0, bh = 0;
    bool fedge = false, bedge = false, met = false;
    auto on_edge = [&](int cx, int cy) { return cx == 0 || cy == 0 || cx == w.w - 1 || cy == w.h - 1; };
    while (!met && reach_q.size() + reach_bq.size() <= cap) {
      if (fh == reach_q.size()) {
        proved = 1;
        proved_edge = fedge;
        break;
      }
      if (!zmode && bh == reach_bq.size()) {
        proved = 2;
        proved_edge = bedge;
        break;
      }
      {  // forward: the moves of the pre-check, plus via landings on target cells of layers without tracks (the A* allows them)
        const std::size_t i = reach_q[fh++];
        const int l = static_cast<int>(i / cells);
        const std::size_t ci = i % cells;
        const int cx = static_cast<int>(ci % static_cast<std::size_t>(w.w)), cy = static_cast<int>(ci / static_cast<std::size_t>(w.w));
        fedge = fedge || on_edge(cx, cy);
        auto reach = [&](std::size_t ni) {
          if (rstamp[ni] == gen) return;
          if (bstamp[ni] == gen) met = true;
          rstamp[ni] = gen;
          reach_q.push_back(ni);
          if (zmode && zone_tgt(static_cast<int>(ni / cells), static_cast<std::int64_t>(ni % cells))) met = true;
        };
        for (int d = 0; d < 8 && !met; ++d) {
          const int ncx = cx + kDx[d], ncy = cy + kDy[d];
          if (ncx < 0 || ncy < 0 || ncx >= w.w || ncy >= w.h) continue;
          const std::size_t ni = static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(ncy) * static_cast<std::size_t>(w.w) + static_cast<std::size_t>(ncx);
          if (rstamp[ni] == gen) continue;
          if (tgt_of(ni) != 1 && cell_cost(w, l, ncx, ncy, net, hw) < 0) continue;
          reach(ni);
        }
        if (!met && nl > 1) {
          bool useful = false;
          for (int l2 = 0; l2 < nl && !useful; ++l2) useful = l2 != l && rstamp[static_cast<std::size_t>(l2) * cells + ci] != gen;
          if (useful && via_ok(cx, cy))
            for (int l2 = 0; l2 < nl && !met; ++l2) {
              const std::size_t ni = static_cast<std::size_t>(l2) * cells + ci;
              if (l2 != l && (layer_ok(net, l2) || tgt_of(ni) == 1 || (zmode && zone_tgt(l2, static_cast<std::int64_t>(ci))))) reach(ni);
            }
        }
      }
      if (met || zmode) continue;
      {  // backward: every cell with a forward move into this one
        const std::size_t i = reach_bq[bh++];
        const int l = static_cast<int>(i / cells);
        const std::size_t ci = i % cells;
        const int cx = static_cast<int>(ci % static_cast<std::size_t>(w.w)), cy = static_cast<int>(ci / static_cast<std::size_t>(w.w));
        bedge = bedge || on_edge(cx, cy);
        const bool is_tgt = tgt_of(i) == 1;
        auto reach = [&](std::size_t ni) {
          if (bstamp[ni] == gen) return;
          if (rstamp[ni] == gen) met = true;
          bstamp[ni] = gen;
          reach_bq.push_back(ni);
        };
        if (is_tgt || cell_cost(w, l, cx, cy, net, hw) >= 0)  // a planar move may enter it: from any neighbour
          for (int d = 0; d < 8 && !met; ++d) {
            const int ncx = cx + kDx[d], ncy = cy + kDy[d];
            if (ncx < 0 || ncy < 0 || ncx >= w.w || ncy >= w.h) continue;
            reach(static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(ncy) * static_cast<std::size_t>(w.w) + static_cast<std::size_t>(ncx));
          }
        if (!met && nl > 1 && (layer_ok(net, l) || is_tgt)) {  // a via may land on it: from the other layers of the cell
          bool useful = false;
          for (int l2 = 0; l2 < nl && !useful; ++l2) useful = l2 != l && bstamp[static_cast<std::size_t>(l2) * cells + ci] != gen;
          if (useful && via_ok(cx, cy))
            for (int l2 = 0; l2 < nl && !met; ++l2)
              if (l2 != l) reach(static_cast<std::size_t>(l2) * cells + ci);
        }
      }
    }
    const long visited = static_cast<long>(reach_q.size() + reach_bq.size());
    res.expansions += visited * opt.flood_work_pct / 100;
    wc().flood_cells += visited;
    late_cells += visited;
    if (proved) ++late_proved;
    return proved;
  }

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
      std::fill(bstamp.begin(), bstamp.end(), 0u);
      gen = 1;
    }
    const Endpoint src = pad_cells(w, c.pad_a);
    const Endpoint dst = c.pad_b >= 0 ? pad_cells(w, c.pad_b) : Endpoint{};
    if (src.cells.empty() || (c.pad_b >= 0 && dst.cells.empty())) return false;
    // Target marks per lattice cell of the window: 1 target, 2 not (zone checks are lazy), 0 otherwise. The buffer is reused
    // between searches and cleared for this window: one byte per cell, read on every move.
    if (is_target.size() < cells * static_cast<std::size_t>(nl)) is_target.resize(cells * static_cast<std::size_t>(nl));
    std::fill_n(is_target.begin(), static_cast<std::ptrdiff_t>(cells * static_cast<std::size_t>(nl)), std::uint8_t{0});
    auto tgt_of = [&](std::size_t i) -> std::uint8_t { return is_target[i]; };
    auto tgt_set = [&](std::size_t i, std::uint8_t v) { is_target[i] = v; };
    for (auto [l, ci] : dst.cells) tgt_set(static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(ci), 1);
    miss_target = false;
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
    geom::Box zone_box;
    if (c.zone_b >= 0) {
      const auto& z = obs->copper().items[static_cast<std::size_t>(c.zone_b)];
      zone_layer = std::countr_zero(z.layers);
      zone_poly = &z.shapes.front().pts;
      zone_box = z.shapes.front().box;
    }
    auto target = [&](int l, std::int64_t ci) -> bool {
      const std::size_t ti = static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(ci);
      const std::uint8_t t = tgt_of(ti);
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
      tgt_set(ti, inside ? 1 : 2);
      return inside;
    };
    const Point tp = c.pad_b >= 0 ? b.pads[static_cast<std::size_t>(c.pad_b)].pos : b.pads[static_cast<std::size_t>(c.pad_a)].pos;
    const bool zone_target = c.zone_b >= 0;
    // Keep the closest approach of this search for the failure event: forward, at the net's own width, for the connection being routed.
    const bool probing = opt.sink != nullptr && c.pad_b >= 0 && width_override == 0 && !force_escapes && !via_override && current >= 0 &&
                         static_cast<std::size_t>(current) < cs.size() && cs[static_cast<std::size_t>(current)].c.pad_a == c.pad_a &&
                         cs[static_cast<std::size_t>(current)].c.pad_b == c.pad_b;
    // A strict probe (routed copper blocks) is worth more than a negotiated one (it can cross routed copper): keep it.
    const bool probe_wanted = probing && !(stall_probe.valid && stall_probe.conn == current && !stall_probe.soft && soft);
    auto store_probe = [&](int l, Point closest, std::vector<std::array<std::int64_t, 3>> pth) {
      stall_probe.valid = true;
      stall_probe.conn = current;
      stall_probe.soft = soft;
      stall_probe.edge = touched_edge;
      stall_probe.layer = l;
      stall_probe.closest = closest;
      stall_probe.path = std::move(pth);
    };
    std::size_t probe_s = SIZE_MAX;
    std::int64_t probe_d2 = INT64_MAX;
    // Reachability pre-check (doc 05 §13): a flood fill over the same legality tests as the A* below, but with no
    // bend states, turn limits or costs, so it admits every path the A* could find. "Unreachable" is therefore
    // exact, found with one visit per lattice point instead of up to nine heap-ordered expansions, and the cell
    // cache it fills is reused by the A* when a path exists. Run only where a failure is likely (reach_check_now).
    bool prechecked = false;  // the pre-check ran (and, since the search goes on, found the target)
    if (reach_check_now && !soft && !zone_target && c.pad_b >= 0) {
      prechecked = true;
      const std::size_t lcells = cells * static_cast<std::size_t>(nl);
      if (rstamp.size() < lcells) rstamp.assign(lcells, 0);
      const std::size_t cap = static_cast<std::size_t>(search_cap());
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
        if (tgt_of(i) == 1) {
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
          if (tgt_of(ni) != 1 && cell_cost(w, l, ncx, ncy, net, hw) < 0) continue;
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
      res.expansions += visited * opt.flood_work_pct / 100;  // counted as work so --work budgets stay deterministic
      wc().flood_cells += visited;
      ++wc().searches;
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
          if (probe_wanted && !edge) {  // the flood has no parents: the nearest cell it reached, without a path
            for (const std::size_t i : reach_q) {
              const std::size_t ci = i % cells;
              const Point pp = at(w.x0 + static_cast<int>(ci % static_cast<std::size_t>(w.w)), w.y0 + static_cast<int>(ci / static_cast<std::size_t>(w.w)));
              const std::int64_t dx = pp.x - tp.x, dy = pp.y - tp.y, d2 = dx * dx + dy * dy;
              if (d2 < probe_d2) probe_d2 = d2, probe_s = i;
            }
            if (probe_s != SIZE_MAX) {
              const int l = static_cast<int>(probe_s / cells);
              const std::size_t ci = probe_s % cells;
              store_probe(l, at(w.x0 + static_cast<int>(ci % static_cast<std::size_t>(w.w)), w.y0 + static_cast<int>(ci / static_cast<std::size_t>(w.w))), {});
            }
          }
          return false;
        }
      }
    }
    const std::int64_t step = pitch, diag = static_cast<std::int64_t>(std::llround(static_cast<double>(pitch) * std::numbers::sqrt2));
    const double via_price = opt.via_cost_mm * via_cost_mult * via_mult_of(net);  // critical nets pay more for a via (issue #24)
    const std::int64_t via_cost = static_cast<std::int64_t>(via_price * 1e6);
    const std::int64_t dir_pen = static_cast<std::int64_t>(auto_dir_pen * static_cast<double>(pitch));
    // Cost-to-go field (GPU) for large windows: exact distances to the targets through cells not known to be
    // blocked by fixed copper; a lower bound on the true cost, so A* stays optimal while expanding far less.
    const bool use_field = !zone_target && opt.field_heuristic && use_cache && !fields_off &&
                           cells * static_cast<std::size_t>(nl) >= static_cast<std::size_t>(opt.field_min_cells);
    bool have_field = false;
    auto make_field = [&] {
      build_field(w, net, hw, dst, step, diag, via_cost);
      // Wall-clock mode only (keeps --work runs deterministic): when shared GPUs make fields cost more than
      // 30% of the run, this variant continues with the octile heuristic.
      const double el = elapsed();
      if (opt.work_budget == 0 && el > 10.0 && field_seconds > 0.3 * el) fields_off = true;
      have_field = field_ok;
    };
    // With field_after > 0 the field is built only once the search has expanded that many states (below).
    if (use_field && opt.field_after <= 0) make_field();
    // --long-weight (D156): a negotiated search of a long connection is weighted A* (Pohl 1970), at most that factor above the cheapest path: on a
    // congested board the optimal search floods the window round every crossing before it reaches a target hundreds of mm away.
    const double hweight = soft && opt.long_weight > 1.0 && c.length >= mm_to_nm(opt.long_weight_mm) ? std::max(opt.heuristic_weight, opt.long_weight) : opt.heuristic_weight;
    auto h = [&](int fl, int gx, int gy) -> std::int64_t {
      if (have_field) {
        const std::int32_t v = field[static_cast<std::size_t>(fl) * cells + static_cast<std::size_t>(gy - w.y0) * static_cast<std::size_t>(w.w) +
                                     static_cast<std::size_t>(gx - w.x0)];
        // Never prune on the field: where it claims "unreachable" fall back to the octile bound, so the search
        // stays complete even if the field's view of blocked cells is stale or wrong.
        if (v < gpu::kFieldInf) return static_cast<std::int64_t>(static_cast<double>(v) * hweight);
      }
      const Point p = at(gx, gy);
      if (zone_target) {
        if (!opt.plane_informed || zone_box.empty()) return std::int64_t{0};  // plane anywhere nearby: no useful lower bound
        // --plane-informed: octile distance to the plane's bounding box, a lower bound on the steps to any cell inside it.
        const std::int64_t bx = std::max<std::int64_t>({0, zone_box.x0 - p.x, p.x - zone_box.x1});
        const std::int64_t by = std::max<std::int64_t>({0, zone_box.y0 - p.y, p.y - zone_box.y1});
        const std::int64_t zmn = std::min(bx, by), zmx = std::max(bx, by);
        return static_cast<std::int64_t>(static_cast<double>((zmx - zmn) + zmn * diag / step) * hweight);
      }
      const std::int64_t dx = std::llabs(p.x - tp.x), dy = std::llabs(p.y - tp.y);
      const std::int64_t mn = std::min(dx, dy), mx = std::max(dx, dy);
      return static_cast<std::int64_t>(static_cast<double>((mx - mn) + mn * diag / step) * hweight);  // octile distance
    };
    auto sidx = [&](int l, std::int64_t ci, int dir) {
      return ((static_cast<std::size_t>(l) * cells + static_cast<std::size_t>(ci)) * D) + (D == 9 ? static_cast<std::size_t>(dir) : 0);
    };
    // Open list: a min-heap of (f, key). The key is the state index; with tie_deep, ties on f go first to the larger g, in
    // units of 256 (saturating at 2^32), then to the lower state index (states < 2^31: parents are int32). Its container is
    // reachable so a field built during the search can re-key it (a priority_queue of pairs is faster here than a vector
    // with a heap of structs: 4-8 % of the run on sbc and logicbone).
    using QE = std::pair<std::int64_t, std::uint64_t>;
    struct OpenList : std::priority_queue<QE, std::vector<QE>, std::greater<>> {
      using std::priority_queue<QE, std::vector<QE>, std::greater<>>::c;
    } open;
    const bool tie_deep = opt.tie_deep;
    auto qkey = [&](std::int64_t g, std::size_t st) -> std::uint64_t {
      if (!tie_deep) return st;
      constexpr std::uint64_t kMax = (std::uint64_t{1} << 32) - 1;
      const std::uint64_t gq = std::min<std::uint64_t>(static_cast<std::uint64_t>(std::max<std::int64_t>(g, 0)) >> 8, kMax);
      return ((kMax - gq) << 31) | st;
    };
    auto qstate = [&](const QE& e) -> std::size_t { return tie_deep ? static_cast<std::size_t>(e.second & ((std::uint64_t{1} << 31) - 1)) : static_cast<std::size_t>(e.second); };
    auto push = [&](std::int64_t g, std::int64_t f, std::size_t st) { open.emplace(f, qkey(g, st)); };
    const Point sp = b.pads[static_cast<std::size_t>(c.pad_a)].pos;
    start_stub.clear();
    target_stub.clear();
    start_drop.clear();
    target_drop.clear();
    for (std::size_t k = 0; k < dst.cells.size(); ++k) {
      const auto key = cell_key(dst.cells[k].first, w.x0 + static_cast<int>(dst.cells[k].second % w.w), w.y0 + static_cast<int>(dst.cells[k].second / w.w));
      if (!target_stub.count(key) || dst.stub[k] == tp) {
        target_stub[key] = dst.stub[k];
        if (k < dst.drop_layer.size() && dst.drop_layer[k] >= 0) target_drop[key] = dst.drop_layer[k];
        else target_drop.erase(key);
      }
    }
    (void)sp;
    for (std::size_t k = 0; k < src.cells.size(); ++k) {
      const auto [l, ci] = src.cells[k];
      const int cx = static_cast<int>(ci % w.w), cy = static_cast<int>(ci / w.w);
      const std::int64_t g0 = src.cost[k];
      const std::size_t s = sidx(l, ci, kNoDir);
      if ((sn[s].tag & kGenMask) == gen && sn[s].g <= g0) continue;
      start_stub[cell_key(l, w.x0 + cx, w.y0 + cy)] = src.stub[k];
      if (k < src.drop_layer.size() && src.drop_layer[k] >= 0) start_drop[cell_key(l, w.x0 + cx, w.y0 + cy)] = src.drop_layer[k];
      else start_drop.erase(cell_key(l, w.x0 + cx, w.y0 + cy));
      sn[s] = SNode{g0, gen | (static_cast<std::uint32_t>(kNoDir) << 28), -1};
      if (h(l, w.x0 + cx, w.y0 + cy) >= kUnreachable) continue;
      push(g0, g0 + h(l, w.x0 + cx, w.y0 + cy), s);
    }
    long expanded = 0;
    const long exp_cap = search_cap();
    const bool coupled_net = obs->coupling_net(net);  // KiCad's pair-gap coupling is checked on every step (doc 05 §59)
    std::size_t goal = SIZE_MAX;
    // Late reachability proof (RouterOptions::reach_after, doc 05 §62). A strict search that has expanded reach_after states is
    // most likely one that cannot reach its target: on the hard PCBench boards successful searches expand a median of 30-230
    // states, while failed ones flood their window with up to nine states per lattice point and took 77-91 % of the first
    // pass. Two floods over the moves the pre-check above uses (no bends, turn limits or costs: a superset of the A*'s moves),
    // one forward from the sources and one backward from the targets, take a cell in turn. When either runs out before they
    // meet, no path exists, which is exact; the end that ran out says whether the source or the target is boxed in, and
    // whether its region touched the window edge (a larger window might help). When they meet, or reach_after_cap cells are
    // visited, the A* goes on unchanged: the floods only filled the legality caches it reads. Flood cells count against the
    // budget like the pre-check's.
    const bool zone_proof = zone_target && opt.plane_informed;  // --plane-informed: forward-only proof for pad-to-plane searches
    const bool late_proof = opt.reach_after > 0 && !soft && ((!zone_target && c.pad_b >= 0) || zone_proof) && !prechecked;
    bool late_done = false, field_tried = false, late_verify = false;
    int proved = 0;           // 1: the source's region ran out, 2: the target's
    bool proved_edge = false;  // that region touched the window edge
    auto run_late_proof = [&] {
      if (zone_proof)
        proved = late_reach_proof(c, w, src, dst, hw, vd, vdrill, proved_edge, [&](int l, std::int64_t ci) { return target(l, ci); });
      else
        proved = late_reach_proof(c, w, src, dst, hw, vd, vdrill, proved_edge);
    };
    while (!open.empty()) {
      const std::int64_t f = open.top().first;
      const std::size_t s = qstate(open.top());
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
      if (late_proof && !late_done && expanded >= opt.reach_after) {
        late_done = true;
        run_late_proof();
        if (proved && opt.reach_verify) {  // reference check: search on; a path found is a mismatch
          late_verify = true;
          proved = 0;
        }
        if (proved) break;
      }
      if (use_field && !field_tried && opt.field_after > 0 && expanded >= opt.field_after) {
        field_tried = true;
        // The field is built now and the open list re-keyed with it: each state's live entry (the one whose f matches its
        // g under the old heuristic) gets g + the new h; the others were stale anyway.
        std::vector<QE> kept;
        kept.reserve(open.size() + 1);
        auto keep = [&](const QE& e) {
          const std::size_t es = qstate(e), elc = es / D;
          const std::int64_t ci2 = static_cast<std::int64_t>(elc % cells);
          if (e.first - h(static_cast<int>(elc / cells), w.x0 + static_cast<int>(ci2 % w.w), w.y0 + static_cast<int>(ci2 / w.w)) <= sn[es].g) kept.push_back(e);
        };
        keep(QE{f, qkey(sn[s].g, s)});  // the state being expanded goes back too: the new key may put another state first
        for (const auto& e : open.c) keep(e);
        make_field();
        ++field_late;
        for (auto& e : kept) {
          const std::size_t es = qstate(e), elc = es / D;
          const std::int64_t ci2 = static_cast<std::int64_t>(elc % cells);
          e.first = sn[es].g + h(static_cast<int>(elc / cells), w.x0 + static_cast<int>(ci2 % w.w), w.y0 + static_cast<int>(ci2 / w.w));
        }
        open.c = std::move(kept);
        std::make_heap(open.c.begin(), open.c.end(), std::greater<>{});
        --expanded;  // the state was not expanded
        continue;
      }
      if (probe_wanted) {  // the expanded state nearest the target (ties: the first)
        const Point pp = at(w.x0 + cx, w.y0 + cy);
        const std::int64_t dx = pp.x - tp.x, dy = pp.y - tp.y, d2 = dx * dx + dy * dy;
        if (d2 < probe_d2) probe_d2 = d2, probe_s = s;
      }
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
        if (coupled_net && obs->coupling_state(at(w.x0 + cx, w.y0 + cy), at(w.x0 + ncx, w.y0 + ncy), l, 2 * hw, net, soft, nullptr) == 2) continue;
        std::int64_t cost = on_layer((d & 1) ? diag : step, l) + extra;
        if (dir_pen > 0 && !(d & 1) && ((d == 0 || d == 4) != ((l % 2 == 0) != opt.layer_dir_swap))) cost += dir_pen;  // against the layer's preferred direction (doc 16)
        if (dir != kNoDir && d != dir) cost += ((std::min((d - dir + 8) % 8, (dir - d + 8) % 8) == 1) ? step / 2 : 2 * step);
        const std::size_t ns = sidx(l, nci, d);
        if ((sn[ns].tag & kGenMask) == gen && sn[ns].g <= gs + cost) continue;
        const std::int64_t hn = h(l, w.x0 + ncx, w.y0 + ncy);
        if (hn >= kUnreachable) continue;  // cannot reach a target from there
        sn[ns] = SNode{gs + cost, gen | (static_cast<std::uint32_t>(d) << 28), static_cast<std::int32_t>(s)};
        push(gs + cost, gs + cost + hn, ns);
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
          const std::int64_t ng = gs + via_cost * 3 / 2 + (vs == 1 ? soft_unit : 0) +
                                  textra(static_cast<std::size_t>(l2) * cells + static_cast<std::size_t>(ci));
          if ((sn[ns].tag & kGenMask) == gen && sn[ns].g <= ng) continue;
          const std::int64_t hn = h(l2, w.x0 + cx, w.y0 + cy);
          if (hn >= kUnreachable) continue;
          sn[ns] = SNode{ng, gen | (static_cast<std::uint32_t>(kNoDir) << 28), static_cast<std::int32_t>(s)};
          push(ng, ng + hn, ns);
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
          push(ng, ng + hn, ns);
        }
      }
    }
    if ((reach_said_no || late_verify) && goal != SIZE_MAX) ++reach_mismatch;
    res.expansions += expanded;
    wc().expansions += expanded;
    ++wc().searches;
    (goal == SIZE_MAX ? exp_fail : exp_ok) += expanded;
    if (goal == SIZE_MAX) (soft ? (corr_hard ? xf_soft_conf : xf_soft_wide) : (corr_hard ? xf_strict_conf : xf_strict_wide)) += expanded;
    (goal == SIZE_MAX ? n_fail : n_ok) += 1;
    if (goal == SIZE_MAX) {
      // Open list exhausted without reaching the window edge: the source is boxed in, so a larger window
      // cannot help (Contour's boxed-in terminal test).
      last_miss = expanded > exp_cap ? Miss::Budget : touched_edge ? Miss::Window : Miss::Enclosed;
      if (proved) {
        last_miss = proved_edge ? Miss::Window : Miss::Enclosed;
        miss_target = proved == 2 && !proved_edge;
        touched_edge = proved_edge;
      }
      if (probe_wanted && probe_s != SIZE_MAX && last_miss != Miss::Budget) {
        // The way to the closest approach, corners only: back along the parents, then the collinear points dropped.
        std::vector<std::array<std::int64_t, 3>> raw;
        for (std::int64_t t = static_cast<std::int64_t>(probe_s); t >= 0 && raw.size() < 100'000; t = sn[static_cast<std::size_t>(t)].parent) {
          const std::size_t lc = static_cast<std::size_t>(t) / D;
          const int l = static_cast<int>(lc / cells);
          const std::int64_t ci = static_cast<std::int64_t>(lc % cells);
          const Point pp = at(w.x0 + static_cast<int>(ci % w.w), w.y0 + static_cast<int>(ci / w.w));
          raw.push_back({pp.x, pp.y, l});
          if (sn[static_cast<std::size_t>(t)].parent < 0) break;
        }
        std::reverse(raw.begin(), raw.end());
        std::vector<std::array<std::int64_t, 3>> corners;
        for (std::size_t i = 0; i < raw.size(); ++i) {
          if (i == 0 || i + 1 == raw.size()) {
            corners.push_back(raw[i]);
            continue;
          }
          const auto &a0 = raw[i - 1], &a1 = raw[i], &a2 = raw[i + 1];
          const bool straight = a0[2] == a1[2] && a1[2] == a2[2] && (a1[0] - a0[0]) * (a2[1] - a1[1]) == (a1[1] - a0[1]) * (a2[0] - a1[0]);
          if (!straight) corners.push_back(a1);
        }
        const std::size_t lc = probe_s / D;
        const std::int64_t ci = static_cast<std::int64_t>(lc % cells);
        store_probe(static_cast<int>(lc / cells), at(w.x0 + static_cast<int>(ci % w.w), w.y0 + static_cast<int>(ci / w.w)), std::move(corners));
      }
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
          else if (obs->disk_state(at(gx, gy), l, hw, net, 0, true) == 2) {
            ++fixed;
            if (std::getenv("TM_DEBUG_ENCLOSED")[0] == '2' && fixed % 8 == 0)
              std::fprintf(stderr, "  blocked cell L%d (%.3f, %.3f)\n", l, nm_to_mm(at(gx, gy).x), nm_to_mm(at(gx, gy).y));
          } else ++other;
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
    ++wc().commits;
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
    bool start_drops = false;
    if (auto it = start_stub.find(cell_key(path.front().layer, path.front().gx, path.front().gy)); it != start_stub.end() && !(it->second == pa)) {
      if (const auto dr = start_drop.find(cell_key(path.front().layer, path.front().gx, path.front().gy)); dr != start_drop.end()) {  // pin -> via on the pad's layer, then on
        segs.push_back({pa, it->second, dr->second});
        vias.push_back(it->second);
        via_span.emplace_back(std::min(layer, dr->second), std::max(layer, dr->second));
        start_drops = true;
      } else {
        segs.push_back({pa, it->second, layer});
      }
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
        if (const auto dr = target_drop.find(cell_key(path.back().layer, path.back().gx, path.back().gy)); dr != target_drop.end()) {  // via, then the pad's layer to the pin
          vias.push_back(it->second);
          via_span.emplace_back(std::min(layer, dr->second), std::max(layer, dr->second));
          layer = dr->second;
        }
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
    if (!start_drops && in_pad_via_needed(c.pad_a, path.front().layer)) pad_vias.push_back(pa);
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
    std::sort(victims.begin(), victims.end());
    victims.erase(std::unique(victims.begin(), victims.end()), victims.end());
    if (!victims.empty() && current >= 0) {  // what stood in the way of this route, for the stuck report; a later success makes it moot
      auto& bl = cs[static_cast<std::size_t>(current)].blockers;
      bl.clear();
      for (int v : victims) bl.push_back(cs[static_cast<std::size_t>(v)].c.net);
      std::sort(bl.begin(), bl.end());
      bl.erase(std::unique(bl.begin(), bl.end()), bl.end());
    }
    if (!ok) {
      commit_why = "exact check rejected the lattice path";
      return false;
    }
    for (int v : victims)
      if (first_net_ids.count(cs[static_cast<std::size_t>(v)].c.net) && !first_net_ids.count(net)) {
        commit_why = "would rip a --first-nets connection";
        return false;
      }
    if (!net_bus.empty() && !bus_net(net))  // a bus is ripped by its own kind only (doc 05 §38)
      for (int v : victims)
        if (bus_net(cs[static_cast<std::size_t>(v)].c.net)) {
          commit_why = "would rip a bus connection";
          return false;
        }
    if (opt.bundle_escape || opt.package_tunnels)  // the lanes of a bundle escape and the tunnels are what the rest of the board is routed around
      for (int v : victims)
        if (cs[static_cast<std::size_t>(v)].bundle_lane) {
          commit_why = "would rip a bundle lane";
          return false;
        }
    if (!txn_protect.empty())
      for (int v : victims)
        if (txn_protect[static_cast<std::size_t>(v)]) {
          commit_why = "would rip a connection of the running transaction";
          return false;
        }
    if (opt.max_victims > 0 && static_cast<int>(victims.size()) > opt.max_victims) {
      commit_why = "would rip too many connections at once";
      return false;
    }
    for (int v : victims)
      if (cs[static_cast<std::size_t>(v)].rips >= rip_cap) {
        commit_why = "would rip a connection already ripped too often";
        return false;
      }
    // Rip up victims, and raise history on the contested cells so later searches avoid them.
    for (const auto& s : merged) bump_history(s, net);  // before the rip, while the conflicts still exist
    if (txn_on) {  // transaction log: what rolling this route back must restore
      for (int v : victims) {
        TxnVictim tv;
        tv.ci = v;
        for (int item : cs[static_cast<std::size_t>(v)].items) {
          const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
          tv.items.push_back({it.kind, it.index});
        }
        txn_log.push_back(std::move(tv));
      }
      for (std::size_t k = 0; k < cs.size(); ++k)
        if (cs[k].implicit && cs[k].routed)
          for (int v : victims)
            if (cs[static_cast<std::size_t>(v)].c.net == cs[k].c.net) {
              txn_implicit.push_back(static_cast<int>(k));
              break;
            }
    }
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

  // Transactional negotiation (doc 16 §12). While txn_on, commit() logs the victims of a route; txn_finish() then tries
  // to re-route each victim without ripping anything and keeps the route only if no more victims failed than the route gained
  // (opt.txn: 1 keeps ties, 2 needs a gain). Otherwise the board goes back to exactly the state before the route.
  struct OldItem { drc::ItemKind kind; int index; };
  struct TxnVictim { int ci; std::vector<OldItem> items; };
  bool txn_on = false;
  std::vector<TxnVictim> txn_log;
  std::vector<int> txn_implicit;
  int txn_attempts_pass = 0;
  std::uint64_t txn_draws = 0;
  // Whole-board snapshot for rolling a transaction back (cheap next to one search: a few hundred copper items).
  struct TxnSnap {
    std::vector<std::vector<OldItem>> items;
    std::vector<std::uint8_t> routed, implicit;
    std::vector<int> rips;
    int res_routed = 0, res_rips = 0;
  };
  TxnSnap txn_capture() {
    TxnSnap snap_;
    snap_.items.resize(cs.size());
    for (std::size_t k = 0; k < cs.size(); ++k) {
      for (int item : cs[k].items) {
        const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
        snap_.items[k].push_back({it.kind, it.index});
      }
      snap_.routed.push_back(cs[k].routed ? 1 : 0);
      snap_.implicit.push_back(cs[k].implicit ? 1 : 0);
      snap_.rips.push_back(cs[k].rips);
    }
    snap_.res_routed = res.routed;
    snap_.res_rips = res.rips;
    return snap_;
  }
  void txn_restore(const TxnSnap& snap_) {
    for (std::size_t k = 0; k < cs.size(); ++k) lift_items(static_cast<int>(k));
    for (std::size_t k = 0; k < cs.size(); ++k) {
      restore_items(static_cast<int>(k), snap_.items[k]);
      cs[k].routed = snap_.routed[k] != 0;
      cs[k].implicit = snap_.implicit[k] != 0;
      cs[k].rips = snap_.rips[k];
    }
    res.routed = snap_.res_routed;
    res.rips = snap_.res_rips;
  }
  std::vector<std::uint8_t> txn_protect;  // connections a nested search must not rip: the transaction's route and its re-routed victims
  int txn_nested_left = 0;
  // Routes connection ci, negotiated, as a transaction. The route is kept only if the victims it rips can be re-routed
  // (strictly, or at depth > 0 by a nested transaction of their own); `gain` is then 1 minus the victims left unrouted.
  // Nested transactions need a full gain; the top level also keeps ties (opt.txn == 1) and, with a temperature, some losses.
  bool txn_try(int ci, int depth, bool top, int& gain) {
    gain = 0;
    if (txn_protect.size() != cs.size()) txn_protect.assign(cs.size(), 0);
    const int saved_current = current;
    const Miss saved_miss = last_miss;
    const TxnSnap snap = txn_capture();
    current = ci;
    txn_on = true;
    txn_log.clear();
    txn_implicit.clear();
    const bool ok = search_and_commit(cs[static_cast<std::size_t>(ci)].c, true);
    txn_on = false;
    std::vector<TxnVictim> log;
    log.swap(txn_log);
    txn_implicit.clear();
    current = saved_current;
    if (!ok) return false;  // nothing was changed
    if (log.empty()) {
      gain = 1;
      return true;
    }
    ++res.txn_tried;
    ++txn_attempts_pass;
    txn_protect[static_cast<std::size_t>(ci)] = 1;
    std::vector<int> protectedv;
    int loss = 0;
    // The victims are re-routed one after the other, so their order matters (a bundle fits back only if its wires go in the
    // right sequence). Up to opt.txn_orders orders are tried: the log order, then each time the victims that failed go first.
    std::vector<int> order;
    for (const auto& tv : log) order.push_back(tv.ci);
    const int attempts = std::max(1, opt.txn_orders);
    TxnSnap post;
    if (attempts > 1) post = txn_capture();
    for (int attempt = 0; attempt < attempts; ++attempt) {
      if (attempt > 0) {
        for (int v : protectedv) txn_protect[static_cast<std::size_t>(v)] = 0;
        protectedv.clear();
        txn_restore(post);
      }
      loss = 0;
      std::vector<int> failed_now, ok_now;
      for (const int v : order) {
        auto& sv = cs[static_cast<std::size_t>(v)];
        if (sv.routed) continue;
        if (out_of_budget()) {
          ++loss;
          failed_now.push_back(v);
          continue;
        }
        current = v;
        expansion_cap = opt.txn_cap;
        bool again = search_and_commit(sv.c, false);
        expansion_cap = 0;
        if (!again && depth > 0 && txn_nested_left > 0) {
          --txn_nested_left;
          int g = 0;
          again = txn_try(v, depth - 1, false, g);
        }
        current = ci;
        if (again) {
          sv.routed = true;
          ++res.routed;
          txn_protect[static_cast<std::size_t>(v)] = 1;
          protectedv.push_back(v);
          ok_now.push_back(v);
        } else {
          ++loss;
          failed_now.push_back(v);
        }
      }
      if (loss == 0 || out_of_budget()) break;
      order = failed_now;  // the ones that did not fit go first next time
      order.insert(order.end(), ok_now.begin(), ok_now.end());
      std::reverse(order.begin() + static_cast<std::ptrdiff_t>(failed_now.size()), order.end());
    }
    txn_protect[static_cast<std::size_t>(ci)] = 0;
    for (int v : protectedv) txn_protect[static_cast<std::size_t>(v)] = 0;
    current = saved_current;
    last_miss = saved_miss;
    const int net = 1 - loss;
    if (std::getenv("TM_DEBUG_TXN"))
      std::fprintf(stderr, "txn pass %d conn %d depth %d: %zu victims, %d lost, net %d, expansions %ld\n", res.passes, ci, depth, log.size(), loss, net, res.expansions);
    // Annealing acceptance (counter-based draw: deterministic): a loss of k connections is kept with probability exp(-k / T).
    bool accept = net > 0 || (top && net == 0 && opt.txn == 1);
    if (!accept && top && net < 0 && opt.txn_temp > 0) {
      double temp = opt.txn_temp;
      if (opt.txn_cool) {
        const double frac = opt.work_budget > 0 ? static_cast<double>(res.expansions) / (static_cast<double>(opt.work_budget) * budget_scale)
                                                : elapsed() / (opt.time_limit_s * budget_scale);
        temp *= std::max(0.0, 1.0 - frac);
      }
      if (temp > 1e-9) accept = RngStream(opt.seed, 0x7A11u, 0).uniform(txn_draws++) < std::exp(static_cast<double>(net) / temp);
    }
    if (accept) {
      ++res.txn_kept;
      gain = net;
      return true;
    }
    txn_restore(snap);
    last_miss = saved_miss;
    why = "transaction rolled back";
    return false;
  }

  // Via retry (D153, M22 item 5): a connection whose search stopped at its expansion cap is searched again at once, strictly, with a via at a
  // quarter of its price and the layer that holds the most routed track at twice the cost per mm, so the route leaves the crowded layer
  // through a via instead of looking for a way along it. Only cap misses: a proved enclosure or a miss at the window's edge does not
  // depend on prices (m22-research). Layer costs only go up, so the octile and field heuristics stay lower bounds.
  bool via_retry(int ci) {
    const auto& st = cs[static_cast<std::size_t>(ci)];
    if (nl < 2 || !opt.allow_vias || !vias_ok(st.c.net)) return false;
    std::vector<double> len(static_cast<std::size_t>(nl), 0.0);
    for (const auto& it : obs->copper().items) {
      if (it.owner < 0 || it.removed || it.kind != drc::ItemKind::Track) continue;
      const auto& t = b.tracks[static_cast<std::size_t>(it.index)];
      len[static_cast<std::size_t>(t.layer)] += std::hypot(static_cast<double>(t.b.x - t.a.x), static_cast<double>(t.b.y - t.a.y));
    }
    const int busy = static_cast<int>(std::max_element(len.begin(), len.end()) - len.begin());
    const auto saved_pm = layer_pm;
    const double saved_mult = via_cost_mult;
    layer_pm.resize(static_cast<std::size_t>(nl), 1000);
    if (len[static_cast<std::size_t>(busy)] > 0) layer_pm[static_cast<std::size_t>(busy)] = std::max<std::int64_t>(layer_pm[static_cast<std::size_t>(busy)], 2000);
    via_cost_mult = saved_mult / 4;
    via_retry_on = true;
    const bool ok = search_and_commit(st.c, false);
    via_retry_on = false;
    via_cost_mult = saved_mult;
    layer_pm = saved_pm;
    if (ok) ++res.via_retried;
    if (std::getenv("TM_DEBUG_VIA_RETRY"))
      std::fprintf(stderr, "via retry conn %d (%s), busy layer %d: %s\n", ci, b.nets[static_cast<std::size_t>(st.c.net)].name.c_str(), busy, ok ? "routed" : why.c_str());
    return ok;
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
    unrelease_escapes(v);
    const bool was_coupled = st.coupled;
    st.coupled = false;
    st.bus_lane = false;
    st.bundle_lane = false;
    ++st.rips;
    ++res.rips;
    ++wc().rips;
    pending.push_back(v);
    if (was_coupled && !pair_partner.empty()) {  // the other half goes too: the pair is re-routed coupled or not at all
      const int p = pair_partner[static_cast<std::size_t>(v)];
      if (p >= 0 && cs[static_cast<std::size_t>(p)].coupled && cs[static_cast<std::size_t>(p)].routed) rip(p);
    }
    for (std::size_t k = 0; k < cs.size(); ++k)
      if (cs[k].implicit && cs[k].routed && cs[k].c.net == st.c.net) {
        cs[k].routed = cs[k].implicit = false;
        unrelease_escapes(static_cast<int>(k));
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
  bool via_retry_on = false;    // the search under way is a via retry (its own nogoods)
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
  std::unordered_map<std::int64_t, int> start_drop, target_drop;    // lattice point -> the pad's layer, when its stub is the via of a via drop
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
                                        (force_escapes ? 2u : 0u) ^ (via_retry_on ? 8u : 0u) ^ (static_cast<std::uint64_t>(width_override) << 20) ^ (!conn_guide.empty() && current >= 0 && conn_guide[static_cast<std::size_t>(current)] >= 0 ? 4u : 0u));
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
    const int gi = (current >= 0 && static_cast<std::size_t>(current) < conn_guide.size()) ? conn_guide[static_cast<std::size_t>(current)] : -1;
    if (!conn_lpref.empty() && current >= 0 && static_cast<std::size_t>(current) < conn_lpref.size() && conn_lpref[static_cast<std::size_t>(current)] >= 0) {
      lp_on = true;
      lp_layer = conn_lpref[static_cast<std::size_t>(current)];
      lp_pen = static_cast<std::int64_t>(opt.layer_assign_pen * static_cast<double>(pitch));
    }
    bool ok = false;
    if (gi >= 0 && bus_mode == 0) {
      guide_set(gi);
      // First a search inside the guide, cut off at a few expansions per cell (as a bus lane's, doc 05 §38): a guide
      // that leads somewhere is followed at once, one that does not costs little. Then the usual searches with the
      // guide as a mild preference.
      Window w;
      w.x0 = std::clamp(to_ix(gd_box.x0) - 1, 0, nx - 1);
      w.y0 = std::clamp(to_iy(gd_box.y0) - 1, 0, ny - 1);
      w.w = std::clamp(to_ix(gd_box.x1) + 1, 0, nx - 1) - w.x0 + 1;
      w.h = std::clamp(to_iy(gd_box.y1) + 1, 0, ny - 1) - w.y0 + 1;
      gd_hard = true;
      expansion_cap = std::max<long>(50'000, 4 * static_cast<long>(gd_cells.size()) / std::max(1, nl));
      std::vector<PathNode> gpath;
      const bool found = !out_of_budget() && search(c, w, gpath);
      expansion_cap = 0;
      gd_hard = false;
      last_miss = Miss::None;
      if (found && commit(c, gpath)) ok = true;
      if (std::getenv("TM_DEBUG_GUIDE"))
        std::fprintf(stderr, "guide conn %d (net %d): cells %zu cap-found %d committed %d%s\n", current, static_cast<int>(c.net), gd_cells.size(), found ? 1 : 0, ok ? 1 : 0, soft ? " soft" : "");
      gd_pen = static_cast<std::int64_t>(opt.guide_pen * static_cast<double>(pitch));
    }
    if (!ok) ok = search_and_commit_inner(c);
    lp_on = false;
    if (gd_on) guide_clear();
    corr = nullptr;
    if (!ok && !bypass_nogoods) nogoods[ng] = 1;
    return ok;
  }

  // How crowded a connection's end is: copper of other nets routed in this run, and pads of other nets of the pad's own part, within
  // 1 mm of the pad's box. Copper on the input board is not counted (the routed-item index holds only this run's).
  int end_crowding(int pad, NetId net) const {
    const auto& pd = b.pads[static_cast<std::size_t>(pad)];
    const Coord h = std::max(pd.size_x, pd.size_y) / 2 + 1'000'000;
    geom::Box bx;
    bx.add(Point{pd.pos.x - h, pd.pos.y - h});
    bx.add(Point{pd.pos.x + h, pd.pos.y + h});
    std::vector<int> ids;
    obs->routed_items_in(bx, ids);
    int n = 0;
    for (const int id : ids) n += obs->copper().items[static_cast<std::size_t>(id)].net != net;
    if (pd.footprint >= 0)
      for (const int q : b.footprints[static_cast<std::size_t>(pd.footprint)].pads) {
        const auto& o = b.pads[static_cast<std::size_t>(q)];
        n += o.net != net && o.pos.x >= bx.x0 && o.pos.x <= bx.x1 && o.pos.y >= bx.y0 && o.pos.y <= bx.y1;
      }
    return n;
  }

  // opt.crowded_source: a strict search starts from the more crowded end. A boxed-in source is proved in a few expansions; a boxed-in
  // target is found only after the search has flooded its window (doc 05 §55).
  bool search_and_commit_inner(const Connection& c) {
    if (opt.crowded_source && strict_pass && !soft && c.pad_b >= 0 && end_crowding(c.pad_b, c.net) > end_crowding(c.pad_a, c.net)) {
      Connection r = c;
      std::swap(r.pad_a, r.pad_b);
      return search_and_commit_dir(r);
    }
    return search_and_commit_dir(c);
  }

  bool search_and_commit_dir(const Connection& c) {
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
    int attempts = strict_pass ? std::min(2, opt.max_attempts) : soft ? std::min(opt.soft_attempts, opt.max_attempts) : opt.max_attempts;
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
      const long dbg_e0 = res.expansions;
      const bool found_path = search(c, w, path);
      reach_check_now = false;
      if (const char* dn = std::getenv("TM_DEBUG_CONN"); dn && b.nets[static_cast<std::size_t>(c.net)].name.find(dn) != std::string::npos)
        std::fprintf(stderr, "[conn %d %s] pass %d soft %d attempt %d window %.0fx%.0f mm: found %d miss %d, %ld expansions (total %ld)\n", current,
                     b.nets[static_cast<std::size_t>(c.net)].name.c_str(), res.passes, soft ? 1 : 0, attempt, nm_to_mm(static_cast<Coord>(w.w) * pitch),
                     nm_to_mm(static_cast<Coord>(w.h) * pitch), found_path ? 1 : 0, static_cast<int>(last_miss), res.expansions - dbg_e0, res.expansions);
      if (!found_path) {
        why = last_miss == Miss::Enclosed ? (miss_target ? "boxed in (target)" : "boxed in") : last_miss == Miss::Budget ? "search budget" : "no path in window";
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
        // --window-escalate (D156, M22 item 3): a search that ran into its window's edge on its last window gets one whole-board window.
        // The long detours that head away first (Aleste's /RAM/MA1, 25.4 mm apart, routed in 217.5 mm with 4 vias) fit no smaller one.
        if (opt.window_escalate && attempt == attempts - 1 && attempts < 4 && last_miss == Miss::Window) {
          attempts = 4;
          attempt = 2;
          ++res.window_escalations;
        }
        continue;
      }
      std::vector<PathNode> refined;
      if (refine_critical(c, a, e, path, refined) && commit(c, refined)) return true;  // else the first path, which was legal, is tried as it was
      if (commit(c, path)) return true;
      why = commit_why;
    }
    return false;
  }

  // A critical net's first path may run over a void of its reference plane or hop layers where a dearer via was asked for: the way round can lie outside the
  // small first window. Then, and only then, the connection is searched again in a 20 mm window (issue #24). A* is optimal in its window and this one contains
  // the first, so the new path costs no more; a search that does not finish (budget) leaves the first path, and so does a refined path that fails the exact
  // check at the commit (the caller tries the first one then). Connections that fail, or that have nothing to improve, cost no extra search: a window this
  // size on a connection that cannot be routed would eat the work budget.
  bool refine_critical(const Connection& c, Point a, Point e, const std::vector<PathNode>& path, std::vector<PathNode>& refined) {
    if (c.pad_b < 0 || path.size() < 2 || crit_rank.empty() || crit_rank[static_cast<std::size_t>(c.net)] == 0) return false;
    bool voids = false, hops = false;
    for (std::size_t i = 0; i < path.size(); ++i) {
      if (split_on && crit_split[static_cast<std::size_t>(c.net)] && refmap.over_void[lat_index(path[i].layer, path[i].gx, path[i].gy)]) voids = true;
      if (i > 0 && path[i].layer != path[i - 1].layer) hops = true;
    }
    if (!voids && !(hops && via_mult_of(c.net) > 1.0)) return false;
    const Coord m = 20'000'000 + c.length / 4;
    Window w;
    w.x0 = std::max(0, to_ix(std::min(a.x, e.x) - m));
    w.y0 = std::max(0, to_iy(std::min(a.y, e.y) - m));
    w.w = std::min(nx - 1, to_ix(std::max(a.x, e.x) + m)) - w.x0 + 1;
    w.h = std::min(ny - 1, to_iy(std::max(a.y, e.y) + m)) - w.y0 + 1;
    const Miss saved_miss = last_miss;
    const bool found = search(c, w, refined) && refined.size() >= 2;
    last_miss = saved_miss;
    return found;
  }

  // The widest track that can get from the closest approach on towards the target (issue #8): the bottleneck of the best way, a
  // widest-path (max-min) search over a probing grid in a corridor of +-2 mm around the straight line from p0 to the target and at most
  // 12 mm long. A node's value is the widest track that is legal there (binary search on the width against the router's own obstacle
  // tests, so clearances, rules, pads, holes and edges are exactly the ones the lattice used); a way's value is its smallest node; layer
  // changes need a legal via. The goal is the far end of the corridor. With `fixed_only`, routed copper does not count (it can be ripped),
  // so the answer is what placement and the rules alone allow. Deterministic budgets: at most `max_nodes` nodes are given a value, and the
  // calls to the obstacle tests are taken from `calls_left`; a search that runs out of either says so (`budget`). Only const Obstacles
  // queries: no work is counted and no state changes, so a run with a viewer routes as one without.
  struct Way {
    Coord width = 0;      // the widest track that gets through (0: none, not even a hairline)
    Point at{};           // where it is narrowest
    int layer = 0;
    bool reached = false;
    bool budget = false;  // ran out of nodes or calls before an answer: the result is not evidence
  };
  Way widest_way(Point p0, int layer0, Point goal, NetId net, Coord cap, bool fixed_only, long& calls_left) const {
    Way out;
    const double dx = static_cast<double>(goal.x - p0.x), dy = static_cast<double>(goal.y - p0.y), dist = std::hypot(dx, dy);
    if (dist < static_cast<double>(pitch)) {
      out.reached = true;
      out.width = cap;
      out.at = p0;
      out.layer = layer0;
      return out;
    }
    const double ux = dx / dist, uy = dy / dist, vx = -uy, vy = ux;
    const double len = std::min(dist, 12'000'000.0), half = 2'000'000.0;
    double ds = static_cast<double>(pitch);
    while ((len / ds + 1) * (2 * half / ds + 1) > 20'000.0) ds *= 1.5;  // a coarser grid for a long way, never above ~20k nodes per layer
    const int ni = static_cast<int>(len / ds) + 1, nj = 2 * static_cast<int>(half / ds) + 1, jc = nj / 2;
    std::vector<int> layers;
    for (int l = 0; l < nl; ++l)
      if (l == layer0 || layer_ok(net, l)) layers.push_back(l);
    const std::size_t per = static_cast<std::size_t>(ni) * static_cast<std::size_t>(nj), total = per * layers.size();
    auto pos = [&](int i, int j) {
      return Point{p0.x + static_cast<Coord>(std::llround(ux * i * ds + vx * (j - jc) * ds)), p0.y + static_cast<Coord>(std::llround(uy * i * ds + vy * (j - jc) * ds))};
    };
    constexpr Coord kUnit = 5'000;  // node values are kept in units of 5 um
    std::vector<std::int16_t> val(total, -1);  // -1: not valued yet
    long valued = 0;
    const long max_nodes = 40'000;
    auto fits = [&](Point q, int l, Coord width) {
      --calls_left;
      const int st = obs->disk_state(q, l, width / 2, net, 0, fixed_only);
      return fixed_only ? st != 2 : st == 0;
    };
    // Returns the node's value in units, or -2 when the budget is gone.
    auto value = [&](std::size_t node) -> int {
      if (val[node] >= 0) return val[node];
      if (calls_left < 16 || valued >= max_nodes) return -2;
      ++valued;
      const int li = static_cast<int>(node / per);
      const std::size_t r = node % per;
      const Point q = pos(static_cast<int>(r % static_cast<std::size_t>(ni)), static_cast<int>(r / static_cast<std::size_t>(ni)));
      const int l = layers[static_cast<std::size_t>(li)];
      Coord lo = 0;
      if (!fits(q, l, 0)) {
        val[node] = 0;
        return 0;
      }
      if (fits(q, l, cap)) {
        lo = cap;
      } else {
        Coord hi = cap;  // fits(lo), !fits(hi)
        while (hi - lo > kUnit) {
          const Coord mid = (lo + hi) / 2;
          (fits(q, l, mid) ? lo : hi) = mid;
        }
      }
      val[node] = static_cast<std::int16_t>(lo / kUnit + 1);  // +1: 0 is "no room for a hairline" but valued
      return val[node];
    };
    auto width_of = [&](int v) { return v <= 1 ? Coord{0} : static_cast<Coord>(v - 1) * kUnit; };
    const int capu = static_cast<int>(cap / kUnit) + 1;
    using QE = std::pair<int, std::size_t>;  // (bottleneck so far, node)
    std::priority_queue<QE> open;
    std::vector<std::int32_t> best(total, -1), prev(total, -1);
    const std::size_t start = static_cast<std::size_t>(std::find(layers.begin(), layers.end(), layer0) - layers.begin()) * per + static_cast<std::size_t>(jc) * static_cast<std::size_t>(ni);
    best[start] = capu;
    open.emplace(capu, start);
    std::size_t goal_node = SIZE_MAX;
    bool exhausted_budget = false;
    while (!open.empty()) {
      const auto [bn, node] = open.top();
      open.pop();
      if (bn < best[node]) continue;  // stale
      const std::size_t r = node % per;
      const int i = static_cast<int>(r % static_cast<std::size_t>(ni)), j = static_cast<int>(r / static_cast<std::size_t>(ni)), li = static_cast<int>(node / per);
      if (i == ni - 1) {
        goal_node = node;
        break;
      }
      auto relax = [&](std::size_t nb) {
        const int v = value(nb);
        if (v == -2) {
          exhausted_budget = true;
          return;
        }
        if (v == 0) return;  // not even a hairline is legal there
        const int nbv = std::min(bn, v);
        if (nbv > best[nb]) {
          best[nb] = nbv;
          prev[nb] = static_cast<std::int32_t>(node);
          open.emplace(nbv, nb);
        }
      };
      for (int di = -1; di <= 1; ++di)
        for (int dj = -1; dj <= 1; ++dj) {
          if (!di && !dj) continue;
          const int i2 = i + di, j2 = j + dj;
          if (i2 < 0 || j2 < 0 || i2 >= ni || j2 >= nj) continue;
          relax(static_cast<std::size_t>(li) * per + static_cast<std::size_t>(j2) * static_cast<std::size_t>(ni) + static_cast<std::size_t>(i2));
        }
      // A layer change here needs a legal via (the through via of the net's class).
      const Point q = pos(i, j);
      for (std::size_t l2 = 0; l2 < layers.size(); ++l2) {
        if (static_cast<int>(l2) == li) continue;
        --calls_left;
        const int vs = obs->via_state(q, via_diameter(net), via_drill(net), net, 0, fixed_only);
        if (fixed_only ? vs == 2 : vs != 0) break;  // no via here, on any layer
        relax(l2 * per + static_cast<std::size_t>(j) * static_cast<std::size_t>(ni) + static_cast<std::size_t>(i));
      }
    }
    if (goal_node == SIZE_MAX) {
      out.budget = exhausted_budget;
      out.width = 0;
      return out;
    }
    out.reached = true;
    out.width = width_of(best[goal_node]);
    // Where the best way is narrowest: the smallest node value along it (the last one if several are equal).
    int minv = INT32_MAX;
    std::size_t at_node = goal_node;
    for (std::int64_t t = static_cast<std::int64_t>(goal_node); t >= 0; t = prev[static_cast<std::size_t>(t)]) {
      const int v = val[static_cast<std::size_t>(t)] >= 0 ? val[static_cast<std::size_t>(t)] : capu;
      if (v <= minv) minv = v, at_node = static_cast<std::size_t>(t);
    }
    const std::size_t r = at_node % per;
    out.at = pos(static_cast<int>(r % static_cast<std::size_t>(ni)), static_cast<int>(r / static_cast<std::size_t>(ni)));
    out.layer = layers[at_node / per];
    return out;
  }

  // The fields a `failure` event gets for the stalled connection `ci` (issue #8, doc 13): the width and clearance of the attempt,
  // where the search got closest and how much room there was in the passage that closed, the verdict those numbers support
  // (placement, rule, contested, or unknown when the evidence is incomplete), the path that was tried, and the rule it came from.
  // Everything is measured with the router's own obstacle tests; nothing here changes costs, legality or the work budget.
  struct StallCache {
    Point closest{};
    int layer = -1, routed = -1, rips = -1;
    std::string fields;  // the measured part: at, gaps, verdict, rule (the path is rebuilt from the current probe each time)
  };
  std::map<int, StallCache> stall_cache;  // by connection: the last measurement, valid while the probe and the routed copper are as they were
  long stall_calls_left = 6'000'000;      // obstacle tests the diagnostics may spend in a run: deterministic, so recordings repeat

  std::string stall_fields(int ci, const std::string& reason) {
    const auto& c = cs[static_cast<std::size_t>(ci)].c;
    const NetId net = c.net;
    const model::NetClass& nc = netclass(net);
    const Coord W = class_width(net);
    // The narrowest track the rules allow: the board minimum, or where it sets none the 0.1 mm below which no fab is assumed.
    const Coord Mw = std::min(W, rules.minimums.track_width > 0 ? rules.minimums.track_width : Coord{100'000});
    const Coord Cmin = std::max<Coord>(rules.minimums.clearance, 0);
    const Coord C = std::max(nc.clearance, Cmin);
    std::string f = ",\"reason\":\"" + failure_reason_code(reason) + "\",\"width_nm\":" + jnum(W) + ",\"clearance_nm\":" + jnum(C) + ",\"min_width_nm\":" + jnum(Mw);
    auto mm = [](Coord v) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%.3g", static_cast<double>(v) / 1e6);
      return std::string(buf);
    };
    const std::string base_rule = "netclass " + nc.name + ": width " + mm(W) + " mm, clearance " + mm(C) + " mm";
    const bool have = stall_probe.valid && stall_probe.conn == ci && !stall_probe.edge && c.pad_b >= 0;
    if (!have) return f + ",\"verdict\":\"unknown\",\"rule\":\"" + base_rule + "\"";
    // The path that was tried, corners only (at most 64): fresh every time. Left out when the search never left the source pad.
    std::string path;
    if (stall_probe.path.size() >= 2) {
      path = ",\"rejected\":[";
      const std::size_t n = std::min<std::size_t>(stall_probe.path.size(), 64);
      for (std::size_t i = 0; i < n; ++i)
        path += (i ? "," : "") + std::string("[") + jnum(stall_probe.path[i][0]) + "," + jnum(stall_probe.path[i][1]) + "," + std::to_string(stall_probe.path[i][2]) + "]";
      path += "]";
    }
    auto& cache = stall_cache[ci];
    const int fails = cs[static_cast<std::size_t>(ci)].fails;
    const bool same = cache.layer == stall_probe.layer && cache.closest == stall_probe.closest && cache.routed == res.routed && cache.rips == res.rips;
    // Measuring costs obstacle tests: do it at a connection's 1st, 2nd, 4th, 8th ... failure, or when nothing has been measured.
    const bool due = cache.layer < 0 || (!same && (fails & (fails - 1)) == 0);
    if (!due) return f + path + cache.fields;
    const Point tp = b.pads[static_cast<std::size_t>(c.pad_b)].pos;
    long calls = std::min<long>(stall_calls_left, 600'000);
    const long before = calls;
    const Way fixed = widest_way(stall_probe.closest, stall_probe.layer, tp, net, W, true, calls);
    const Way all = widest_way(stall_probe.closest, stall_probe.layer, tp, net, W, false, calls);
    stall_calls_left -= before - calls;
    std::string g, verdict = "unknown", rule = base_rule;
    if (!fixed.budget && !all.budget) {
      const Coord gf = fixed.reached ? fixed.width : 0, ga = all.reached ? all.width : 0;
      const Way& show = (gf >= W || ga < gf) && all.reached ? all : fixed;
      const Point at = show.reached ? show.at : stall_probe.closest;
      g += ",\"at\":[" + jnum(at.x) + "," + jnum(at.y) + "],\"gap_nm\":" + jnum(ga) + ",\"gap_fixed_nm\":" + jnum(gf) + ",\"gap_layer\":" + std::to_string(show.reached ? show.layer : stall_probe.layer);
      if (gf >= W) {
        if (ga < W) {
          verdict = "contested";  // fixed copper leaves room; routed copper of other nets closes it (negotiation, not a placement edit)
          rule += "; fixed copper alone leaves " + mm(gf) + " mm";
        }  // else: the gap takes the net's own width, so something else stopped the search: unknown
      } else if (gf >= Mw) {
        verdict = "rule";  // a narrower legal track passes
        rule += "; a track of " + mm(Mw) + " mm fits (" + mm(gf) + " mm free)";
      } else if (C > Cmin && gf + 2 * (C - Cmin) >= Mw) {
        verdict = "rule";  // the board minimum clearance would pass the narrowest track
        rule += "; at the board minimum clearance " + mm(Cmin) + " mm a track of " + mm(Mw) + " mm fits";
      } else {
        verdict = "placement";
        rule += "; narrowest legal width " + mm(Mw) + " mm, " + mm(gf) + " mm free";
      }
    }
    g += ",\"verdict\":\"" + verdict + "\",\"rule\":\"" + rule + "\"";
    cache = {stall_probe.closest, stall_probe.layer, res.routed, res.rips, g};
    return f + path + g;
  }

  // Are the connection's pads already joined through fixed copper and other routed connections of its net?
  bool joined(int ci) {
    const auto& c = cs[static_cast<std::size_t>(ci)].c;
    const int ra = init_root[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(c.pad_a)])];
    const int rb = c.pad_b >= 0 ? init_root[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(c.pad_b)])] : init_root[static_cast<std::size_t>(c.zone_b)];
    return joined_roots(c.net, ra, rb);
  }
  // Are two fixed-copper groups (init_root values) of `net` joined through fixed copper and routed connections of the net?
  bool joined_roots(NetId net, int ra, int rb) {
    if (ra == rb) return true;
    const Connection c{net, -1, -1, -1, 0};
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

  // Pad-to-plane fallback (D155, --plane-informed): a pad that cannot reach its plane joins the nearest pad of its net within
  // plane_fallback_mm on the plane's side of the net's plan, and the connection becomes that pad-to-pad connection. On the PoE switch
  // copy two GND pads failed into the plane (the fill does not reach them; the search used its whole cap) while each routes to a
  // GND pad 1.17 mm away (bench/records/m22-research). Strict searches, nearest first, at most three candidates.
  bool plane_fallback(int ci) {
    auto& st = cs[static_cast<std::size_t>(ci)];
    if (st.c.zone_b < 0 || st.c.pad_a < 0 || opt.plane_fallback_mm <= 0) return false;
    const NetId net = st.c.net;
    const Point a = b.pads[static_cast<std::size_t>(st.c.pad_a)].pos;
    const int rz = init_root[static_cast<std::size_t>(st.c.zone_b)];
    const double lim = opt.plane_fallback_mm * 1e6;
    // The net's plan is a tree over its fixed-copper groups. Without this connection it falls into the pad's side and the plane's
    // side; a pad on the plane's side keeps the tree spanning when it replaces the plane as this connection's target.
    auto root_of = [&](const Connection& c, bool second) {
      if (!second) return init_root[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(c.pad_a)])];
      return c.pad_b >= 0 ? init_root[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(c.pad_b)])] : init_root[static_cast<std::size_t>(c.zone_b)];
    };
    std::set<int> plane_side{rz};
    for (bool grew = true; grew;) {
      grew = false;
      for (std::size_t k = 0; k < cs.size(); ++k) {
        if (static_cast<int>(k) == ci || cs[k].c.net != net || cs[k].c.pad_a < 0) continue;
        const int x = root_of(cs[k].c, false), y = root_of(cs[k].c, true);
        if (plane_side.count(x) != plane_side.count(y)) {
          plane_side.insert(x);
          plane_side.insert(y);
          grew = true;
        }
      }
    }
    std::vector<std::pair<double, int>> cand;
    for (std::size_t p = 0; p < b.pads.size(); ++p) {
      const int it = pad_item[p];
      if (static_cast<int>(p) == st.c.pad_a || it < 0 || obs->copper().items[static_cast<std::size_t>(it)].net != net) continue;
      const Point q = b.pads[p].pos;
      const double d = std::hypot(static_cast<double>(q.x - a.x), static_cast<double>(q.y - a.y));
      if (d > lim) continue;
      if (init_root[static_cast<std::size_t>(it)] == init_root[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(st.c.pad_a)])]) continue;
      if (!plane_side.count(init_root[static_cast<std::size_t>(it)])) continue;
      cand.emplace_back(d, static_cast<int>(p));
    }
    std::sort(cand.begin(), cand.end());
    const long keep_cap = expansion_cap;
    const bool keep_bypass = bypass_nogoods;
    auto attempt = [&](const Connection& alt) {
      expansion_cap = opt.rescue_cap;
      bypass_nogoods = true;  // a nogood of the plane search is no verdict on another target or source
      const bool ok = search_and_commit(alt, false);
      bypass_nogoods = keep_bypass;
      expansion_cap = keep_cap;
      if (std::getenv("TM_DEBUG_PLANE"))
        std::fprintf(stderr, "plane fallback %s -> %s: %s (%s)\n", pad_label(alt.pad_a).c_str(), alt.pad_b >= 0 ? pad_label(alt.pad_b).c_str() : "plane", ok ? "routed" : "failed", why.c_str());
      if (ok) {
        st.c = alt;
        ++res.plane_fallbacks;
      }
      return ok;
    };
    for (std::size_t k = 0; k < cand.size() && k < 3 && !out_of_budget(); ++k)
      if (attempt(Connection{net, st.c.pad_a, cand[k].second, -1, static_cast<Coord>(cand[k].first)})) return true;
    // The connection joins the pad's whole fixed-copper group to the plane: any other pad of the group may make that join.
    // On the PoE switch copy C115.2 shares a group with 20 GND pads tied by locked copper, and the plan chose it to drop into the plane.
    const int ra = init_root[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(st.c.pad_a)])];
    std::vector<std::pair<double, int>> mates;
    for (std::size_t p = 0; p < b.pads.size(); ++p) {
      const int it = pad_item[p];
      if (static_cast<int>(p) == st.c.pad_a || it < 0 || init_root[static_cast<std::size_t>(it)] != ra) continue;
      const Point q = b.pads[p].pos;
      mates.emplace_back(std::hypot(static_cast<double>(q.x - a.x), static_cast<double>(q.y - a.y)), static_cast<int>(p));
    }
    std::sort(mates.begin(), mates.end());
    for (std::size_t k = 0; k < mates.size() && k < static_cast<std::size_t>(opt.plane_fallback_sources) && !out_of_budget(); ++k)
      if (attempt(Connection{net, mates[k].second, -1, st.c.zone_b, 0})) return true;
    // Another fill of the net on the plane's side: the plan picks the nearest piece of the plane, which may be a sliver with no
    // room for a via (on the PoE copy C115.2 was planned into a 2.9 x 2.5 mm island beside the 19,000 mm^2 GND plane).
    std::vector<std::pair<double, int>> fills;
    const auto& cm = obs->copper();
    for (std::size_t i = 0; i < cm.items.size(); ++i) {
      const auto& it = cm.items[i];
      if (static_cast<int>(i) == st.c.zone_b || it.kind != drc::ItemKind::Zone || it.net != net || it.footprint >= 0) continue;
      if (!plane_side.count(init_root[i]) || zone_fill_area(cm, {static_cast<int>(i)}) < kPlaneTargetArea || !reaches(st.c.pad_a, static_cast<int>(i))) continue;
      fills.emplace_back(zone_dist(a, static_cast<int>(i)), static_cast<int>(i));
    }
    std::sort(fills.begin(), fills.end());
    for (std::size_t k = 0; k < fills.size() && k < 2 && !out_of_budget(); ++k) {
      if (attempt(Connection{net, st.c.pad_a, -1, fills[k].second, 0})) return true;
      for (std::size_t m = 0; m < mates.size() && m < static_cast<std::size_t>(opt.plane_fallback_sources) && !out_of_budget(); ++m)
        if (attempt(Connection{net, mates[m].second, -1, fills[k].second, 0})) return true;
    }
    return false;
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
      wc().expansions += now - charged;
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
  // True when lifted copper would pass the commit's exact check (rule 1) on the board as it now stands. A via that
  // fails as a plain via is reported as not fitting, also when it was a via in pad (conservative: it is routed again).
  bool saved_fits(const std::vector<OldItem>& saved) const {
    for (const auto& o : saved) {
      if (o.kind == drc::ItemKind::Via) {
        const auto& v = b.vias[static_cast<std::size_t>(o.index)];
        const int s = v.type == model::ViaType::Through ? obs->via_state(v.pos, v.size, v.drill, v.net, 0, false)
                                                        : obs->via_state_span(v.pos, v.size, v.drill, v.net, 0, false, nullptr, v.layer_top, v.layer_bottom, v.type);
        if (s == 2) return false;
      } else {
        const auto& t = b.tracks[static_cast<std::size_t>(o.index)];
        if (obs->segment_state(t.a, t.b, t.layer, t.width, t.net, false, nullptr) == 2) return false;
      }
    }
    return true;
  }
  struct LiftedVia {
    int owner = -1;
    model::Via via;
    std::vector<OldItem> saved;  // the via, then its legs
    std::vector<bool> leg_at_a;  // leg i's end `a` is at the via
  };
  // Lifts a routed via of connection `owner` with the tracks of that connection that end at it; nothing when other copper ends there
  // too or the via stands in a pad of its connection (it is tied in place).
  std::optional<LiftedVia> lift_via(int item) {
    const auto& vit = obs->copper().items[static_cast<std::size_t>(item)];
    LiftedVia L;
    L.owner = vit.owner;
    L.via = b.vias[static_cast<std::size_t>(vit.index)];
    auto& oc = cs[static_cast<std::size_t>(L.owner)];
    const Point P = L.via.pos;
    std::vector<int> drop{item};
    L.saved.push_back({drc::ItemKind::Via, vit.index});
    for (const int it_id : oc.items) {
      const auto& it = obs->copper().items[static_cast<std::size_t>(it_id)];
      if (it.removed || it.kind != drc::ItemKind::Track) continue;
      const auto& t = b.tracks[static_cast<std::size_t>(it.index)];
      if (t.a == P || t.b == P) {
        drop.push_back(it_id);
        L.saved.push_back({drc::ItemKind::Track, it.index});
        L.leg_at_a.push_back(t.a == P);
      }
    }
    if (drop.size() < 2) return std::nullopt;
    for (const int pad : {oc.c.pad_a, oc.c.pad_b})
      for (int l = 0; l < nl && pad >= 0; ++l)
        if (on_pad_copper(P, pad, l)) return std::nullopt;
    std::vector<int> ids;
    obs->routed_items_in(geom::Shape::point(P, 0).box.inflated(1), ids);
    for (const int id : ids) {
      const auto& it = obs->copper().items[static_cast<std::size_t>(id)];
      if (!it.removed && it.net == L.via.net && it.owner != L.owner) return std::nullopt;
    }
    unlink_items(L.owner, drop);
    return L;
  }
  void unlink_items(int owner, const std::vector<int>& items) {
    auto& oc = cs[static_cast<std::size_t>(owner)];
    for (const int item : items) {
      const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
      if (opt.sink) emit(std::string("{\"type\":\"") + (it.kind == drc::ItemKind::Via ? "via_remove" : "track_remove") + "\",\"id\":" + std::to_string(it.index) + "}");
      remove_routed(item);
      oc.items.erase(std::find(oc.items.begin(), oc.items.end(), item));
    }
  }
  // Positions for a lifted via, nearest first (rings of 16 round where it was), where it and its re-drawn legs pass the exact check
  // against everything now on the board.
  std::vector<Point> via_positions(const LiftedVia& L) {
    std::vector<Point> out;
    const Point P = L.via.pos;
    for (int ring = 1; ring <= opt.via_shove_rings; ++ring) {
      const double r = static_cast<double>(mm_to_nm(opt.via_shove_step_mm)) * ring;
      for (int k = 0; k < 16; ++k) {
        const double ang = k * std::numbers::pi / 8;
        const Point Q{P.x + static_cast<Coord>(std::llround(r * std::cos(ang))), P.y + static_cast<Coord>(std::llround(r * std::sin(ang)))};
        if (obs->via_state(Q, L.via.size, L.via.drill, L.via.net, 0, false) != 0 || obs->via_disallowed(Q, L.via.size, L.via.net, 0, nl - 1, model::ViaType::Through)) continue;
        bool legal = true;
        for (std::size_t i = 0; i + 1 < L.saved.size() && legal; ++i) {
          const auto& t = b.tracks[static_cast<std::size_t>(L.saved[i + 1].index)];
          const Point far = L.leg_at_a[i] ? t.b : t.a;
          if (far == Q || !layer_ok(t.net, t.layer) || obs->segment_state(far, Q, t.layer, t.width, t.net, false) != 0) legal = false;
        }
        if (legal) out.push_back(Q);
      }
    }
    return out;
  }
  // Puts a lifted via down at Q (checked by via_positions) with its legs re-drawn to it; returns the new items.
  std::vector<int> place_at(const LiftedVia& L, Point Q) {
    std::vector<int> placed;
    model::Via nv = L.via;
    nv.pos = Q;
    b.vias.push_back(nv);
    placed.push_back(obs->add_via(static_cast<int>(b.vias.size() - 1), L.owner));
    emit_via_add(static_cast<int>(b.vias.size() - 1));
    for (std::size_t i = 0; i + 1 < L.saved.size(); ++i) {
      model::Track t = b.tracks[static_cast<std::size_t>(L.saved[i + 1].index)];
      (L.leg_at_a[i] ? t.a : t.b) = Q;
      b.tracks.push_back(t);
      placed.push_back(obs->add_track(static_cast<int>(b.tracks.size() - 1), L.owner));
      emit_track_add(static_cast<int>(b.tracks.size() - 1));
    }
    for (const int id : placed) {
      near_mark(id, +1);
      cs[static_cast<std::size_t>(L.owner)].items.push_back(id);
    }
    return placed;
  }
  // Via shove (D156, doc 05 §64): a pin boxed in by other nets' vias is often freed by moving one or two of them a few tenths of a mm, which
  // the rip-up cannot do: it routes the other connection again from scratch, and on a full board that one then fails instead (the pin
  // rescue's measured failure, doc 05 §63). Here only a via and the tracks ending at it move; the rest of its connection stays. The vias
  // of other nets within via_shove_mm of the stuck connection's pads, nearest first, are lifted one at a time, then in pairs, and the stuck
  // connection is searched without them (strictly, capped). When it routes, each lifted via goes down at the nearest position where it
  // and its re-drawn legs pass the exact check beside the new route; when one finds none, the new route is lifted and everything is put
  // back as it was. A push-and-shove router's via move (KiCad's PNS), as a repair step of the batch router.
  long shove_work = 0, shove_start = 0;  // expansions the via shove has spent in this run; res.expansions when the current shove began
  bool shove_spent() const {
    return opt.work_budget > 0 && static_cast<double>(shove_work + res.expansions - shove_start) >= opt.via_shove_share * static_cast<double>(opt.work_budget);
  }
  bool via_shove(int ci) {
    const auto& st = cs[static_cast<std::size_t>(ci)];
    if (st.c.pad_a < 0) return false;
    // Its searches come out of the run's budget: past via_shove_share of it the shove stops (on kitspace_d20 an unbounded shove routed 10
    // connections and lost 41 to the budget it took from the main loop).
    shove_start = res.expansions;
    if (shove_spent()) return false;
    const bool done = via_shove_try(ci);
    shove_work += res.expansions - shove_start;
    return done;
  }
  bool via_shove_try(int ci) {
    const auto& st = cs[static_cast<std::size_t>(ci)];
    const bool dbg = std::getenv("TM_DEBUG_SHOVE") != nullptr;
    struct Cand { std::int64_t d2; int item; };
    std::vector<Cand> cands;
    std::vector<int> ids;
    const Coord reach = mm_to_nm(opt.via_shove_mm);
    for (const int pad : {st.c.pad_a, st.c.pad_b}) {
      if (pad < 0) continue;
      const Point pp = b.pads[static_cast<std::size_t>(pad)].pos;
      ids.clear();
      obs->routed_items_in(obs->copper().items[static_cast<std::size_t>(pad_item[static_cast<std::size_t>(pad)])].box.inflated(reach), ids);
      for (const int id : ids) {
        const auto& it = obs->copper().items[static_cast<std::size_t>(id)];
        if (it.removed || it.kind != drc::ItemKind::Via || it.net == st.c.net || it.owner < 0 || it.bare_of >= 0) continue;
        const auto& oc = cs[static_cast<std::size_t>(it.owner)];
        if (!oc.routed || oc.bundle_lane || oc.c.zone_b >= 0 || b.vias[static_cast<std::size_t>(it.index)].type != model::ViaType::Through) continue;
        const Point vp = b.vias[static_cast<std::size_t>(it.index)].pos;
        const std::int64_t dx = vp.x - pp.x, dy = vp.y - pp.y;
        cands.push_back({dx * dx + dy * dy, id});
      }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.d2 != y.d2 ? x.d2 < y.d2 : x.item < y.item; });
    // Vias by (owner, position): a via that is lifted and put back is a new copper item.
    std::vector<std::pair<int, Point>> vias;
    for (const auto& cd : cands) {
      const auto& it = obs->copper().items[static_cast<std::size_t>(cd.item)];
      const std::pair<int, Point> key{it.owner, b.vias[static_cast<std::size_t>(it.index)].pos};
      if (std::find(vias.begin(), vias.end(), key) == vias.end()) vias.push_back(key);
    }
    if (static_cast<int>(vias.size()) > opt.via_shove_vias) vias.resize(static_cast<std::size_t>(opt.via_shove_vias));
    auto via_item = [&](const std::pair<int, Point>& key) {
      for (const int item : cs[static_cast<std::size_t>(key.first)].items) {
        const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
        if (!it.removed && it.kind == drc::ItemKind::Via && b.vias[static_cast<std::size_t>(it.index)].pos == key.second) return item;
      }
      return -1;
    };
    std::vector<std::vector<std::pair<int, Point>>> groups;
    for (const auto& v : vias) groups.push_back({v});
    for (std::size_t i = 0; i < vias.size() && i < 4; ++i)
      for (std::size_t j = i + 1; j < vias.size() && j < 4; ++j) groups.push_back({vias[i], vias[j]});
    int trials = 0;
    const bool saved_force = force_escapes;
    for (const auto& g : groups) {
      if (trials >= opt.via_shove_trials || out_of_budget() || shove_spent()) break;
      std::vector<LiftedVia> lifted;
      bool ok_lift = true;
      for (const auto& key : g) {
        const int item = via_item(key);
        if (item < 0) { ok_lift = false; break; }
        auto L = lift_via(item);
        if (!L) { ok_lift = false; break; }
        lifted.push_back(std::move(*L));
      }
      auto put_back = [&] {
        for (auto it = lifted.rbegin(); it != lifted.rend(); ++it) restore_items(it->owner, it->saved);
      };
      if (!ok_lift) {
        put_back();
        continue;
      }
      auto attempt = [&] {
        ++trials;
        ++res.via_shove_trials;
        force_escapes = true;
        expansion_cap = opt.via_shove_cap;
        const bool ok = search_and_commit(st.c, false);
        expansion_cap = 0;
        force_escapes = saved_force;
        return ok;
      };
      auto note = [&](const char* what) {
        if (!dbg) return;
        std::fprintf(stderr, "shove for conn %d (%s): lift", ci, b.nets[static_cast<std::size_t>(st.c.net)].name.c_str());
        for (const auto& L : lifted) std::fprintf(stderr, " %s@(%.3f, %.3f)", b.nets[static_cast<std::size_t>(L.via.net)].name.c_str(), nm_to_mm(L.via.pos.x), nm_to_mm(L.via.pos.y));
        std::fprintf(stderr, ": %s\n", what);
      };
      // Without the lifted vias, does the connection route at all? If not, they are not what blocks it.
      if (!attempt()) {
        note(why.c_str());
        put_back();
        continue;
      }
      // It does. First the vias go down beside the new route, if there is room.
      std::vector<std::pair<int, std::vector<int>>> placed;
      bool placed_all = true;
      for (std::size_t i = 0; i < lifted.size() && placed_all; ++i) {
        const auto pos = via_positions(lifted[i]);
        if (pos.empty()) placed_all = false;
        else placed.emplace_back(lifted[i].owner, place_at(lifted[i], pos.front()));
      }
      if (placed_all) {
        note("routed, vias moved beside it");
        ++res.via_shoves;
        return true;
      }
      for (auto it = placed.rbegin(); it != placed.rend(); ++it) unlink_items(it->first, it->second);
      lift_items(ci);
      // No room beside that route: a single via goes down at each of its positions in turn and the connection is searched round it.
      if (lifted.size() == 1) {
        const auto pos = via_positions(lifted[0]);
        for (const Point Q : pos) {
          if (trials >= opt.via_shove_trials || out_of_budget() || shove_spent()) break;
          auto p = place_at(lifted[0], Q);
          if (attempt()) {
            note("routed round the moved via");
            ++res.via_shoves;
            return true;
          }
          unlink_items(lifted[0].owner, p);
        }
      }
      note("blocker, but no position for it lets the connection through");
      put_back();
    }
    return false;
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
            unrelease_escapes(ca);
            unrelease_escapes(cb);
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

  // ---------------------------------------------------------------------------------------------------
  // Buses (doc 05 §38): the connections of a group between the same two components form a bundle and are routed
  // lane by lane in the order of their pins. The first lane routed leads; every later one is searched inside a
  // guide around the nearest lane already routed (same layers along its tracks, any layer around its vias and the
  // lane's own pads), so it changes layer where its neighbour did. A lane that cannot be routed that way is routed
  // like any other connection, with the guide as a preference only, and counts as a lane if it still came out on
  // the neighbour's layers with the same vias; otherwise it has left the bus.
  // ---------------------------------------------------------------------------------------------------
  std::vector<int> net_bus;  // net -> index in opt.buses, -1 none; empty when there are no buses
  bool bus_net(NetId n) const { return static_cast<std::size_t>(n) < net_bus.size() && net_bus[static_cast<std::size_t>(n)] >= 0; }
  struct Bundle {
    int bus = -1;
    std::vector<int> conns;  // lane order
  };
  std::vector<Bundle> bundles;
  std::vector<int> conn_bundle, conn_lane;   // per connection; -1 outside every bundle. Empty when there are no bundles
  // Layer assignment (doc 16 F2; Ozdal and Wong, TCAD 2006, layer-by-layer assignment of crossing nets): the
  // straight chords between a connection's pads that cross are the escapes that must swap sides, and on one layer
  // they cannot. Each connection gets the layer that leaves the fewest crossings on its layer (a greedy start and
  // single flips until none improves, deterministic); tracks off that layer cost lp_pen per lattice step.
  std::int64_t soft_unit = 0;  // cost of crossing another net's copper: opt.soft_cost_mm, growing with the pass (PathFinder's present-sharing factor)
  struct PkgBox { geom::Box box; int layer; int footprint; };
  std::vector<PkgBox> pkg_boxes;
  double auto_dir_pen = 0;  // opt.layer_dir_pen resolved for this board
  bool pkg_on = false;
  std::int64_t pkg_pen = 0;
  // Footprints with at least 30 SMD pads on one layer: the box of their pads, a pad pitch wider on every side.
  void package_boxes() {
    pkg_boxes.clear();
    pkg_on = false;
    auto_dir_pen = std::max(0.0, opt.layer_dir_pen);
    double pen = std::max(0.0, opt.package_pen);
    std::map<int, std::vector<int>> by;
    for (std::size_t i = 0; i < b.pads.size(); ++i)
      if (b.pads[i].footprint >= 0 && b.pads[i].type == model::PadType::Smd) by[b.pads[i].footprint].push_back(static_cast<int>(i));
    for (const auto& [fp, ids] : by) {
      if (ids.size() < 30) continue;
      const model::LayerMask m = b.pads[static_cast<std::size_t>(ids[0])].copper;
      geom::Box bx;
      Coord half = 0;
      bool same = true;
      for (int i : ids) {
        const auto& pd = b.pads[static_cast<std::size_t>(i)];
        same = same && pd.copper == m;
        bx.add(pd.pos);
        half = std::max(half, std::max(pd.size_x, pd.size_y));
      }
      if (!same || m == 0) continue;
      // A ball-grid-like array: at least 5 distinct columns and rows, at least 40 % of the grid cells filled (a connector
      // or a QFP has the pad count but not the filled grid).
      std::set<Coord> gx, gy;
      for (int i : ids) {
        gx.insert(b.pads[static_cast<std::size_t>(i)].pos.x / 10'000);
        gy.insert(b.pads[static_cast<std::size_t>(i)].pos.y / 10'000);
      }
      if (gx.size() < 5 || gy.size() < 5 || 10 * ids.size() < 4 * gx.size() * gy.size()) continue;
      pkg_boxes.push_back({bx.inflated(half), std::countr_zero(m), fp});
    }
    // Automatic settings (doc 16 §10, D98): on a two-layer board with a ball-grid package the layers get preferred
    // directions and the package interior is dear on its pad layer; srj19/srj20 gap boards, 30 M work: +7 and +4 clean.
    // A breakout board: at least 70 % of the connections have an end on a ball-grid package (srj19/srj20; on ordinary
    // two-layer boards the same settings cost 1-3 points of completion: quick 89.2 -> 86.8 %, mid 53.5 -> 51.1 %).
    bool breakout = false;
    if (nl == 2 && !pkg_boxes.empty() && !cs.empty()) {
      std::size_t touching = 0;
      for (const auto& st : cs) {
        const int fa = st.c.pad_a >= 0 ? b.pads[static_cast<std::size_t>(st.c.pad_a)].footprint : -1;
        const int fb = st.c.pad_b >= 0 ? b.pads[static_cast<std::size_t>(st.c.pad_b)].footprint : -1;
        for (const auto& bx : pkg_boxes)
          if (bx.footprint == fa || bx.footprint == fb) {
            ++touching;
            break;
          }
      }
      breakout = 10 * touching >= 7 * cs.size();
    }
    if (breakout) {
      if (opt.layer_dir_pen < 0) auto_dir_pen = opt.layer_dir_pen == -1.0 ? 0.3 : -opt.layer_dir_pen;
      if (opt.package_pen < 0) pen = 3.0;
      // Search profile of M15 (doc 16 §12, D99): negotiation as transactions with annealing acceptance, reachability floods before
      // every strict search and counted at a quarter of an expansion (they cost far less time), fields cached. Options the caller
      // set explicitly (anything but the defaults) are left alone.
      if (opt.txn < 0) opt.txn = 1;
      if (opt.txn_temp < 0) opt.txn_temp = 1.5;
      if (opt.breakout_profile) {
        if (opt.reach_check == 1) opt.reach_check = 2;
        if (opt.flood_work_pct == 100) opt.flood_work_pct = 25;
        if (opt.field_prefill == 0) opt.field_prefill = 1;
        if (opt.field_cache_mb == 0) opt.field_cache_mb = 512;
      }
    } else {
      if (opt.txn < 0) opt.txn = 0;
      if (opt.txn_temp < 0) opt.txn_temp = 0;
    }
    pkg_pen = static_cast<std::int64_t>(pen * static_cast<double>(pitch));
    pkg_on = !pkg_boxes.empty() && pen > 0;
  }
  std::vector<int> conn_lpref;
  bool lp_on = false;
  int lp_layer = 0;
  std::int64_t lp_pen = 0;
  void layer_assign() {
    conn_lpref.assign(cs.size(), -1);
    const int k = std::min(nl, 2);  // two outer layers; inner layers are planes or left to the search
    if (!(opt.layer_assign || opt.layer_assign_angle) || nl < 2) return;
    const std::size_t n = cs.size();
    struct Chord { Point a, e; };
    std::vector<Chord> ch(n);
    std::vector<char> use(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
      const auto& c = cs[i].c;
      if (c.pad_b < 0) continue;
      ch[i] = {b.pads[static_cast<std::size_t>(c.pad_a)].pos, b.pads[static_cast<std::size_t>(c.pad_b)].pos};
      use[i] = 1;
    }
    auto ccw = [](Point a, Point bq, Point c) {
      return static_cast<long double>(c.y - a.y) * static_cast<long double>(bq.x - a.x) - static_cast<long double>(bq.y - a.y) * static_cast<long double>(c.x - a.x);
    };
    if (opt.layer_assign_angle) {  // by direction: mostly horizontal chords prefer the horizontal layer, the others the vertical one
      for (std::size_t i = 0; i < n; ++i)
        if (use[i]) conn_lpref[i] = std::llabs(ch[i].e.x - ch[i].a.x) >= std::llabs(ch[i].e.y - ch[i].a.y) ? 0 : nl - 1;
      return;
    }
    std::vector<std::vector<int>> adj(n);
    for (std::size_t i = 0; i < n; ++i)
      for (std::size_t j = i + 1; j < n; ++j) {
        if (!use[i] || !use[j] || cs[i].c.net == cs[j].c.net) continue;
        const auto &p = ch[i], &q = ch[j];
        if (ccw(p.a, p.e, q.a) * ccw(p.a, p.e, q.e) < 0 && ccw(q.a, q.e, p.a) * ccw(q.a, q.e, p.e) < 0) {
          adj[i].push_back(static_cast<int>(j));
          adj[j].push_back(static_cast<int>(i));
        }
      }
    std::vector<int> lay(n, -1), order;
    for (std::size_t i = 0; i < n; ++i)
      if (use[i]) order.push_back(static_cast<int>(i));
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return adj[static_cast<std::size_t>(x)].size() > adj[static_cast<std::size_t>(y)].size(); });
    auto conflicts = [&](int i, int l) {
      int c = 0;
      for (int j : adj[static_cast<std::size_t>(i)]) c += lay[static_cast<std::size_t>(j)] == l;
      return c;
    };
    for (int i : order) {
      int best = 0, bc = 1 << 30;
      for (int l = 0; l < k; ++l)
        if (const int c = conflicts(i, l); c < bc) bc = c, best = l;
      lay[static_cast<std::size_t>(i)] = best;
    }
    for (int round = 0; round < 30; ++round) {
      bool moved = false;
      for (int i : order) {
        int best = lay[static_cast<std::size_t>(i)], bc = conflicts(i, best);
        for (int l = 0; l < k; ++l)
          if (const int c = conflicts(i, l); c < bc) bc = c, best = l;
        if (best != lay[static_cast<std::size_t>(i)]) {
          lay[static_cast<std::size_t>(i)] = best;
          moved = true;
        }
      }
      if (!moved) break;
    }
    long same = 0, all = 0;
    for (std::size_t i = 0; i < n; ++i)
      for (int j : adj[i])
        if (j > static_cast<int>(i)) {
          ++all;
          same += lay[i] == lay[static_cast<std::size_t>(j)];
        }
    for (std::size_t i = 0; i < n; ++i) conn_lpref[i] = lay[i] < 0 ? -1 : (lay[i] == 0 ? 0 : nl - 1);
    if (std::getenv("TM_DEBUG_LAYERS")) std::fprintf(stderr, "layer assignment: %ld crossing pairs, %ld on one layer after the assignment\n", all, same);
  }
  // Path guidance (RouterOptions::guides): per connection the guide matching its pads, or -1; cells within
  // guide_radius of the current connection's guide are set in gd_mask.
  std::vector<int> conn_guide;
  std::vector<std::uint8_t> gd_mask;
  std::vector<std::size_t> gd_cells;
  bool gd_on = false, gd_hard = false;  // gd_hard: cells off the guide are blocked
  std::int64_t gd_pen = 0;
  geom::Box gd_box;
  void guide_clear() {
    for (std::size_t i : gd_cells) gd_mask[i] = 0;
    gd_cells.clear();
    gd_on = gd_hard = false;
    gd_box = geom::Box{};
  }
  void guide_mark(Point a, Point e, Coord r, model::LayerMask mask) {
    geom::Box sb;
    sb.add(a);
    sb.add(e);
    sb = sb.inflated(r);
    gd_box.add(Point{sb.x0, sb.y0});
    gd_box.add(Point{sb.x1, sb.y1});
    const int x0 = std::clamp(to_ix(sb.x0), 0, nx - 1), x1 = std::clamp(to_ix(sb.x1), 0, nx - 1);
    const int y0 = std::clamp(to_iy(sb.y0), 0, ny - 1), y1 = std::clamp(to_iy(sb.y1), 0, ny - 1);
    for (int gy = y0; gy <= y1; ++gy)
      for (int gx = x0; gx <= x1; ++gx) {
        if (geom::point_seg_dist(at(gx, gy), a, e) > static_cast<long double>(r)) continue;
        for (int l = 0; l < nl; ++l) {
          if (!(mask & model::layer_bit(l))) continue;
          const std::size_t i = lat_index(l, gx, gy);
          if (gd_mask[i]) continue;
          gd_mask[i] = 1;
          gd_cells.push_back(i);
        }
      }
  }
  void guide_set(int gi) {
    guide_clear();
    if (gd_mask.empty()) gd_mask.assign(static_cast<std::size_t>(nl) * static_cast<std::size_t>(ny) * static_cast<std::size_t>(nx), 0);
    const auto& g = opt.guides[static_cast<std::size_t>(gi)];
    const Coord r = static_cast<Coord>(opt.guide_radius * static_cast<double>(pitch));
    const model::LayerMask all = nl >= 64 ? ~model::LayerMask{0} : (model::LayerMask{1} << nl) - 1;
    for (std::size_t k = 0; k < g.segs.size(); ++k) guide_mark(g.segs[k][0], g.segs[k][1], r, model::layer_bit(std::clamp(g.seg_layer[k], 0, nl - 1)));
    for (Point v : g.vias) guide_mark(v, v, r, all);
    // The pads themselves: a guide that starts a pitch off the pad centre still reaches it.
    guide_mark(g.a, g.a, 2 * r, all);
    guide_mark(g.b, g.b, 2 * r, all);
    gd_pen = static_cast<std::int64_t>(opt.guide_pen * static_cast<double>(pitch));
    gd_on = true;
  }
  void guide_match() {
    conn_guide.assign(cs.size(), -1);
    if (opt.guides.empty()) return;
    const Coord tol = 150'000;
    auto near = [&](Point u, Point v) { return std::llabs(u.x - v.x) <= tol && std::llabs(u.y - v.y) <= tol; };
    for (std::size_t i = 0; i < cs.size(); ++i) {
      const auto& c = cs[i].c;
      if (c.pad_b < 0) continue;
      const Point pa = b.pads[static_cast<std::size_t>(c.pad_a)].pos, pb = b.pads[static_cast<std::size_t>(c.pad_b)].pos;
      for (std::size_t g = 0; g < opt.guides.size(); ++g) {
        const auto& gp = opt.guides[g];
        if ((near(pa, gp.a) && near(pb, gp.b)) || (near(pa, gp.b) && near(pb, gp.a))) {
          conn_guide[i] = static_cast<int>(g);
          break;
        }
      }
    }
    if (std::getenv("TM_DEBUG_GUIDE")) {
      long m = 0;
      for (int g : conn_guide) m += g >= 0;
      std::fprintf(stderr, "guides: %zu given, %ld of %zu connections matched\n", opt.guides.size(), m, cs.size());
    }
  }
  std::vector<std::uint8_t> bus_guide;       // per lattice cell (lat_index): inside the current guide
  std::vector<std::size_t> bus_guide_cells;  // the cells set, to clear them again
  int bus_mode = 0;                          // 0 no guide; 1 cells off the guide cost bus_pen per step; 2 they are blocked
  std::int64_t bus_pen = 0;
  bool tune_bus = false;                     // tune_net is tuning a bus member
  ViaHeights bus_vh;                         // what a via adds to a member's length in KiCad's skew check
  drc::ViaHeights len_vh;                    // the same for delay tuning (kicad_measure), computed when first needed

  // Finds the bundles and makes each one's connections consecutive, lane by lane, at the place the router's order
  // gave the first of them. Bundles go to the front only with opt.bus_first: on a dense board the lanes out of a
  // ball grid are the hardest connections, and trying them all first spends the budget before the easy ones are
  // routed (doc 05 §38). A bundle needs two connections of different nets of one group joining the same two
  // components; its lanes are ordered along the pad row at the component with the lower index (the wider extent
  // of x and y), so neighbouring lanes are neighbouring pins.
  void order_buses(std::vector<Connection>& out) {
    net_bus.assign(b.nets.size(), -1);
    for (std::size_t g = 0; g < opt.buses.size(); ++g)
      for (NetId n : opt.buses[g].nets)
        if (n > 0 && static_cast<std::size_t>(n) < net_bus.size() && net_bus[static_cast<std::size_t>(n)] < 0) net_bus[static_cast<std::size_t>(n)] = static_cast<int>(g);
    std::map<std::array<int, 3>, std::vector<std::size_t>> by;  // (bus, component, component) -> connections
    for (std::size_t i = 0; i < out.size(); ++i) {
      auto& c = out[i];
      if (!bus_net(c.net) || c.pad_b < 0) continue;
      int fa = b.pads[static_cast<std::size_t>(c.pad_a)].footprint, fb = b.pads[static_cast<std::size_t>(c.pad_b)].footprint;
      if (fa < 0 || fb < 0 || fa == fb) continue;
      if (fa > fb) {  // every lane of a bundle runs the same way
        std::swap(c.pad_a, c.pad_b);
        std::swap(fa, fb);
      }
      by[{net_bus[static_cast<std::size_t>(c.net)], fa, fb}].push_back(i);
    }
    std::vector<int> bundle_of(out.size(), -1), lane_of(out.size(), -1);
    std::vector<std::vector<std::size_t>> members;  // per bundle, lane order
    for (auto& [key, list] : by) {
      std::set<NetId> nets;
      for (std::size_t i : list) nets.insert(out[i].net);
      if (nets.size() < 2) continue;
      geom::Box box;
      for (std::size_t i : list) box.add(b.pads[static_cast<std::size_t>(out[i].pad_a)].pos);
      const bool along_x = box.x1 - box.x0 >= box.y1 - box.y0;
      auto key_of = [&](std::size_t i) {
        const Point pa = b.pads[static_cast<std::size_t>(out[i].pad_a)].pos, pb = b.pads[static_cast<std::size_t>(out[i].pad_b)].pos;
        return std::array<Coord, 4>{along_x ? pa.x : pa.y, along_x ? pa.y : pa.x, along_x ? pb.x : pb.y, static_cast<Coord>(i)};
      };
      std::sort(list.begin(), list.end(), [&](std::size_t x, std::size_t y) { return key_of(x) < key_of(y); });
      for (std::size_t k = 0; k < list.size(); ++k) {
        bundle_of[list[k]] = static_cast<int>(bundles.size());
        lane_of[list[k]] = static_cast<int>(k);
      }
      members.push_back(list);
      bundles.push_back({key[0], {}});
    }
    if (bundles.empty()) return;
    std::vector<std::size_t> order;
    std::vector<char> placed(bundles.size(), 0);
    for (std::size_t i = 0; i < out.size(); ++i) {
      if (bundle_of[i] < 0) {
        order.push_back(i);
      } else if (!placed[static_cast<std::size_t>(bundle_of[i])]) {
        placed[static_cast<std::size_t>(bundle_of[i])] = 1;
        const auto& m = members[static_cast<std::size_t>(bundle_of[i])];
        order.insert(order.end(), m.begin(), m.end());
      }
    }
    if (opt.bus_first) std::stable_partition(order.begin(), order.end(), [&](std::size_t i) { return bundle_of[i] >= 0; });
    // --first-nets keep their place in front.
    if (crit_order_on) std::stable_sort(order.begin(), order.end(), [&](std::size_t i, std::size_t j) { return order_key(out[i].net) < order_key(out[j].net); });
    else if (!first_net_ids.empty()) {
      const auto mid = std::stable_partition(order.begin(), order.end(), [&](std::size_t i) { return first_net_ids.count(out[i].net) > 0; });
      if (opt.first_nets_ordered) std::stable_sort(order.begin(), mid, [&](std::size_t i, std::size_t j) { return first_pos(out[i].net) < first_pos(out[j].net); });
    }
    std::vector<Connection> sorted;
    conn_bundle.assign(out.size(), -1);
    conn_lane.assign(out.size(), -1);
    for (std::size_t k = 0; k < order.size(); ++k) {
      sorted.push_back(out[order[k]]);
      conn_bundle[k] = bundle_of[order[k]];
      conn_lane[k] = lane_of[order[k]];
    }
    out = std::move(sorted);
    for (auto& bd : bundles) bd.conns.clear();
    std::vector<std::pair<int, int>> lanes;  // (lane, connection) per bundle
    for (std::size_t bi = 0; bi < bundles.size(); ++bi) {
      lanes.clear();
      for (std::size_t k = 0; k < out.size(); ++k)
        if (conn_bundle[k] == static_cast<int>(bi)) lanes.emplace_back(conn_lane[k], static_cast<int>(k));
      std::sort(lanes.begin(), lanes.end());
      for (const auto& [l, k] : lanes) bundles[bi].conns.push_back(k);
    }
    bus_pen = pitch / 4;
    bus_vh = via_heights(b, rules);
    if (opt.sink)
      for (const auto& bd : bundles) {
        std::string nets;
        for (int k : bd.conns) nets += (nets.empty() ? "" : ",") + std::to_string(out[static_cast<std::size_t>(k)].net);
        emit("{\"type\":\"bus\",\"name\":\"" + json_text(opt.buses[static_cast<std::size_t>(bd.bus)].name) + "\",\"nets\":[" + nets + "]}");
      }
  }
  // A name as a JSON string body.
  static std::string json_text(const std::string& in) {
    std::string out;
    for (char ch : in) {
      if (ch == '"' || ch == '\\') out += '\\';
      if (static_cast<unsigned char>(ch) >= 0x20) out += ch;
    }
    return out;
  }

  // The lane a connection follows: the nearest lane of its bundle that is routed as a lane (the lower one of two
  // equally near), or -1 when there is none and the connection leads.
  int bus_reference(int ci) const {
    const auto& bd = bundles[static_cast<std::size_t>(conn_bundle[static_cast<std::size_t>(ci)])];
    const int lane = conn_lane[static_cast<std::size_t>(ci)];
    int best = -1, best_d = 0;
    for (int k = 0; k < static_cast<int>(bd.conns.size()); ++k) {
      const auto& o = cs[static_cast<std::size_t>(bd.conns[static_cast<std::size_t>(k)])];
      if (k == lane || !o.routed || !o.bus_lane || o.items.empty()) continue;
      if (best < 0 || std::abs(k - lane) < best_d) best = bd.conns[static_cast<std::size_t>(k)], best_d = std::abs(k - lane);
    }
    return best;
  }
  // Layers of a connection's tracks in the order they were laid, and its vias.
  std::vector<int> bus_layers(int ci, int& vias) const {
    std::vector<int> seq;
    vias = 0;
    for (int item : cs[static_cast<std::size_t>(ci)].items) {
      const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
      if (it.kind == drc::ItemKind::Via) {
        ++vias;
      } else if (it.kind == drc::ItemKind::Track) {
        const int l = b.tracks[static_cast<std::size_t>(it.index)].layer;
        if (seq.empty() || seq.back() != l) seq.push_back(l);
      }
    }
    return seq;
  }
  // The two connections have the same layers in order (either way round: a search may run from either end) and as
  // many vias.
  bool bus_same_layers(int ci, int ref) const {
    int v = 0, rv = 0;
    const auto seq = bus_layers(ci, v);
    const auto rseq = bus_layers(ref, rv);
    return v == rv && (seq == rseq || std::equal(seq.begin(), seq.end(), rseq.rbegin(), rseq.rend()));
  }
  void bus_clear_guide() {
    for (std::size_t i : bus_guide_cells) bus_guide[i] = 0;
    bus_guide_cells.clear();
    bus_mode = 0;
  }
  // Marks the lattice cells within `r` of segment a-e on the layers of `mask`; grows `box`.
  void bus_mark(Point a, Point e, Coord r, model::LayerMask mask, geom::Box& box) {
    geom::Box sb;
    sb.add(a);
    sb.add(e);
    sb = sb.inflated(r);
    box.add(Point{sb.x0, sb.y0});
    box.add(Point{sb.x1, sb.y1});
    const int x0 = std::clamp(to_ix(sb.x0), 0, nx - 1), x1 = std::clamp(to_ix(sb.x1), 0, nx - 1);
    const int y0 = std::clamp(to_iy(sb.y0), 0, ny - 1), y1 = std::clamp(to_iy(sb.y1), 0, ny - 1);
    for (int gy = y0; gy <= y1; ++gy)
      for (int gx = x0; gx <= x1; ++gx) {
        if (geom::point_seg_dist(at(gx, gy), a, e) > static_cast<long double>(r)) continue;
        for (int l = 0; l < nl; ++l) {
          if (!(mask & model::layer_bit(l))) continue;
          const std::size_t i = lat_index(l, gx, gy);
          if (bus_guide[i]) continue;
          bus_guide[i] = 1;
          bus_guide_cells.push_back(i);
        }
      }
  }
  // Builds the guide of connection `ci` around lane `ref`; returns the box the guide covers.
  geom::Box bus_set_guide(int ci, int ref) {
    bus_clear_guide();
    if (bus_guide.empty()) bus_guide.assign(static_cast<std::size_t>(nl) * static_cast<std::size_t>(ny) * static_cast<std::size_t>(nx), 0);
    const auto& c = cs[static_cast<std::size_t>(ci)].c;
    const auto& rc = cs[static_cast<std::size_t>(ref)].c;
    auto pos = [&](int pad) { return b.pads[static_cast<std::size_t>(pad)].pos; };
    auto dist = [](Point u, Point v) { return static_cast<Coord>(std::hypot(static_cast<double>(u.x - v.x), static_cast<double>(u.y - v.y))); };
    const Coord da = dist(pos(c.pad_a), pos(rc.pad_a)), db = dist(pos(c.pad_b), pos(rc.pad_b));
    // Room for the lane beside its neighbour: the pads' own distance (the lanes run that far apart where nothing
    // squeezes them), two more lanes and a via.
    const Coord room = 2 * (class_width(c.net) + std::max(netclass(c.net).clearance, rules.minimums.clearance)) + class_via_diameter(c.net) + pitch;
    const Coord r = std::min<Coord>(std::max(da, db), 5'000'000) + room;
    const model::LayerMask all = nl >= 64 ? ~model::LayerMask{0} : (model::LayerMask{1} << nl) - 1;
    geom::Box box;
    for (int item : cs[static_cast<std::size_t>(ref)].items) {
      const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
      if (it.kind == drc::ItemKind::Via) {
        bus_mark(it.pos, it.pos, r, all, box);
      } else if (it.kind == drc::ItemKind::Track) {
        const auto& t = b.tracks[static_cast<std::size_t>(it.index)];
        bus_mark(t.a, t.b, r, model::layer_bit(t.layer), box);
      }
    }
    bus_mark(pos(c.pad_a), pos(c.pad_a), da + room, all, box);
    bus_mark(pos(c.pad_b), pos(c.pad_b), db + room, all, box);
    return box;
  }
  // Routes lane `ci` inside the guide of its reference lane. False when it leads (no reference) or no path inside the
  // guide passes the exact check; the guide then stays as a preference (bus_mode 1) for the searches that follow.
  bool route_bus_lane(int ci) {
    const int ref = bus_reference(ci);
    if (ref < 0) return false;
    const geom::Box box = bus_set_guide(ci, ref);
    const auto& c = cs[static_cast<std::size_t>(ci)].c;
    Window w;
    w.x0 = std::clamp(to_ix(box.x0) - 1, 0, nx - 1);
    w.y0 = std::clamp(to_iy(box.y0) - 1, 0, ny - 1);
    w.w = std::clamp(to_ix(box.x1) + 1, 0, nx - 1) - w.x0 + 1;
    w.h = std::clamp(to_iy(box.y1) + 1, 0, ny - 1) - w.y0 + 1;
    soft = false;
    bus_mode = 2;
    std::vector<PathNode> path;
    bool done = false;
    // A lane beside its neighbour is found quickly or not at all: the search is cut off at a few expansions per
    // cell of the guide, so a guide with no way through costs little. A path the exact check rejects teaches the
    // search its blocked cells; the next attempt goes round them.
    for (int attempt = 0; attempt < 3 && !done && !out_of_budget(); ++attempt) {
      expansion_cap = std::max<long>(50'000, 4 * static_cast<long>(bus_guide_cells.size()) / std::max(1, nl));
      const bool found = search(c, w, path);
      expansion_cap = 0;
      if (!found) break;
      done = commit(c, path);
    }
    last_miss = Miss::None;  // an exhausted guide says nothing about the pin being boxed in
    bus_mode = 1;
    // Neighbouring lanes are about as long as each other, on the same layers. A lane that had to wind through the
    // guide or hop over something to stay in it (an obstacle the neighbour passed on the other side) is taken back:
    // the usual search may pass the obstacle on the lane's own side, and the lanes after it then follow that.
    if (done) {
      const auto& rc = cs[static_cast<std::size_t>(ref)].c;
      auto pos = [&](int pad) { return b.pads[static_cast<std::size_t>(pad)].pos; };
      auto dist = [](Point u, Point v) { return std::hypot(static_cast<double>(u.x - v.x), static_cast<double>(u.y - v.y)); };
      // Detour: length beyond the shortest octilinear line between the pads.
      auto detour = [&](int k) {
        const auto& kc = cs[static_cast<std::size_t>(k)].c;
        const double dx = std::fabs(static_cast<double>(pos(kc.pad_a).x - pos(kc.pad_b).x)), dy = std::fabs(static_cast<double>(pos(kc.pad_a).y - pos(kc.pad_b).y));
        return net_length({k}) - (std::max(dx, dy) + (std::numbers::sqrt2 - 1) * std::min(dx, dy));
      };
      const double room = (dist(pos(c.pad_a), pos(rc.pad_a)) + dist(pos(c.pad_b), pos(rc.pad_b))) / 2 + 500'000 + net_length({ref}) / 50;
      if (std::getenv("TM_DEBUG_BUS"))
        std::fprintf(stderr, "bus lane conn %d: detour %.3f mm inside the guide of conn %d (detour %.3f mm, room %.3f)\n", ci, detour(ci) / 1e6, ref, detour(ref) / 1e6, room / 1e6);
      if (!bus_same_layers(ci, ref) || detour(ci) > detour(ref) + room) {
        lift_items(ci);
        done = false;
      }
    }
    return done;
  }
  // After the attempts on lane `ci`: drops the guide and, if the connection is routed, decides whether it is a lane
  // (it leads, or has its reference lane's layers in order, either way round, and as many vias).
  void bus_note(int ci, bool ok) {
    bus_clear_guide();
    auto& st = cs[static_cast<std::size_t>(ci)];
    if (!ok) return;
    const int ref = bus_reference(ci);
    const char* state = "leader";
    st.bus_lane = true;
    if (ref >= 0) {
      st.bus_lane = bus_same_layers(ci, ref);
      state = st.bus_lane ? "followed" : "left";
    }
    if (opt.sink)
      emit("{\"type\":\"bus_lane\",\"bus\":\"" + json_text(opt.buses[static_cast<std::size_t>(bundles[static_cast<std::size_t>(conn_bundle[static_cast<std::size_t>(ci)])].bus)].name) +
           "\",\"conn\":" + std::to_string(ci) + ",\"net\":" + std::to_string(st.c.net) + ",\"state\":\"" + state + "\"}");
    if (std::getenv("TM_DEBUG_BUS"))
      std::fprintf(stderr, "bus lane %s conn %d: %s, %.3f mm, %zu items\n", b.nets[static_cast<std::size_t>(st.c.net)].name.c_str(), ci, state,
                   net_length({ci}) / 1e6, st.items.size());
  }
  // A net's routed length as KiCad's skew check sums it: tracks, and the stackup height of each via between the
  // layers it joins (route/bus.hpp; `tracemaker buses` reports the same).
  double bus_length(NetId net, const std::vector<int>& list, double* via_part = nullptr) const {
    std::vector<model::Track> tr;
    std::vector<model::Via> vi;
    for (int ci : list)
      for (int item : cs[static_cast<std::size_t>(ci)].items) {
        const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
        if (it.kind == drc::ItemKind::Track) tr.push_back(b.tracks[static_cast<std::size_t>(it.index)]);
        else if (it.kind == drc::ItemKind::Via) vi.push_back(b.vias[static_cast<std::size_t>(it.index)]);
      }
    const double v = net_via_length(b, bus_vh, net, tr, {}, vi);
    if (via_part) *via_part = v;
    return net_length(list) + v;
  }
  // Length matching of the groups with a skew limit: members with every connection routed are measured (tracks
  // and via heights, as KiCad's skew check does), and each one shorter than the longest by more than half the limit
  // gets meanders up to between half the limit below the longest and the longest. Returns the number of groups
  // within their limit afterwards.
  int tune_bus_skew() {
    int within = 0;
    std::set<NetId> open;  // nets with a connection still unrouted: their length is not the net's
    for (const auto& st : cs)
      if (!st.routed) open.insert(st.c.net);
    for (const auto& g : opt.buses) {
      if (g.skew <= 0) continue;
      auto lengths = [&](double& lo, double& hi) {
        const auto by = routed_conns_by_net();
        int n = 0;
        for (NetId net : g.nets) {
          const auto it = by.find(net);
          if (open.count(net) || it == by.end()) continue;
          const double len = bus_length(net, it->second);
          lo = n ? std::min(lo, len) : len;
          hi = n ? std::max(hi, len) : len;
          ++n;
        }
        return n;
      };
      double lo = 0, hi = 0;
      if (lengths(lo, hi) < 2) continue;
      const double longest = hi;
      const auto by = routed_conns_by_net();
      if (std::getenv("TM_DEBUG_BUS"))
        for (NetId net : g.nets)
          if (const auto it = by.find(net); it != by.end())
            std::fprintf(stderr, "bus %s: %s %.3f mm before tuning%s\n", g.name.c_str(), b.nets[static_cast<std::size_t>(net)].name.c_str(),
                         bus_length(net, it->second) / 1e6, open.count(net) ? " (incomplete)" : "");
      tune_bus = true;
      for (NetId net : g.nets) {
        const auto it = by.find(net);
        if (open.count(net) || it == by.end()) continue;
        double via_part = 0;
        if (longest - bus_length(net, it->second, &via_part) <= static_cast<double>(g.skew) / 2) continue;
        // tune_net counts tracks; meanders add no via, so the net's via length comes off both bounds.
        tune_net(net, it->second, static_cast<Coord>(longest - static_cast<double>(g.skew) / 2 - via_part), static_cast<Coord>(longest - via_part));
      }
      tune_bus = false;
      lengths(lo, hi);
      if (hi - lo <= static_cast<double>(g.skew)) ++within;
      if (opt.sink)
        emit("{\"type\":\"bus_skew\",\"bus\":\"" + json_text(g.name) + "\",\"skew\":" + std::to_string(static_cast<Coord>(hi - lo)) + ",\"limit\":" +
             std::to_string(g.skew) + "}");
      if (std::getenv("TM_DEBUG_BUS")) std::fprintf(stderr, "bus %s: skew %.3f mm (limit %.3f)\n", g.name.c_str(), (hi - lo) / 1e6, nm_to_mm(g.skew));
    }
    return within;
  }

  // Tidy sweep (doc 16 §12): every routed connection is lifted and searched again with nothing else moving (strict, the usual
  // costs); the new copper is kept only if length + 2 mm per via went down, else the old copper comes back exactly. A route
  // can only get cheaper, and the room a detour or loop gave up may be what an open connection needs. Longest excess first.
  int tidy_sweep() {
    struct Old { drc::ItemKind kind; int index; };
    auto cost_of = [&](const std::vector<int>& items) {
      double len = 0;
      int vias = 0;
      for (int item : items) {
        const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
        if (it.kind == drc::ItemKind::Via) ++vias;
        else len += std::hypot(static_cast<double>(b.tracks[static_cast<std::size_t>(it.index)].b.x - b.tracks[static_cast<std::size_t>(it.index)].a.x),
                               static_cast<double>(b.tracks[static_cast<std::size_t>(it.index)].b.y - b.tracks[static_cast<std::size_t>(it.index)].a.y));
      }
      return len + 2e6 * vias;
    };
    std::vector<std::pair<double, int>> order;  // (-excess over the straight distance, connection)
    for (std::size_t i = 0; i < cs.size(); ++i) {
      const auto& st = cs[i];
      if (!st.routed || st.implicit || st.coupled || st.bus_lane || st.items.empty() || st.c.pad_b < 0) continue;
      const Point a = b.pads[static_cast<std::size_t>(st.c.pad_a)].pos, e = b.pads[static_cast<std::size_t>(st.c.pad_b)].pos;
      order.emplace_back(-(cost_of(st.items) - std::hypot(static_cast<double>(e.x - a.x), static_cast<double>(e.y - a.y))), static_cast<int>(i));
    }
    std::sort(order.begin(), order.end());
    bypass_nogoods = true;
    int done = 0;
    for (const auto& [excess, ci] : order) {
      if (out_of_budget()) break;
      auto& st = cs[static_cast<std::size_t>(ci)];
      const double old_cost = cost_of(st.items);
      std::vector<Old> saved;
      for (int item : st.items) {
        const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
        saved.push_back({it.kind, it.index});
        remove_routed(item);
      }
      st.items.clear();
      current = ci;
      const bool ok = search_and_commit(st.c, false);
      if (ok && cost_of(st.items) < old_cost - 1e3) {
        ++done;
        continue;
      }
      for (int item : st.items) remove_routed(item);
      st.items.clear();
      for (const auto& o : saved) {
        const int id = o.kind == drc::ItemKind::Via ? obs->add_via(o.index, ci) : obs->add_track(o.index, ci);
        near_mark(id, +1);
        st.items.push_back(id);
      }
    }
    bypass_nogoods = false;
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
      if (!st.routed || st.implicit || st.coupled || st.bus_lane || st.items.empty()) continue;
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
      if (!cs[ci].routed || cs[ci].implicit || cs[ci].coupled || cs[ci].bus_lane) continue;
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
            !cs[static_cast<std::size_t>(o)].implicit && !cs[static_cast<std::size_t>(o)].coupled && !cs[static_cast<std::size_t>(o)].bus_lane &&
            !(bus_net(cs[static_cast<std::size_t>(o)].c.net) && !bus_net(cs[static_cast<std::size_t>(owner)].c.net)) &&
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

  // Regional rebuild of open connections (doc 16 F4). For each unrouted connection: lift the routed connections whose
  // copper lies within `lns_open_radius` of its first pad, route the open one first and the lifted ones after it,
  // in seeded orders, strictly (no crossing). The first order that routes all of them is kept; otherwise every
  // lifted connection is restored exactly, so completion never drops (transaction, rule 4). Open connections of
  // a dense package are usually blocked by their neighbours' escapes, which a restart redoes with the same order.
  int lns_open(int trials) {
    bypass_nogoods = true;
    strict_pass = false;
    soft = false;
    std::vector<int> open;
    for (std::size_t ci = 0; ci < cs.size(); ++ci)
      if (!cs[ci].routed && !cs[ci].implicit && !cs[ci].coupled && !cs[ci].bus_lane && cs[ci].c.pad_b >= 0) open.push_back(static_cast<int>(ci));
    int fixed = 0;
    for (int x : open) {
      if (out_of_budget()) break;
      if (cs[static_cast<std::size_t>(x)].routed) continue;
      const Point p0 = b.pads[static_cast<std::size_t>(cs[static_cast<std::size_t>(x)].c.pad_a)].pos;
      std::vector<int> ids;
      obs->routed_items_in(geom::Shape::point(p0, static_cast<Coord>(opt.lns_open_radius_mm * 1e6)).box, ids);
      std::vector<int> group;
      for (int id : ids) {
        const int o = obs->copper().items[static_cast<std::size_t>(id)].owner;
        if (o < 0 || o == x || std::find(group.begin(), group.end(), o) != group.end()) continue;
        const auto& st = cs[static_cast<std::size_t>(o)];
        if (!st.routed || st.implicit || st.coupled || st.bus_lane) continue;
        if (first_net_ids.count(st.c.net) && !first_net_ids.count(cs[static_cast<std::size_t>(x)].c.net)) continue;
        if (bus_net(st.c.net) && !bus_net(cs[static_cast<std::size_t>(x)].c.net)) continue;
        group.push_back(o);
      }
      std::sort(group.begin(), group.end());
      if (group.empty() || group.size() > 24) continue;
      std::vector<std::vector<OldItem>> saved;
      for (int g : group) saved.push_back(lift_items(g));
      bool done = false;
      for (int trial = 0; trial < trials && !done && !out_of_budget(); ++trial) {
        std::vector<int> order(group.begin(), group.end());
        const RngStream rr(opt.seed, 0x1A57u, static_cast<std::uint64_t>(x) * 4096 + static_cast<std::uint64_t>(trial));
        std::vector<std::pair<double, int>> key;
        for (std::size_t k = 0; k < order.size(); ++k) key.emplace_back(rr.uniform(k), order[k]);
        std::sort(key.begin(), key.end());
        std::vector<int> seq{x};
        for (const auto& kv : key) seq.push_back(kv.second);
        bool ok = true;
        std::size_t placed = 0;
        for (int ci : seq) {
          current = ci;
          if (!search_and_commit(cs[static_cast<std::size_t>(ci)].c, false)) {
            ok = false;
            break;
          }
          ++placed;
        }
        if (ok) {
          done = true;
          break;
        }
        for (std::size_t k = 0; k < placed; ++k) lift_items(seq[k]);  // undo this order
      }
      if (done) {
        auto& st = cs[static_cast<std::size_t>(x)];
        st.routed = true;
        st.dead = false;
        ++res.routed;
        release_escapes(x);
        ++fixed;
      } else {
        for (std::size_t k = 0; k < group.size(); ++k) restore_items(group[k], saved[k]);
      }
    }
    bypass_nogoods = false;
    return fixed;
  }

  // Proof-triggered pin rescue (D155, doc 05 §63). Called when a strict search has just proved an end of connection x
  // boxed in: if this run's routed copper is part of the wall, the routed connections with copper within r of either
  // end are lifted, x is routed first and the lifted ones after it, in seeded orders. Kept only if all of them route
  // again (the routed count rises by one); otherwise every lifted connection is restored exactly (rule 4). On failure
  // the radius widens (opt.rescue_radii_mm), which is the "widen the last traces" of the M22 research: on the failing
  // PCBench boards, other nets' copper within 2 mm of an open pin was what blocked 155 of 288 that a whole-board
  // search with every option could not route (bench/records/m22-research). Freerouting's RetryNeighbourhood grows its
  // rip radius the same way (1.5 to 12 mm). Each search here is capped at rescue_cap expansions; it counts as work.
  bool pin_rescue(int x) {
    auto& sx = cs[static_cast<std::size_t>(x)];
    if (sx.routed || sx.c.pad_a < 0 || sx.rescues >= opt.rescue_per_conn) return false;
    ++sx.rescues;
    const bool keep_strict = strict_pass, keep_soft = soft, keep_bypass = bypass_nogoods;
    const long keep_cap = expansion_cap;
    const int keep_current = current;
    strict_pass = false;  // the usual window ladder: the rescue is the expensive step already
    soft = false;
    bypass_nogoods = true;
    std::vector<Point> ends{b.pads[static_cast<std::size_t>(sx.c.pad_a)].pos};
    if (sx.c.pad_b >= 0) ends.push_back(b.pads[static_cast<std::size_t>(sx.c.pad_b)].pos);
    bool done = false;
    for (std::size_t ri = 0; ri < opt.rescue_radii_mm.size() && !done && !out_of_budget(); ++ri) {
      const Coord r = static_cast<Coord>(opt.rescue_radii_mm[ri] * 1e6);
      std::vector<int> group;
      for (const Point& p : ends) {
        std::vector<int> ids;
        obs->routed_items_in(geom::Shape::point(p, r).box, ids);
        for (int id : ids) {
          const int o = obs->copper().items[static_cast<std::size_t>(id)].owner;
          if (o < 0 || o == x || std::find(group.begin(), group.end(), o) != group.end()) continue;
          const auto& st = cs[static_cast<std::size_t>(o)];
          if (!st.routed || st.implicit || st.coupled || st.bus_lane || st.bundle_lane) continue;
          if (first_net_ids.count(st.c.net) && !first_net_ids.count(sx.c.net)) continue;
          if (bus_net(st.c.net) && !bus_net(sx.c.net)) continue;
          group.push_back(o);
        }
      }
      if (group.empty()) continue;  // the wall is fixed copper this close: a wider radius may still hold routed copper
      if (static_cast<int>(group.size()) > opt.rescue_victims) break;  // wider only lifts more
      std::sort(group.begin(), group.end());
      ++res.rescue_tried;
      std::vector<std::vector<OldItem>> saved;
      for (int g : group) saved.push_back(lift_items(g));
      auto route = [&](int ci) {
        current = ci;
        expansion_cap = opt.rescue_cap;
        const bool ok = search_and_commit(cs[static_cast<std::size_t>(ci)].c, false);
        expansion_cap = keep_cap;
        return ok;
      };
      const bool rdbg = std::getenv("TM_DEBUG_RESCUE") != nullptr;
      if (rdbg) std::fprintf(stderr, "rescue %s r=%.1f mm: %zu lifted\n", pad_label(sx.c.pad_a).c_str(), opt.rescue_radii_mm[ri], group.size());
      if (!route(x)) {  // the pin does not route even with its neighbours gone: a wider radius may lift what blocks it
        if (rdbg) std::fprintf(stderr, "  pin still fails: %s\n", why.c_str());
        for (std::size_t k = 0; k < group.size(); ++k) restore_items(group[k], saved[k]);
        continue;
      }
      // Every victim whose old copper still passes the exact check gets it back unchanged; only those the new path
      // crosses are routed again, in seeded orders.
      std::vector<char> back(group.size(), 0);
      for (std::size_t k = 0; k < group.size(); ++k)
        if (saved_fits(saved[k])) {
          restore_items(group[k], saved[k]);
          back[k] = 1;
        }
      if (rdbg) std::fprintf(stderr, "  pin routed; %d of %zu lifted fit again unchanged\n", static_cast<int>(std::count(back.begin(), back.end(), 1)), group.size());
      for (int trial = 0; trial < opt.rescue_orders && !done && !out_of_budget(); ++trial) {
        const RngStream rr(opt.seed, 0x5E5Cu, (static_cast<std::uint64_t>(x) * 64 + ri) * 64 + static_cast<std::uint64_t>(trial));
        std::vector<std::pair<double, std::size_t>> key;
        for (std::size_t k = 0; k < group.size(); ++k)
          if (!back[k]) key.emplace_back(trial == 0 ? static_cast<double>(k) : rr.uniform(k), k);
        std::sort(key.begin(), key.end());
        std::vector<int> placed;
        bool ok = true;
        for (const auto& kv : key) {
          if (!(ok = route(group[kv.second]))) break;
          placed.push_back(group[kv.second]);
        }
        if (rdbg) std::fprintf(stderr, "  order %d: %zu of %zu re-routed%s\n", trial, placed.size(), key.size(), ok ? "" : (", failed: " + why).c_str());
        if (ok) done = true;
        else
          for (int ci : placed) lift_items(ci);  // undo this order
      }
      if (!done) {
        lift_items(x);
        for (std::size_t k = 0; k < group.size(); ++k)
          if (!back[k]) restore_items(group[k], saved[k]);
      }
    }
    strict_pass = keep_strict;
    soft = keep_soft;
    bypass_nogoods = keep_bypass;
    current = keep_current;
    if (done) ++res.rescued;
    return done;
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
  // `rate`: tune a delay instead (doc 05 §59): `mn` and `mx` are attoseconds, the net is measured as KiCad measures
  // it (vias and pad-to-die delays included), and a nanometre of meander on copper layer l adds rate[l]; tracks on a
  // layer whose rate is 0 get none.
  bool tune_net(NetId net, const std::vector<int>& list, std::optional<Coord> mn, std::optional<Coord> mx, const std::vector<double>* rate = nullptr,
                std::vector<int>* ripped = nullptr, NetId keep = 0) {
    auto seg_len = [](const model::Track& t) { return std::hypot(static_cast<double>(t.b.x - t.a.x), static_cast<double>(t.b.y - t.a.y)); };
    auto rate_on = [&](int layer) { return rate ? (*rate)[static_cast<std::size_t>(layer)] : 1.0; };
      double len = 0;
      if (rate) {
        len = kicad_measure(net).total_delay();
      } else {
        for (int ci : list)
          for (int item : cs[static_cast<std::size_t>(ci)].items) {
            const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
            if (it.kind == drc::ItemKind::Track) len += seg_len(b.tracks[static_cast<std::size_t>(it.index)]);
          }
      }
      const double target = mx ? (static_cast<double>(*mn) + static_cast<double>(*mx)) / 2 : static_cast<double>(*mn) * 1.005;
      double deficit = target - len;
      if (static_cast<double>(*mn) <= len) return true;
      const Coord w = class_width(net), clr = std::max(netclass(net).clearance, rules.minimums.clearance);
      const Coord p = w + clr + 20'000;  // spacing between the meander's parallel runs (centre to centre)
      // Bus members (tune_bus): the lanes beside a member leave its longest track no room, so every track is tried,
      // longest first, and the last meander is cut to the length still missing (the fixed heights step by 0.5 mm).
      std::vector<int> tried;
      // A bus member or a delay may need several tracks: each is tried, longest first.
      const bool every_track = tune_bus || rate;
      for (int round = 0; round < (every_track ? 400 : 50) && deficit > 1'000; ++round) {
        // Longest track of the net first.
        int best_ci = -1, best_item = -1;
        double best_l = 0;
        for (int ci : list)
          for (int item : cs[static_cast<std::size_t>(ci)].items) {
            const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
            if (it.kind != drc::ItemKind::Track) continue;
            if (!tried.empty() && std::find(tried.begin(), tried.end(), item) != tried.end()) continue;
            if (rate_on(b.tracks[static_cast<std::size_t>(it.index)].layer) <= 0) continue;
            const double l = seg_len(b.tracks[static_cast<std::size_t>(it.index)]);
            if (l > best_l && l > static_cast<double>(3 * p)) { best_l = l; best_ci = ci; best_item = item; }
          }
        if (best_item < 0) break;
        const model::Track t = b.tracks[static_cast<std::size_t>(obs->copper().items[static_cast<std::size_t>(best_item)].index)];
        const double ux = static_cast<double>(t.b.x - t.a.x) / best_l, uy = static_cast<double>(t.b.y - t.a.y) / best_l;
        bool placed = false;
        const double r = rate_on(t.layer);
        std::array<double, 6> heights{1.5, 1.0, 0.6, 0.4, 0.25, 0.0};
        // A bus member or a delay is brought into a narrow window: the last meander is cut to what is still missing.
        if ((tune_bus || rate) && deficit / r >= 100'000 && deficit / r <= 3e6) heights[5] = deficit / r / 2e6;
        for (double h_mm : heights) {
          if (h_mm <= 0) continue;
          const double h = h_mm * 1e6;
          int k = static_cast<int>(std::floor((best_l - static_cast<double>(p)) / (2.0 * static_cast<double>(p))));
          k = std::min(k, static_cast<int>(std::ceil(deficit / (2 * h * r))));
          if (mx) k = std::min(k, static_cast<int>(std::floor((static_cast<double>(*mx) - len) / (2 * h * r))));
          if (k <= 0) continue;
          // With `ripped` (tune_with_room), a meander of this height that only routed copper of other nets blocks is
          // placed when neither side is free: the connections in its way are ripped (and appended to `ripped`).
          for (int trial = 0; trial < (ripped ? 4 : 2); ++trial) {
            const int pass_rip = trial / 2;
            const double side = trial % 2 == 0 ? 1.0 : -1.0;
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
            if (!good && pass_rip == 1) {
              std::vector<int> owners;
              bool soft_only = true;
              for (std::size_t i = 0; i + 1 < pts.size() && soft_only; ++i)
                if (!(pts[i] == pts[i + 1]) && obs->segment_state(pts[i], pts[i + 1], t.layer, t.width, net, true, &owners) == 2) soft_only = false;
              std::sort(owners.begin(), owners.end());
              owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
              if (std::getenv("TM_DEBUG_ROOM")) {
                std::fprintf(stderr, "room cand track (%.3f,%.3f)-(%.3f,%.3f) L%d h%.2f side%+.0f k%d fixed=%d owners:", nm_to_mm(t.a.x), nm_to_mm(t.a.y), nm_to_mm(t.b.x), nm_to_mm(t.b.y), t.layer, h_mm, side, k, soft_only ? 0 : 1);
                for (int o : owners) std::fprintf(stderr, " %d(%s%s)", o, o >= 0 ? b.nets[static_cast<std::size_t>(cs[static_cast<std::size_t>(o)].c.net)].name.c_str() : "-", o >= 0 && !room_rippable(o, net, keep) ? ",keep" : "");
                std::fprintf(stderr, "\n");
              }
              for (int o : owners)
                if (!room_rippable(o, net, keep)) soft_only = false;
              if (soft_only && !owners.empty() && ripped->size() + owners.size() <= 16) {
                for (int o : owners) {
                  if (cs[static_cast<std::size_t>(o)].routed) rip(o);
                  ripped->push_back(o);
                }
                good = true;
                for (std::size_t i = 0; i + 1 < pts.size() && good; ++i)
                  if (!(pts[i] == pts[i + 1]) && obs->segment_state(pts[i], pts[i + 1], t.layer, t.width, net, false, nullptr) != 0) good = false;
              }
            }
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
            len += 2 * h * k * r;
            deficit -= 2 * h * k * r;
            placed = true;
            break;
          }
          if (placed) break;
        }
        if (!placed && every_track) {
          tried.push_back(best_item);
          continue;
        }
        if (!placed) break;
        tried.clear();
      }
      if (rate) len = kicad_measure(net).total_delay();
      return len >= static_cast<double>(*mn) && (!mx || len <= static_cast<double>(*mx));
  }

  // A routed connection tune_with_room may rip to make room for the meanders of `net`: one of another net, not `keep`
  // (the pair partner), with no length, delay or skew rule of its own, not coupled and not a bus or bundle lane.
  bool room_rippable(int ci, NetId net, NetId keep) const {
    if (ci < 0) return false;
    const auto& st = cs[static_cast<std::size_t>(ci)];
    if (!st.routed || st.implicit || st.coupled || st.bus_lane || st.bundle_lane || st.c.net == net || st.c.net == keep) return false;
    return !obs->rules().net_constraint(st.c.net, "length") && obs->rules().net_rule_with(st.c.net, "skew") < 0 && !obs->rules().skew_constraint(st.c.net);
  }

  // Delay tuning that makes room (D152, doc 05 §61): when the meanders of `net` do not fit, tuning goes on with meanders
  // that only routed connections of other nets block (room_rippable): those are ripped, then re-routed strictly. The
  // result is kept only if the net is in range and every ripped connection routes again; otherwise the board goes back
  // to how it was after the plain attempt.
  bool tune_with_room(NetId net, const std::vector<int>& list, Coord mn, Coord mx, const std::vector<double>& rate, NetId keep) {
    const TxnSnap before = txn_capture();
    if (tune_net(net, list, mn, mx, &rate)) return true;
    // The plain attempt filled the free room with small meanders; the room attempt starts again from the untuned net so
    // that a tall meander can take the place of routed copper. On failure the plain attempt's result is restored.
    const TxnSnap snap = txn_capture();
    txn_restore(before);
    const std::size_t pend = pending.size();
    const int saved_current = current;
    const double saved_scale = budget_scale;
    std::vector<std::uint8_t> was(cs.size());
    for (std::size_t k = 0; k < cs.size(); ++k) was[k] = cs[k].routed ? 1 : 0;
    std::vector<int> victims;
    bool ok = tune_net(net, list, mn, mx, &rate, &victims, keep);
    if (victims.empty()) {  // nothing was ripped: the same free meanders as the plain attempt
      if (!ok) txn_restore(snap);
      return ok;
    }
    if (std::getenv("TM_DEBUG_TUNE")) std::fprintf(stderr, "  room for %s: retune %s, now %.3f ps\n", b.nets[static_cast<std::size_t>(net)].name.c_str(), ok ? "yes" : "no", kicad_measure(net).total_delay() / 1e3);
    if (ok) {
      // Tuning runs after the search budget is spent: the re-routes get an allowance of their own, in work units.
      constexpr long kRoomWork = 400'000;
      if (opt.work_budget > 0)
        budget_scale = std::max(budget_scale, static_cast<double>(res.expansions + kRoomWork) / static_cast<double>(opt.work_budget));
      else
        budget_scale = std::max(budget_scale, elapsed() / opt.time_limit_s + 0.02);
      for (std::size_t k = 0; k < cs.size() && ok; ++k) {
        auto& st = cs[k];
        if (!was[k] || st.routed) continue;
        current = static_cast<int>(k);
        bool again = false;
        if (joined(static_cast<int>(k))) {
          st.implicit = again = true;
        } else {
          expansion_cap = opt.txn_cap > 0 ? opt.txn_cap : 200'000;
          again = search_and_commit(st.c, false);
          expansion_cap = 0;
        }
        if (!again) {
          ok = false;
          break;
        }
        st.routed = true;
        ++res.routed;
        release_escapes(static_cast<int>(k));
      }
      budget_scale = saved_scale;
    }
    pending.resize(pend);
    current = saved_current;
    if (std::getenv("TM_DEBUG_TUNE"))
      std::fprintf(stderr, "  room for %s: %zu ripped, %s\n", b.nets[static_cast<std::size_t>(net)].name.c_str(), victims.size(), ok ? "kept" : "rolled back");
    if (ok) return true;
    txn_restore(snap);
    for (int v : victims) release_escapes(v);
    return false;
  }

  // `net` as KiCad's length check measures it now (drc/length.hpp): every live pad, track, arc and via of the net.
  drc::NetLength kicad_measure(NetId net) {
    if (len_vh.to.empty() && rules.use_height_for_length) len_vh = drc::via_heights(b, rules);
    drc::LengthItems li;
    for (const auto& it : obs->copper().items) {
      if (it.net != net || it.removed) continue;
      switch (it.kind) {
        case drc::ItemKind::Pad: li.pads.push_back(it.index); break;
        case drc::ItemKind::Track: li.tracks.push_back(it.index); break;
        case drc::ItemKind::Arc: li.arcs.push_back(it.index); break;
        case drc::ItemKind::Via: li.vias.push_back(it.index); break;
        default: break;
      }
    }
    return drc::net_length(b, rules, len_vh, li, rules.profile_of(netclass(net)));
  }

  // Delay per nanometre of track on each copper layer for `net` (attoseconds), from its class's tuning profile; empty
  // when the net has no profile.
  std::vector<double> delay_rates(NetId net) const {
    const auto* tp = rules.profile_of(netclass(net));
    if (!tp) return {};
    std::vector<double> out(static_cast<std::size_t>(b.copper_count()), 0.0);
    for (int l = 0; l < b.copper_count(); ++l)
      for (const auto& e : tp->layers)
        if (e.layer == b.copper_name(l)) out[static_cast<std::size_t>(l)] = static_cast<double>(e.delay) / 1e6;
    return out;
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

  // Delay tuning (doc 05 §59): a `length` rule in ps or fs is a window on the net's delay, which meanders raise by the
  // delay per mm of the layer they are on (the class's tuning profile). Then each `skew` rule in ps: the nets it groups
  // (or each pair, with within_diff_pairs) are brought to within half the limit below the one with the most delay,
  // which KiCad measures the others against. Returns the nets brought into range.
  int tune_delays() {
    int tuned = 0;
    const auto by = routed_conns_by_net();
    for (const auto& [net, list] : by) {
      const auto k = obs->rules().net_constraint(net, "length");
      if (!k || !k->time || !k->min) continue;
      const auto rate = delay_rates(net);
      if (rate.empty()) continue;
      if (k->max ? tune_with_room(net, list, *k->min, *k->max, rate, obs->rules().dp_partner(net)) : tune_net(net, list, k->min, k->max, &rate)) ++tuned;
    }
    std::map<int, std::vector<NetId>> groups;  // skew rule position → its routed nets
    for (const auto& [net, list] : by) {
      const int ri = obs->rules().net_rule_with(net, "skew");
      if (ri < 0) continue;
      const auto k = obs->rules().net_constraint(net, "skew");
      if (k && k->time && k->max) groups[ri].push_back(net);
    }
    for (const auto& [ri, nets] : groups) {
      const auto k = obs->rules().net_constraint(nets.front(), "skew");
      std::vector<std::vector<NetId>> sets;
      if (std::find(k->items.begin(), k->items.end(), "within_diff_pairs") != k->items.end()) {
        for (NetId n : nets) {
          const NetId q = obs->rules().dp_partner(n);
          if (q > n && std::find(nets.begin(), nets.end(), q) != nets.end()) sets.push_back({n, q});
        }
      } else {
        sets.push_back(nets);
      }
      for (const auto& set : sets) {
        double longest = 0;
        for (NetId n : set) longest = std::max(longest, kicad_measure(n).total_delay());
        for (NetId n : set) {
          if (std::getenv("TM_DEBUG_TUNE"))
            std::fprintf(stderr, "delay skew %s: %.3f ps, longest %.3f ps, max %.3f ps\n", b.nets[static_cast<std::size_t>(n)].name.c_str(),
                         kicad_measure(n).total_delay() / 1e3, longest / 1e3, static_cast<double>(*k->max) / 1e3);
          if (longest - kicad_measure(n).total_delay() <= static_cast<double>(*k->max) / 2) { ++tuned; continue; }
          const auto rate = delay_rates(n);
          if (rate.empty()) continue;
          const auto mn = static_cast<Coord>(longest - static_cast<double>(*k->max) / 4), mx = static_cast<Coord>(longest);
          const bool ok = tune_with_room(n, by.at(n), mn, mx, rate, obs->rules().dp_partner(n));
          if (std::getenv("TM_DEBUG_TUNE")) std::fprintf(stderr, "  tuned %s: %s, now %.3f ps\n", b.nets[static_cast<std::size_t>(n)].name.c_str(), ok ? "yes" : "no", kicad_measure(n).total_delay() / 1e3);
          if (ok) ++tuned;
        }
      }
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

  // Lattice samples of the segment a-c over a void of the layer's reference plane (issue #24): how much of it a critical net would run over a split.
  std::size_t void_samples(int layer, Point a, Point c) const {
    if (!split_on) return 0;
    const double len = std::hypot(static_cast<double>(c.x - a.x), static_cast<double>(c.y - a.y));
    const int n = std::max(1, static_cast<int>(std::ceil(len / static_cast<double>(pitch))));
    std::size_t hits = 0;
    for (int k = 0; k <= n; ++k) {
      const Point p{a.x + static_cast<Coord>(std::llround(static_cast<double>(c.x - a.x) * k / n)), a.y + static_cast<Coord>(std::llround(static_cast<double>(c.y - a.y) * k / n))};
      const int gx = to_ix(p.x), gy = to_iy(p.y);
      if (gx >= 0 && gy >= 0 && gx < nx && gy < ny && refmap.over_void[lat_index(layer, gx, gy)]) ++hits;
    }
    return hits;
  }

  int smooth_paths() {
    int changed = 0;
    auto octi = [](Point a, Point c) {
      const Coord dx = c.x - a.x, dy = c.y - a.y;
      return dx == 0 || dy == 0 || std::llabs(dx) == std::llabs(dy);
    };
    // Up to two octilinear segments from a to c (straight if already octilinear), or empty if none is legal.
    // `max_void`: a critical net's shortcut may not run over more of its reference plane's voids than the path it replaces did (the shortest way is
    // not the one the search chose when the split cost made it dear).
    auto shortcut = [&](Point a, Point c, int layer, Coord width, NetId net, std::size_t max_void) -> std::vector<Point> {
      auto legal = [&](Point u, Point v) { return u == v || obs->segment_state(u, v, layer, width, net, false, nullptr) == 0; };
      auto over = [&](const std::vector<Point>& v) {
        if (!split_on || !crit_split[static_cast<std::size_t>(net)]) return false;
        std::size_t n = 0;
        for (std::size_t i = 0; i + 1 < v.size(); ++i) n += void_samples(layer, v[i], v[i + 1]);
        return n > max_void;
      };
      if (octi(a, c)) return legal(a, c) && !over({a, c}) ? std::vector<Point>{a, c} : std::vector<Point>{};
      const Coord dx = c.x - a.x, dy = c.y - a.y, m = std::min(std::llabs(dx), std::llabs(dy));
      const Point diag{a.x + (dx > 0 ? m : -m), a.y + (dy > 0 ? m : -m)};       // diagonal first
      if (legal(a, diag) && legal(diag, c) && !over({a, diag, c})) return {a, diag, c};
      const Point straight{c.x - (dx > 0 ? m : -m), c.y - (dy > 0 ? m : -m)};  // straight first
      if (legal(a, straight) && legal(straight, c) && !over({a, straight, c})) return {a, straight, c};
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
        // Void samples of the run up to each vertex (critical nets with the split cost only): the part a shortcut would replace is a difference.
        std::vector<std::size_t> void_to(pts.size(), 0);
        const bool crit_run = split_on && crit_split[static_cast<std::size_t>(tr[k].net)];
        if (crit_run)
          for (std::size_t q = 0; q + 1 < pts.size(); ++q) void_to[q + 1] = void_to[q] + void_samples(tr[k].layer, pts[q], pts[q + 1]);
        std::size_t i = 0;
        while (i + 1 < pts.size()) {
          std::size_t best = i + 1;
          std::vector<Point> via_pts;
          for (std::size_t j = pts.size() - 1; j >= i + 2; --j) {
            auto sc = shortcut(pts[i], pts[j], tr[k].layer, tr[k].width, tr[k].net, void_to[j] - void_to[i]);
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

  // ---------------------------------------------------------------------------------------------------
  // Bundle escapes (doc 05 §50, route/bundle.hpp): lanes placed as geometry before the search starts. The planner proposes polylines; every piece is checked
  // by the router's own exact test, here and again when the lane is placed, so the plan cannot introduce a violation.
  // ---------------------------------------------------------------------------------------------------
  bool place_bundle_lane(int ci, int layer, const std::vector<Point>& pts) {
    auto& st = cs[static_cast<std::size_t>(ci)];
    if (st.routed || pts.size() < 2) return false;
    const NetId net = st.c.net;
    const Coord width = class_width(net);
    if (!layer_ok(net, layer)) return false;
    const Point pa = b.pads[static_cast<std::size_t>(st.c.pad_a)].pos, pb = b.pads[static_cast<std::size_t>(st.c.pad_b)].pos;
    if (!((pts.front() == pa && pts.back() == pb) || (pts.front() == pb && pts.back() == pa))) return false;
    for (std::size_t i = 1; i < pts.size(); ++i)
      if (obs->segment_state(pts[i - 1], pts[i], layer, width, net, false, nullptr) != 0) return false;
    for (std::size_t i = 1; i < pts.size(); ++i) {
      b.tracks.push_back(model::Track{pts[i - 1], pts[i], width, layer, net, false, sexpr::kNoNode});
      const int id = static_cast<int>(b.tracks.size() - 1);
      st.items.push_back(obs->add_track(id, ci));
      near_mark(st.items.back(), +1);
      emit_track_add(id);
    }
    st.routed = true;
    st.bus_lane = true;     // clean-up leaves it alone
    st.bundle_lane = true;  // and nothing may rip it
    ++res.routed;
    release_escapes(ci);
    return true;
  }
  bool place_bundle_tunnel(int ci, const std::vector<std::pair<int, std::vector<Point>>>& tracks, const std::vector<Point>& vias) {
    auto& st = cs[static_cast<std::size_t>(ci)];
    if (st.routed) return false;
    const NetId net = st.c.net;
    const Coord width = class_width(net), vd = via_diameter(net), vdrill = via_drill(net);
    for (const auto& [layer, pts] : tracks) {
      if (!layer_ok(net, layer)) return false;
      for (std::size_t i = 1; i < pts.size(); ++i)
        if (obs->segment_state(pts[i - 1], pts[i], layer, width, net, false, nullptr) != 0) return false;
    }
    for (const Point v : vias)
      if (obs->via_state(v, vd, vdrill, net, 0, false, nullptr) != 0) return false;
    for (const auto& [layer, pts] : tracks)
      for (std::size_t i = 1; i < pts.size(); ++i) {
        b.tracks.push_back(model::Track{pts[i - 1], pts[i], width, layer, net, false, sexpr::kNoNode});
        const int id = static_cast<int>(b.tracks.size() - 1);
        st.items.push_back(obs->add_track(id, ci));
        near_mark(st.items.back(), +1);
        emit_track_add(id);
      }
    for (const Point v : vias) {
      b.vias.push_back(model::Via{v, vd, vdrill, 0, nl - 1, model::ViaType::Through, net, false, sexpr::kNoNode});
      const int id = static_cast<int>(b.vias.size() - 1);
      st.items.push_back(obs->add_via(id, ci));
      near_mark(st.items.back(), +1);
      emit_via_add(id);
    }
    st.routed = true;
    st.bus_lane = true;
    st.bundle_lane = true;
    ++res.routed;
    release_escapes(ci);
    return true;
  }
  int route_bundle_escapes() {
    std::vector<BundleConn> in;
    for (std::size_t ci = 0; ci < cs.size(); ++ci) {
      const auto& st = cs[ci];
      if (st.routed || st.implicit || st.dead || st.c.pad_a < 0 || st.c.pad_b < 0 || st.c.zone_b >= 0) continue;
      if (!pair_partner.empty() && pair_partner[ci] >= 0) continue;
      in.push_back({static_cast<int>(ci), st.c.net, st.c.pad_a, st.c.pad_b});
    }
    BundleHooks h;
    const bool dbg_legs = std::getenv("TM_DEBUG_BUNDLE_LEGS") != nullptr;
    h.free = [&](Point a, Point c, int layer, Coord w, NetId net) {
      const bool ok = layer_ok(net, layer) && obs->segment_state(a, c, layer, w, net, false, nullptr) == 0;
      if (!ok && dbg_legs) {
        std::vector<int> owners;
        const int st = obs->segment_state(a, c, layer, w, net, true, &owners);
        std::fprintf(stderr, "      blocked (%.2f,%.2f)-(%.2f,%.2f) w %.3f net %s: ignoring routed copper %d, %zu routed owners\n", nm_to_mm(a.x), nm_to_mm(a.y), nm_to_mm(c.x), nm_to_mm(c.y),
                     nm_to_mm(w), b.nets[static_cast<std::size_t>(net)].name.c_str(), st, owners.size());
      }
      return ok;
    };
    h.width = [&](NetId n) { return class_width(n); };
    h.clearance = [&](NetId n) { return std::max(netclass(n).clearance, rules.minimums.clearance); };
    h.layer_ok = [&](int l, NetId n) { return layer_ok(n, l); };
    h.place = [&](const BundleConn& c, int layer, const std::vector<Point>& pts) { return place_bundle_lane(c.ci, layer, pts); };
    h.via_ok = [&](Point p, NetId n) { return obs->via_state(p, via_diameter(n), via_drill(n), n, 0, false, nullptr) == 0 && vias_ok(n); };
    h.via_diameter = [&](NetId n) { return via_diameter(n); };
    h.place_tunnel = [&](const BundleConn& c, const std::vector<std::pair<int, std::vector<Point>>>& tr, const std::vector<Point>& vs) { return place_bundle_tunnel(c.ci, tr, vs); };
    BundleOptions bo;
    bo.min_lanes = std::max(1, opt.bundle_min_lanes);
    if (opt.package_tunnels) {  // first: the lanes then go round the stubs and vias (after the lanes there is no room for a via between two pins' lanes)
      res.tunnels = route_tunnels(b, in, h, bo).placed;
      std::vector<BundleConn> left;
      for (const auto& c : in)
        if (!cs[static_cast<std::size_t>(c.ci)].routed) left.push_back(c);
      in = std::move(left);
    }
    BundleStats bs;
    if (opt.bundle_escape) bs = route_bundles(b, in, h, bo);
    res.bundle_groups = bs.groups, res.bundle_spines = bs.spines, res.bundle_conns = bs.lanes, res.bundle_lanes = bs.placed;
    if (std::getenv("TM_DEBUG_TIMING")) std::fprintf(stderr, "[%.2f s] bundle escapes: %d groups, %d with a spine, %d of %d connections placed as lanes\n", elapsed(), bs.groups, bs.spines, bs.placed, bs.lanes);
    if (opt.sink) emit("{\"type\":\"stage\",\"name\":\"bundle escapes\",\"state\":\"done\",\"detail\":\"" + std::to_string(bs.placed) + " lanes in " + std::to_string(bs.spines) + " bundles\"}");
    return bs.placed;
  }

  // Escape planning (M9): reserve each dense-package pin's escape corridor for its net (route/escape.hpp).
  void plan_escape_reservations() {
    std::vector<char> needs(b.pads.size(), 0);
    pad_open.assign(b.pads.size(), 0);
    esc_released.assign(cs.size(), 0);
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
        ++wc().fixed_checks;
        return layer_ok(n, layer) && code_ok(obs->fixed_code(p, layer, class_width(n) / 2, 0, n), n) && rule_state(p, layer, class_width(n) / 2, 0, n) != 2;
      };
      fin.via_free = [&](Point p, NetId n) {
        ++wc().fixed_checks;
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
      if (opt.escape_length_mm > nm_to_mm(EscapeOptions{}.length)) extend_corridors(plan);
    }
    pad_stub.clear();
    if (opt.escape_stubs || opt.fanout_hold)
      for (const auto& c : plan)
        if (!c.via && !c.has_mid && c.tail.empty() && std::hypot(static_cast<double>(c.b.x - c.a.x), static_cast<double>(c.b.y - c.a.y)) > 4.0 * static_cast<double>(pitch))
          pad_stub[c.pad] = {c.b, c.layer};
    reserve.assign(static_cast<std::size_t>(nl) * static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), 0);
    pad_reserved.assign(b.pads.size(), {});
    pad_plan.assign(b.pads.size(), {});
    vreserve.clear();
    pad_vreserved.assign(b.pads.size(), {});
    if (opt.escape_via_keep_mm > 0) vreserve.assign(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), 0);
    reserve_pen = static_cast<std::int64_t>(2 * opt.soft_cost_mm * 1e6);
    res.escape_corridors = 0;
    res.escape_corridor_mm = 0;
    plan_json.clear();
    auto mark = [&](std::size_t gi, NetId net, int pad) {
      if (reserve[gi] == 0) {
        reserve[gi] = static_cast<std::int32_t>(net);
        pad_reserved[static_cast<std::size_t>(pad)].push_back(gi);
        pad_plan[static_cast<std::size_t>(pad)].push_back(gi);
      } else if (reserve[gi] != static_cast<std::int32_t>(net)) {
        reserve[gi] = -1;  // two plans want it: reserved for nobody
      }
    };
    for (const auto& c : plan) {
      if (!layer_ok(c.net, c.layer) || (c.via && !vias_ok(c.net))) continue;  // custom disallow rules
      if (c.via) {  // a dog-bone is only worth reserving where that net's via fits among the fixed copper
        const int gx = to_ix(c.b.x), gy = to_iy(c.b.y);
        if (gx < 0 || gy < 0 || gx >= nx || gy >= ny) continue;
        ++wc().fixed_checks;
        const std::int32_t code = obs->fixed_via_code(at(gx, gy), via_diameter(c.net), via_drill(c.net), 0, cache_for(c.net).rep);
        if (!code_ok(code, c.net) || obs->via_disallowed(at(gx, gy), via_diameter(c.net), c.net, 0, nl - 1, model::ViaType::Through)) continue;
      }
      ++res.escape_corridors;
      res.escape_corridor_mm += nm_to_mm(static_cast<Coord>(std::hypot(static_cast<double>(c.b.x - c.a.x), static_cast<double>(c.b.y - c.a.y))));
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
      if (!vreserve.empty()) {  // the points where another net's via would cover the corridor's track
        const Coord rv = via_diameter(c.net) / 2 + std::max(netclass(c.net).clearance, rules.minimums.clearance) + class_width(c.net) / 2;
        const long double rv2 = static_cast<long double>(rv) * static_cast<long double>(rv);
        auto vstrip = [&](Point sa, Point sb) {
          const int ix0 = std::max(0, to_ix(std::min(sa.x, sb.x) - rv)), ix1 = std::min(nx - 1, to_ix(std::max(sa.x, sb.x) + rv));
          const int iy0 = std::max(0, to_iy(std::min(sa.y, sb.y) - rv)), iy1 = std::min(ny - 1, to_iy(std::max(sa.y, sb.y) + rv));
          const long double ux = static_cast<long double>(sb.x - sa.x), uy = static_cast<long double>(sb.y - sa.y);
          const long double len2 = ux * ux + uy * uy;
          for (int gy = iy0; gy <= iy1; ++gy)
            for (int gx = ix0; gx <= ix1; ++gx) {
              const Point p = at(gx, gy);
              const long double px = static_cast<long double>(p.x - sa.x), py = static_cast<long double>(p.y - sa.y);
              const long double t = len2 > 0 ? std::clamp((px * ux + py * uy) / len2, 0.0L, 1.0L) : 0.0L;
              const long double dx = px - t * ux, dy = py - t * uy;
              if (dx * dx + dy * dy > rv2) continue;
              const std::size_t gi = static_cast<std::size_t>(gy) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(gx);
              if (vreserve[gi] == 0) {
                vreserve[gi] = static_cast<std::int32_t>(c.net);
                pad_vreserved[static_cast<std::size_t>(c.pad)].push_back(gi);
              } else if (vreserve[gi] != static_cast<std::int32_t>(c.net)) {
                vreserve[gi] = -1;
              }
            }
        };
        Point from = c.a;
        if (c.has_mid) {
          vstrip(c.a, c.mid);
          from = c.mid;
        }
        // The keep runs escape_via_keep_mm past the pad's edge (never past the corridor's end); between there and the
        // corridor's end, where other nets' tracks may not come, the pin's fan-out via has room to stand.
        Point end = c.b;
        if (c.tail.empty()) {
          const double ux = static_cast<double>(c.b.x - from.x), uy = static_cast<double>(c.b.y - from.y), len = std::hypot(ux, uy);
          const auto [hx, hy] = pad_half_extents(b.pads[static_cast<std::size_t>(c.pad)]);
          const double edge = len > 0 ? std::abs(ux / len) * static_cast<double>(hx) + std::abs(uy / len) * static_cast<double>(hy) : 0;
          const double keep_len = std::min(len, (c.has_mid ? 0 : edge) + opt.escape_via_keep_mm * 1e6);
          end = len > 0 ? Point{from.x + static_cast<Coord>(std::llround(ux / len * keep_len)), from.y + static_cast<Coord>(std::llround(uy / len * keep_len))} : from;
        }
        vstrip(from, end);
        from = c.b;
        for (const Point& q : c.tail) {
          vstrip(from, q);
          from = q;
        }
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
  // Longer perimeter corridors (--escape-length): from the end of the planned stub, straight on, one lattice step at a time, while a track of the net
  // fits against the fixed copper (pads, keep-outs, zones as the obstacles hold them) and the board is not left. Bundles of parallel straight escapes
  // as a designer draws them: the pins of one side keep their order and their channels toward the far side of the package.
  void extend_corridors(std::vector<EscapeCorridor>& plan) {
    const Coord limit = mm_to_nm(opt.escape_length_mm);
    for (auto& c : plan) {
      if (c.via || c.has_mid || !c.tail.empty()) continue;
      const double dx = static_cast<double>(c.b.x - c.a.x), dy = static_cast<double>(c.b.y - c.a.y), len = std::hypot(dx, dy);
      if (len <= 0) continue;
      const double ux = dx / len, uy = dy / len;
      const auto [hx, hy] = pad_half_extents(b.pads[static_cast<std::size_t>(c.pad)]);
      const Coord edge = static_cast<Coord>(std::fabs(ux) > std::fabs(uy) ? hx : hy);
      const Coord w2 = class_width(c.net) / 2;
      Coord reach = static_cast<Coord>(len);
      for (Coord d = reach + pitch; d <= edge + limit; d += pitch) {
        const Point q{c.a.x + static_cast<Coord>(std::llround(ux * static_cast<double>(d))), c.a.y + static_cast<Coord>(std::llround(uy * static_cast<double>(d)))};
        const int gx = to_ix(q.x), gy = to_iy(q.y);
        if (gx < 0 || gy < 0 || gx >= nx || gy >= ny) break;
        ++wc().fixed_checks;
        if (!layer_ok(c.net, c.layer) || !code_ok(obs->fixed_code(q, c.layer, w2, 0, c.net), c.net) || rule_state(q, c.layer, w2, 0, c.net) == 2) break;
        reach = d;
      }
      c.b = {c.a.x + static_cast<Coord>(std::llround(ux * static_cast<double>(reach))), c.a.y + static_cast<Coord>(std::llround(uy * static_cast<double>(reach)))};
    }
  }
  // escape_persist: the corridors of a pin come back when its connection is ripped up, as planned (cells another net holds now stay with it).
  void reclaim_pad(int pad, NetId net) {
    if (reserve.empty() || pad < 0 || pad_plan.empty() || !pad_reserved[static_cast<std::size_t>(pad)].empty()) return;
    for (std::size_t gi : pad_plan[static_cast<std::size_t>(pad)])
      if (reserve[gi] == 0) {
        reserve[gi] = static_cast<std::int32_t>(net);
        pad_reserved[static_cast<std::size_t>(pad)].push_back(gi);
      }
    if (!pad_reserved[static_cast<std::size_t>(pad)].empty()) ++res.escape_reclaimed;
  }
  void unrelease_escapes(int ci) {
    if (!opt.escape_persist || reserve.empty() || static_cast<std::size_t>(ci) >= esc_released.size() || !esc_released[static_cast<std::size_t>(ci)]) return;
    esc_released[static_cast<std::size_t>(ci)] = 0;
    const auto& c = cs[static_cast<std::size_t>(ci)].c;
    for (int pad : {c.pad_a, c.pad_b}) {
      if (pad < 0) continue;
      ++pad_open[static_cast<std::size_t>(pad)];
      reclaim_pad(pad, c.net);
    }
  }
  // Another connection of this net, not routed yet, with a pin on the same footprint as `pad`.
  bool net_open_at_package(NetId net, int pad) const {
    const int fp = b.pads[static_cast<std::size_t>(pad)].footprint;
    if (fp < 0) return false;
    for (const auto& st : cs) {
      if (st.routed || st.c.net != net) continue;
      for (int q : {st.c.pad_a, st.c.pad_b})
        if (q >= 0 && b.pads[static_cast<std::size_t>(q)].footprint == fp) return true;
    }
    return false;
  }
  // --fanout-heads (D157): the heads go into the router's copy of the board before its obstacles are built, so they are fixed copper
  // of their net for the whole run: other nets route round them, the pin's own route runs over them. They are not routed copper, so
  // they are in no result and are not written; each route runs pad to pad and a head touches nothing but its own pad, so leaving
  // them out disconnects nothing. Planned against a copy of the obstacles at hard zones (the conservative fill).
  void place_fanout_heads() {
    Obstacles probe(b, rules);
    FanoutHeadHooks h;
    h.width = [&](NetId n) { return class_width(n); };
    h.clearance = [&](NetId n) { return std::max(netclass(n).clearance, rules.minimums.clearance); };
    h.free = [&](Point a, Point c, int l, Coord w, NetId n) { return layer_ok(n, l) && probe.segment_state(a, c, l, w, n, false, nullptr) == 0; };
    h.add = [&](Point a, Point c, int l, Coord w, NetId n) {
      b.tracks.push_back(model::Track{a, c, w, l, n, false, sexpr::kNoNode});
      return probe.add_track(static_cast<int>(b.tracks.size() - 1), 0);
    };
    h.remove = [&](int item) {
      probe.remove_item(item);
      b.tracks.pop_back();
    };
    FanoutHeadOptions fo;
    fo.reach = mm_to_nm(opt.fanout_heads_mm);
    fo.min_pins = opt.fanout_heads_min_pins;
    const auto heads = plan_fanout_heads(b, h, fo);
    res.fanout_heads = static_cast<int>(heads.size());
    if (std::getenv("TM_DEBUG_HEADS"))
      for (const auto& fh : heads) {
        const auto& p = b.pads[static_cast<std::size_t>(fh.pad)];
        std::fprintf(stderr, "head %s.%s %s:", b.footprints[static_cast<std::size_t>(p.footprint)].reference.c_str(), p.number.c_str(), b.nets[static_cast<std::size_t>(fh.net)].name.c_str());
        for (const Point q : fh.pts) std::fprintf(stderr, " (%.3f,%.3f)", nm_to_mm(q.x), nm_to_mm(q.y));
        std::fprintf(stderr, "\n");
      }
  }
  // --fanout-hold (D156): before the strict pass, a placeholder track of the pin's net along each open pin's straight corridor, checked
  // exactly against all copper. It belongs to no connection (it is never in a result or a snapshot) and goes when the pin is routed or
  // the strict pass ends; until then the other nets' tracks and vias route round it, as round a designer's fan-out.
  void place_holds() {
    hold_item.assign(b.pads.size(), -1);
    for (std::size_t pad = 0; pad < b.pads.size(); ++pad) {
      const auto it = pad_stub.find(static_cast<int>(pad));
      if (it == pad_stub.end() || pad_open[pad] == 0) continue;
      const auto& p = b.pads[pad];
      const Point V = it->second.first;
      const int l = it->second.second;
      const Coord width = class_width(p.net);
      if (!layer_ok(p.net, l) || obs->segment_state(p.pos, V, l, width, p.net, false, nullptr) != 0) continue;
      int owner = -1;
      for (std::size_t ci = 0; ci < cs.size() && owner < 0; ++ci)
        if (cs[ci].c.pad_a == static_cast<int>(pad) || cs[ci].c.pad_b == static_cast<int>(pad)) owner = static_cast<int>(ci);
      b.tracks.push_back(model::Track{p.pos, V, width, l, p.net, false, sexpr::kNoNode});
      const int id = static_cast<int>(b.tracks.size() - 1);
      hold_item[pad] = obs->add_track(id, owner);
      near_mark(hold_item[pad], +1);
      emit_track_add(id);
      ++res.fanout_holds;
    }
  }
  void drop_hold(int pad) {
    if (pad < 0 || static_cast<std::size_t>(pad) >= hold_item.size() || hold_item[static_cast<std::size_t>(pad)] < 0) return;
    const int item = hold_item[static_cast<std::size_t>(pad)];
    if (opt.sink) emit("{\"type\":\"track_remove\",\"id\":" + std::to_string(obs->copper().items[static_cast<std::size_t>(item)].index) + "}");
    remove_routed(item);
    hold_item[static_cast<std::size_t>(pad)] = -1;
  }
  void drop_holds() {
    for (std::size_t pad = 0; pad < hold_item.size(); ++pad) drop_hold(static_cast<int>(pad));
  }
  void release_pad(int pad, NetId net) {
    drop_hold(pad);
    if (reserve.empty() || pad < 0) return;
    if (opt.sink && !pad_reserved[static_cast<std::size_t>(pad)].empty()) emit("{\"type\":\"escape_release\",\"id\":" + std::to_string(pad) + "}");
    for (std::size_t gi : pad_reserved[static_cast<std::size_t>(pad)])
      if (reserve[gi] == static_cast<std::int32_t>(net)) reserve[gi] = 0;
    pad_reserved[static_cast<std::size_t>(pad)].clear();
    if (!vreserve.empty()) {
      for (std::size_t gi : pad_vreserved[static_cast<std::size_t>(pad)])
        if (vreserve[gi] == static_cast<std::int32_t>(net)) vreserve[gi] = 0;
      pad_vreserved[static_cast<std::size_t>(pad)].clear();
    }
  }
  // A pin whose connections are all routed no longer needs its corridor.
  void release_escapes(int ci) {
    if (reserve.empty()) return;
    const auto& c = cs[static_cast<std::size_t>(ci)].c;
    if (opt.escape_persist && static_cast<std::size_t>(ci) < esc_released.size()) esc_released[static_cast<std::size_t>(ci)] = 1;
    for (int pad : {c.pad_a, c.pad_b}) {
      if (pad < 0) continue;
      auto& open = pad_open[static_cast<std::size_t>(pad)];
      if (open > 0 && --open == 0 && !(opt.escape_persist && net_open_at_package(c.net, pad))) release_pad(pad, c.net);
    }
  }

  RouteResult run() {
    t0 = std::chrono::steady_clock::now();
    rip_cap = opt.max_rips_per_connection;
    budget_scale = (opt.polish_fraction > 0 && opt.polish_fraction < 1) ? 1.0 - opt.polish_fraction : 1.0;
    soft_unit = static_cast<std::int64_t>(opt.soft_cost_mm * 1e6);
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
    if (opt.plan_only) {
      for (const auto& st : cs) {
        const auto& pa = b.pads[static_cast<std::size_t>(st.c.pad_a)];
        RouteResult::Unrouted u;
        u.net = b.nets[static_cast<std::size_t>(st.c.net)].name;
        u.a = b.footprints[static_cast<std::size_t>(pa.footprint)].reference + "." + pa.number;
        u.b = "zone";
        if (st.c.pad_b >= 0) {
          const auto& pb = b.pads[static_cast<std::size_t>(st.c.pad_b)];
          u.b = b.footprints[static_cast<std::size_t>(pb.footprint)].reference + "." + pb.number;
        }
        u.reason = "not_attempted";
        u.pad_a = st.c.pad_a;
        u.pad_b = st.c.pad_b;
        u.pos_a = pa.pos;
        u.pos_b = st.c.pad_b >= 0 ? b.pads[static_cast<std::size_t>(st.c.pad_b)].pos : pa.pos;
        res.unrouted.push_back(std::move(u));
      }
      res.seconds = elapsed();
      res.time_limited = time_stopped;
      return res;
    }
    guide_match();
    layer_assign();
    package_boxes();
    if (opt.pin_via_pen_mm > 0) build_pin_via();
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
    if ((opt.bundle_escape || opt.package_tunnels) && !opt.warm && opt.warm_tracks.empty()) {
      route_bundle_escapes();
      pending.clear();
      for (std::size_t i = 0; i < cs.size(); ++i)
        if (!cs[i].routed) pending.push_back(static_cast<int>(i));
    }
    emit_ratsnest();

    // Best legal state seen (most connections routed): connection -> its tracks/vias.
    int best_routed = -1;
    std::vector<model::Track> best_tracks;
    std::vector<model::Via> best_vias;
    std::vector<std::vector<OldItem>> best_items;  // per connection, for restoring the best state into the live board
    auto snapshot = [&]() {
      best_routed = res.routed;
      best_tracks.clear();
      best_vias.clear();
      best_items.assign(cs.size(), {});
      for (std::size_t ci = 0; ci < cs.size(); ++ci)
        for (int item : cs[ci].items) {
          const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
          best_items[ci].push_back({it.kind, it.index});
        }
      for (const auto& st : cs)
        for (int item : st.items) {
          const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
          if (it.kind == drc::ItemKind::Via) best_vias.push_back(b.vias[static_cast<std::size_t>(it.index)]);
          else best_tracks.push_back(b.tracks[static_cast<std::size_t>(it.index)]);
        }
      res.bus_lanes = res.bus_left = 0;
      for (std::size_t i = 0; i < conn_bundle.size(); ++i)
        if (conn_bundle[i] >= 0 && cs[i].routed && !cs[i].implicit) ++(cs[i].bus_lane ? res.bus_lanes : res.bus_left);
    };

    std::vector<std::uint8_t> best_unrouted;
    std::vector<std::string> best_why;  // reasons as they stood at the best state; later restarts overwrite cs[].why
    std::vector<std::vector<NetId>> best_blockers;
    auto snapshot_unrouted = [&]() {
      best_unrouted.assign(cs.size(), 0);
      best_why.assign(cs.size(), {});
      best_blockers.assign(cs.size(), {});
      for (std::size_t i = 0; i < cs.size(); ++i) {
        best_unrouted[i] = cs[i].routed ? 0 : 1;
        if (!cs[i].routed) best_why[i] = cs[i].why, best_blockers[i] = cs[i].blockers;
      }
    };
    // Restarts that keep the lessons (design doc 06 §3.6): when negotiation stalls with budget left, rip
    // everything and start again, hardest (most failed) connections first, keeping history, nogoods and the
    // best legal state seen so far.
    // After the planned restarts, keep restarting while budget remains (up to 40 more), diversified: nogoods
    // cleared, rip cap raised, history halved, ties in the hardest-first order shuffled (avr_ledprojector stopped
    // at 43 of 120 s with every remaining attempt a nogood).
    if (opt.warm || !opt.warm_tracks.empty()) {  // start from earlier routes: those connections count as routed, the rest are pending
      std::map<std::pair<int, int>, const ConnRoute*> by_pads;
      if (opt.warm)
        for (const auto& cr : *opt.warm) by_pads[{cr.pad_a, cr.pad_b}] = &cr;
      std::vector<ConnRoute> from_board;  // copper of a routed board: all of a net's tracks and vias go to its only connection
      if (!opt.warm_tracks.empty() || !opt.warm_vias.empty()) {
        std::map<NetId, int> per_net;
        for (const auto& c : cs) ++per_net[c.c.net];
        std::map<NetId, std::size_t> slot;
        from_board.reserve(cs.size());
        for (std::size_t ci = 0; ci < cs.size(); ++ci)
          if (per_net[cs[ci].c.net] == 1) {
            slot[cs[ci].c.net] = from_board.size();
            ConnRoute cr;
            cr.pad_a = cs[ci].c.pad_a;
            cr.pad_b = cs[ci].c.pad_b;
            from_board.push_back(std::move(cr));
          }
        for (const auto& t : opt.warm_tracks)
          if (const auto it = slot.find(t.net); it != slot.end()) from_board[it->second].tracks.push_back(t);
        for (const auto& v : opt.warm_vias)
          if (const auto it = slot.find(v.net); it != slot.end()) from_board[it->second].vias.push_back(v);
        for (const auto& cr : from_board)
          if (!cr.tracks.empty()) by_pads[{cr.pad_a, cr.pad_b}] = &cr;
      }
      std::vector<ConnRoute> legalized;  // routes saved under looser rules, moved until the clearances of these rules hold
      if (opt.legalize && !by_pads.empty()) {
        legalized.reserve(by_pads.size());
        for (const auto& [key, cr] : by_pads) legalized.push_back(*cr);
        LegalizeOptions lo;
        lo.outline = obs->outline();
        lo.seed = opt.seed;
        lo.subdivide_mm = opt.legalize_subdivide_mm;
        const LegalizeResult lr = legalize_routes(b, rules, legalized, lo);
        if (std::getenv("TM_DEBUG_LEGALIZE"))
          std::fprintf(stderr, "legalize: %d vertices, %d moved (at most %.3f mm), %d pairs too close before, %d after, %d sweeps\n", lr.nodes, lr.moved,
                       lr.max_move_mm, lr.pairs_before, lr.pairs_after, lr.sweeps_used);
        std::size_t k = 0;
        for (auto& [key, cr] : by_pads) cr = &legalized[k++];
      }
      int taken = 0, dropped = 0;
      // Saved under other (looser) rules: the routes that stay are a largest conflict-free subset of those legal against the fixed
      // copper (greedy vertex cover of the conflict graph: drop the route with most conflicts until none is left), so a tighter rule
      // costs as few routes as possible; each one still passes the strict exact check when it is put in below.
      std::vector<std::uint8_t> drop(cs.size(), 0);
      if (opt.state_recheck) {
        std::vector<std::vector<int>> items_of(cs.size());
        std::vector<std::uint8_t> cand(cs.size(), 0);
        for (std::size_t ci = 0; ci < cs.size(); ++ci) {
          const auto it = cs[ci].routed ? by_pads.end() : by_pads.find({cs[ci].c.pad_a, cs[ci].c.pad_b});
          if (it == by_pads.end()) continue;
          bool legal = true;
          for (const auto& t : it->second->tracks)
            if (!(t.a == t.b) && obs->segment_state(t.a, t.b, t.layer, t.width, t.net, false, nullptr) == 2) legal = false;
          for (const auto& v : it->second->vias)
            if (obs->via_state(v.pos, v.size, v.drill, v.net, 0, false, nullptr) == 2) legal = false;
          if (legal) cand[ci] = 1;
          else drop[ci] = 1;
        }
        for (std::size_t ci = 0; ci < cs.size(); ++ci) {  // all candidates in at once, to see who conflicts with whom
          if (!cand[ci]) continue;
          const auto& wr = *by_pads.find({cs[ci].c.pad_a, cs[ci].c.pad_b})->second;
          for (const auto& t : wr.tracks) {
            b.tracks.push_back(t);
            items_of[ci].push_back(obs->add_track(static_cast<int>(b.tracks.size() - 1), static_cast<int>(ci)));
          }
          for (const auto& v : wr.vias) {
            b.vias.push_back(v);
            items_of[ci].push_back(obs->add_via(static_cast<int>(b.vias.size() - 1), static_cast<int>(ci)));
          }
        }
        std::vector<std::set<int>> adj(cs.size());
        for (std::size_t ci = 0; ci < cs.size(); ++ci) {
          if (!cand[ci]) continue;
          const auto& wr = *by_pads.find({cs[ci].c.pad_a, cs[ci].c.pad_b})->second;
          std::vector<int> owners;
          for (const auto& t : wr.tracks)
            if (!(t.a == t.b)) obs->segment_state(t.a, t.b, t.layer, t.width, t.net, true, &owners);
          for (const auto& v : wr.vias) obs->via_state(v.pos, v.size, v.drill, v.net, 0, true, &owners);
          for (int o : owners) {
            if (o == static_cast<int>(ci)) continue;
            if (o >= 0 && static_cast<std::size_t>(o) < cs.size() && cand[static_cast<std::size_t>(o)]) {
              adj[ci].insert(o);
              adj[static_cast<std::size_t>(o)].insert(static_cast<int>(ci));
            } else {
              drop[ci] = 1;  // conflicts with copper that is not a saved route (already routed here)
            }
          }
        }
        for (std::size_t ci = 0; ci < cs.size(); ++ci)
          if (drop[ci]) {
            for (int o : adj[ci]) adj[static_cast<std::size_t>(o)].erase(static_cast<int>(ci));
            adj[ci].clear();
          }
        for (;;) {
          std::size_t worst = cs.size();
          for (std::size_t ci = 0; ci < cs.size(); ++ci)
            if (!adj[ci].empty() && (worst == cs.size() || adj[ci].size() >= adj[worst].size())) worst = ci;
          if (worst == cs.size()) break;
          drop[worst] = 1;
          for (int o : adj[worst]) adj[static_cast<std::size_t>(o)].erase(static_cast<int>(worst));
          adj[worst].clear();
        }
        for (std::size_t ci = 0; ci < cs.size(); ++ci)
          for (int item : items_of[ci]) obs->remove_item(item);
      }
      for (std::size_t ci = 0; ci < cs.size(); ++ci) {
        if (cs[ci].routed) continue;
        const auto it = by_pads.find({cs[ci].c.pad_a, cs[ci].c.pad_b});
        if (it == by_pads.end()) continue;
        auto& st = cs[ci];
        if (opt.state_recheck) {
          bool legal = !drop[ci];
          for (const auto& t : it->second->tracks)
            if (legal && !(t.a == t.b) && obs->segment_state(t.a, t.b, t.layer, t.width, t.net, false, nullptr) == 2) legal = false;
          for (const auto& v : it->second->vias)
            if (legal && obs->via_state(v.pos, v.size, v.drill, v.net, 0, false, nullptr) == 2) legal = false;
          if (!legal) {
            ++dropped;
            continue;
          }
        }
        ++taken;
        for (const auto& t : it->second->tracks) {
          b.tracks.push_back(t);
          st.items.push_back(obs->add_track(static_cast<int>(b.tracks.size() - 1), static_cast<int>(ci)));
          near_mark(st.items.back(), +1);
        }
        for (const auto& v : it->second->vias) {
          b.vias.push_back(v);
          st.items.push_back(obs->add_via(static_cast<int>(b.vias.size() - 1), static_cast<int>(ci)));
          near_mark(st.items.back(), +1);
        }
        st.routed = true;
        ++res.routed;
        release_escapes(static_cast<int>(ci));
      }
      if (opt.state_recheck) std::fprintf(stderr, "warm start: %d saved routes taken over, %d not legal under these rules\n", taken, dropped);
      if (opt.bundle_escape || opt.package_tunnels) route_bundle_escapes();  // the connections that are still open, as blocks, on the copper taken over
      pending.clear();
      for (std::size_t i = 0; i < cs.size(); ++i)
        if (!cs[i].routed) pending.push_back(static_cast<int>(i));
      snapshot();
      snapshot_unrouted();
    }
    for (int restart = 0; restart <= opt.max_restarts + 40; ++restart) {
    if (restart > 0) {
      phase = WorkPhase::Setup;  // ripping everything, escape reservations and coupled pairs again
      if (out_of_budget() || best_routed == res.connections) break;
      for (std::size_t i = 0; i < cs.size(); ++i) {
        auto& st = cs[i];
        for (int item : st.items) {
          const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
          if (opt.sink) emit(std::string("{\"type\":\"") + (it.kind == drc::ItemKind::Via ? "via_remove" : "track_remove") + "\",\"id\":" + std::to_string(it.index) + "}");
          remove_routed(item);
        }
        st.items.clear();
        st.routed = st.implicit = st.coupled = st.bus_lane = st.bundle_lane = false;
      }
      res.routed = 0;
      if (opt.escape_plan || opt.escape_flow) plan_escape_reservations();  // everything is unrouted again: corridors back
      if (opt.diff_pairs || !opt.pair_nets.empty()) route_diff_pairs();  // pairs first again, coupled
      if (opt.bundle_escape || opt.package_tunnels) route_bundle_escapes();  // and the bundles
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
      // Each bundle together again, lane by lane, where its hardest connection stands.
      if (!bundles.empty()) {
        std::vector<int> grouped;
        std::vector<char> placed(bundles.size(), 0);
        for (int c : order) {
          const int bi = conn_bundle[static_cast<std::size_t>(c)];
          if (bi < 0) {
            grouped.push_back(c);
          } else if (!placed[static_cast<std::size_t>(bi)]) {
            placed[static_cast<std::size_t>(bi)] = 1;
            grouped.insert(grouped.end(), bundles[static_cast<std::size_t>(bi)].conns.begin(), bundles[static_cast<std::size_t>(bi)].conns.end());
          }
        }
        order = std::move(grouped);
        if (opt.bus_first) std::stable_partition(order.begin(), order.end(), [&](int c) { return conn_bundle[static_cast<std::size_t>(c)] >= 0; });
      }
      // --first-nets go first again: a restart rips everything, including them.
      if (crit_order_on)
        std::stable_sort(order.begin(), order.end(), [&](int x, int y) { return order_key(cs[static_cast<std::size_t>(x)].c.net) < order_key(cs[static_cast<std::size_t>(y)].c.net); });
      else if (!first_net_ids.empty()) {
        const auto mid = std::stable_partition(order.begin(), order.end(), [&](int c) { return first_net_ids.count(cs[static_cast<std::size_t>(c)].c.net) > 0; });
        if (opt.first_nets_ordered)
          std::stable_sort(order.begin(), mid, [&](int x, int y) { return first_pos(cs[static_cast<std::size_t>(x)].c.net) < first_pos(cs[static_cast<std::size_t>(y)].c.net); });
      }
      pending.assign(order.begin(), order.end());
      pending.erase(std::remove_if(pending.begin(), pending.end(), [&](int c) { return cs[static_cast<std::size_t>(c)].routed || cs[static_cast<std::size_t>(c)].dead; }),
                    pending.end());
      ++res.restarts;
      emit("{\"type\":\"stage\",\"name\":\"restart " + std::to_string(restart) + "\",\"state\":\"begin\",\"detail\":\"hardest connections first, history kept\"}");
    }
    int run_best = -1, run_best_pass = 0;  // best routed count since this restart began, and the pass that set it
    for (int pass = 0; pass < opt.max_passes && !pending.empty() && !out_of_budget(); ++pass) {
      res.passes = pass + 1;
      strict_pass = pass == 0 && opt.strict_first && !opt.warm;
      if (opt.fanout_hold && !pad_stub.empty()) {
        if (strict_pass) place_holds();
        else drop_holds();
      }
      soft_unit = static_cast<std::int64_t>(opt.soft_cost_mm * 1e6 * (1.0 + opt.soft_growth * std::min(pass, 20)));
      phase = strict_pass ? WorkPhase::FirstPass : WorkPhase::Negotiation;
      std::vector<int> failed;
      const int routed_before = res.routed;
      txn_attempts_pass = 0;
      // --pass-bound (D156): a negotiation pass routes the connections queued when it began; those it rips wait for the next pass, at the
      // next pass's crossing cost. Unbounded, a pass whose successes rip long nets that rip others in turn never ends, so the cost that
      // makes negotiation converge (PathFinder, McMurchie and Ebeling 1995) never rises.
      const std::size_t pass_quota = opt.pass_bound && !strict_pass ? pending.size() : SIZE_MAX;
      std::size_t pass_taken = 0;
      while (!pending.empty() && !out_of_budget() && pass_taken < pass_quota) {
        ++pass_taken;
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
        // A bus lane first tries to run along the neighbour lane routed before it; failing that, the usual searches
        // below run with the neighbour's guide as a preference (doc 05 §38).
        const bool lane = !conn_bundle.empty() && conn_bundle[static_cast<std::size_t>(ci)] >= 0;
        stall_probe.valid = false;  // a probe is of this attempt only
        bool ok = lane && route_bus_lane(ci);
        if (!ok) ok = search_and_commit(st.c, false);
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
          if (!ok && opt.via_shove) {
            ok = via_shove(ci);
            if (!ok) reason += "; via shove failed";
          }
          if (!ok && opt.rescue_in_loop && !opt.rescue_radii_mm.empty()) {
            ok = pin_rescue(ci);
            if (!ok) reason += "; pin rescue failed";
          }
        }
        if (!ok && opt.via_retry && last_miss == Miss::Budget) {
          ok = via_retry(ci);
          if (!ok) reason += "; via retry: " + why;
        }
        if (!ok && opt.rip_up && (pass > 0 || !opt.strict_first || opt.warm)) {
          const bool use_txn = opt.txn > 0 && pair_partner.empty() && conn_bundle.empty();
          if (use_txn) {
            int gain = 0;
            txn_nested_left = opt.txn_nested;
            ok = txn_try(ci, opt.txn_depth, true, gain);
          } else {
            ok = search_and_commit(st.c, true);  // pass 0: strict; later: negotiate
          }
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
        if (!ok && opt.plane_informed && st.c.zone_b >= 0 && !out_of_budget()) {
          ok = plane_fallback(ci);
          if (!ok) reason += "; no nearby pad joined to the plane";
        }
        cs[static_cast<std::size_t>(ci)].why = reason;
        if (lane) bus_note(ci, ok);
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
                 jnum(a.y) + "],\"b\":[" + jnum(e.x) + "," + jnum(e.y) + "],\"blockers\":[],\"blocker_nets\":" + blocker_nets_json(st.blockers) +
                 ",\"region\":[" + jnum(std::min(a.x, e.x) - 1'500'000) + "," + jnum(std::min(a.y, e.y) - 1'500'000) + "," + jnum(std::max(a.x, e.x) + 1'500'000) + "," +
                 jnum(std::max(a.y, e.y) + 1'500'000) + "]" + stall_fields(ci, reason) + "}");
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
      if (std::getenv("TM_DEBUG_LNS")) std::fprintf(stderr, "end of pass %d: routed %d of %d, budget %d\n", pass, res.routed, res.connections, out_of_budget() ? 0 : 1);
      // Regional rebuild of what is still open, from the second pass on and then every second pass (doc 16 F4).
      if (opt.lns_open_trials > 0 && pass >= 1 && pass % 2 == 1 && res.routed < res.connections && !out_of_budget()) {
        const WorkPhase keep = phase;
        phase = WorkPhase::Negotiation;
        const int f = lns_open(opt.lns_open_trials);
        phase = keep;
        res.lns_fixed += f;
        if (std::getenv("TM_DEBUG_LNS")) std::fprintf(stderr, "pass %d lns_open: %d closed, %d of %d routed\n", pass, f, res.routed, res.connections);
      }
      // Next pass: hardest (most failed) first.
      std::stable_sort(failed.begin(), failed.end(), [&](int x, int y) { return cs[static_cast<std::size_t>(x)].fails > cs[static_cast<std::size_t>(y)].fails; });
      for (int f : failed) pending.push_back(f);
      if (opt.tidy > 0 && res.routed < res.connections && pass % opt.tidy == opt.tidy - 1 && !out_of_budget()) {
        const WorkPhase keep_phase = phase;
        phase = WorkPhase::Negotiation;
        soft = false;
        const int t = tidy_sweep();
        phase = keep_phase;
        res.tidied += t;
        if (std::getenv("TM_DEBUG_TXN")) std::fprintf(stderr, "tidy after pass %d: %d connections shortened\n", pass, t);
        // Room may have appeared: the open connections go round again at once.
        for (std::size_t k = 0; k < cs.size(); ++k)
          if (!cs[k].routed && !cs[k].dead && std::find(pending.begin(), pending.end(), static_cast<int>(k)) == pending.end()) pending.push_back(static_cast<int>(k));
      }
      if (pass > 0 && res.routed <= routed_before && res.rips == 0 && txn_attempts_pass == 0) break;
      if (opt.stall_passes > 0) {
        if (res.routed > run_best) run_best = res.routed, run_best_pass = pass;
        if (pass - run_best_pass >= opt.stall_passes) break;
      }
    }
    drop_holds();
    if (res.routed > best_routed) {
      snapshot();
      snapshot_unrouted();
    }
    }  // restarts
    // Polish (doc 16): the reserved share of the budget goes to the best state found, whose open connections are
    // rebuilt one by one with their neighbours (lns_open). The live board is first put back to that state.
    if (opt.polish_fraction > 0 && best_routed >= 0 && best_routed < res.connections && !best_items.empty()) {
      budget_scale = 1.0;
      phase = WorkPhase::Negotiation;
      for (std::size_t ci = 0; ci < cs.size(); ++ci) {
        lift_items(static_cast<int>(ci));
        cs[ci].routed = cs[ci].implicit = cs[ci].coupled = cs[ci].bus_lane = cs[ci].bundle_lane = false;
      }
      for (std::size_t ci = 0; ci < cs.size(); ++ci)
        if (!best_items[ci].empty() || (!best_unrouted.empty() && !best_unrouted[ci])) {
          restore_items(static_cast<int>(ci), best_items[ci]);
          cs[ci].routed = best_unrouted.empty() || !best_unrouted[ci];
        }
      res.routed = 0;
      for (const auto& st : cs) res.routed += st.routed ? 1 : 0;
      for (int round = 0; round < 4 && res.routed < res.connections && !out_of_budget(); ++round) {
        const int f = lns_open(opt.polish_trials);
        if (std::getenv("TM_DEBUG_LNS")) std::fprintf(stderr, "polish round %d: %d closed, %d of %d routed\n", round, f, res.routed, res.connections);
        if (res.routed > best_routed) {
          snapshot();
          snapshot_unrouted();
        }
        if (f == 0) break;
      }
    }
    // End-game retry (D155, doc 05 §63): when the main loop is over, every connection still open that is not proved dead is
    // tried once more, strictly, on the board as it now stands, and then (with --rescue) rescued. On the failing PCBench
    // boards 139 of 349 open connections route alone on the final board: never attempted before the budget ran out, ripped
    // and not routed again, or tried only while their pin was boxed in (bench/records/m22-research). The retry gets an
    // allowance of its own (endgame_frac of the budget, in work units with --work), triggered by the end of the loop and
    // not taken from it, which is what lost for --polish. It starts from the best state, so the clean-up below still runs.
    if (opt.endgame_frac > 0 && best_routed >= 0 && best_routed < res.connections && !best_items.empty() && res.routed != best_routed) {
      // Negotiation usually ends away from its best state: put the board back to it first, as polish does, but lift only the
      // connections whose copper differs (the others keep their pair, bus and bundle flags).
      std::vector<char> differs(cs.size(), 0);
      for (std::size_t ci = 0; ci < cs.size(); ++ci) {
        const auto& st = cs[ci];
        const bool best_ok = best_unrouted.empty() || !best_unrouted[ci];
        bool d = st.routed != best_ok || st.items.size() != best_items[ci].size();
        for (std::size_t k = 0; !d && k < st.items.size(); ++k) {
          const auto& it = obs->copper().items[static_cast<std::size_t>(st.items[k])];
          d = it.kind != best_items[ci][k].kind || it.index != best_items[ci][k].index;
        }
        differs[ci] = d;
      }
      for (std::size_t ci = 0; ci < cs.size(); ++ci)
        if (differs[ci]) {
          lift_items(static_cast<int>(ci));
          cs[ci].routed = cs[ci].implicit = cs[ci].coupled = cs[ci].bus_lane = cs[ci].bundle_lane = false;
        }
      for (std::size_t ci = 0; ci < cs.size(); ++ci)
        if (differs[ci] && (!best_items[ci].empty() || (!best_unrouted.empty() && !best_unrouted[ci]))) {
          restore_items(static_cast<int>(ci), best_items[ci]);
          cs[ci].routed = best_unrouted.empty() || !best_unrouted[ci];
          cs[ci].implicit = cs[ci].routed && best_items[ci].empty();
        }
      res.routed = 0;
      for (const auto& st : cs) res.routed += st.routed ? 1 : 0;
    }
    if (opt.endgame_frac > 0 && best_routed >= 0 && res.routed == best_routed && res.routed < res.connections) {
      const double keep_scale = budget_scale;
      if (opt.work_budget > 0)
        budget_scale = std::max(budget_scale, (static_cast<double>(res.expansions) + opt.endgame_frac * static_cast<double>(opt.work_budget)) / static_cast<double>(opt.work_budget));
      else
        budget_scale = std::max(budget_scale, elapsed() / opt.time_limit_s + opt.endgame_frac);
      phase = WorkPhase::Negotiation;
      const bool keep_strict = strict_pass;
      strict_pass = false;
      soft = false;
      std::vector<int> open;
      for (std::size_t ci = 0; ci < cs.size(); ++ci)
        if (!cs[ci].routed && !cs[ci].implicit && !cs[ci].dead && !cs[ci].coupled && !cs[ci].bus_lane && cs[ci].c.pad_a >= 0) open.push_back(static_cast<int>(ci));
      std::stable_sort(open.begin(), open.end(), [&](int x, int y) { return cs[static_cast<std::size_t>(x)].c.length < cs[static_cast<std::size_t>(y)].c.length; });
      for (int ci : open) {
        if (out_of_budget()) break;
        auto& st = cs[static_cast<std::size_t>(ci)];
        if (st.routed) continue;
        current = ci;
        if (joined(ci)) {
          st.routed = st.implicit = true;
          ++res.routed;
          release_escapes(ci);
          continue;
        }
        ++res.endgame_tried;
        expansion_cap = opt.endgame_cap;
        bool ok = search_and_commit(st.c, false);
        expansion_cap = 0;
        if (!ok && !opt.rescue_radii_mm.empty() && (last_miss == Miss::Enclosed || why.starts_with("boxed in"))) {
          st.rescues = std::min(st.rescues, opt.rescue_per_conn - 1);  // the board around it has changed since its last rescue
          ok = pin_rescue(ci);
        }
        if (!ok && opt.plane_informed && st.c.zone_b >= 0) ok = plane_fallback(ci);
        if (!ok) continue;
        st.routed = true;
        st.why.clear();
        ++res.routed;
        ++res.endgame_fixed;
        release_escapes(ci);
      }
      strict_pass = keep_strict;
      budget_scale = keep_scale;
      if (res.routed > best_routed) {
        snapshot();
        snapshot_unrouted();
      }
    }
    if (opt.deadline && best_routed == res.connections && res.connections > 0) {  // first complete variant sets the deadline
      const double d = 2 * elapsed() + 5;
      double cur = opt.deadline->load();
      while (d < cur && !opt.deadline->compare_exchange_weak(cur, d)) {
      }
    }
    if (opt.keep_final) {  // the state the search ended in (negotiation keeps the board legal at every step)
      res.final_routed = res.routed;
      for (std::size_t ci = 0; ci < cs.size(); ++ci) {
        if (cs[ci].items.empty()) continue;
        ConnRoute cr;
        cr.pad_a = cs[ci].c.pad_a;
        cr.pad_b = cs[ci].c.pad_b;
        for (int item : cs[ci].items) {
          const auto& it = obs->copper().items[static_cast<std::size_t>(item)];
          if (it.kind == drc::ItemKind::Via) cr.vias.push_back(b.vias[static_cast<std::size_t>(it.index)]);
          else cr.tracks.push_back(b.tracks[static_cast<std::size_t>(it.index)]);
        }
        res.final_routes.push_back(std::move(cr));
      }
    }
    // Clean-up only when the live state is a best state (it then stays one: every connection keeps a route).
    // Via-saving re-routes need search budget; smoothing is cheap geometry and always runs (it uses no budget and
    // no randomness, so --work runs stay deterministic).
    phase = WorkPhase::Cleanup;
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
      res.length_tuned = tune_lengths() + tune_skew() + tune_delays();
      if (!opt.buses.empty()) res.length_tuned += tune_bus_skew();
      if (std::getenv("TM_DEBUG_TUNE")) std::fprintf(stderr, "tuned nets: %d\n", res.length_tuned);
      snapshot();
      snapshot_unrouted();
    }
    if (best_unrouted.empty()) snapshot_unrouted();
    res.routed = best_routed;
    for (std::size_t ci = 0; ci < cs.size() && ci < best_items.size(); ++ci) {
      if (best_items[ci].empty()) continue;
      ConnRoute cr;
      cr.pad_a = cs[ci].c.pad_a;
      cr.pad_b = cs[ci].c.pad_b;
      for (const auto& o : best_items[ci]) {
        if (o.kind == drc::ItemKind::Via) cr.vias.push_back(b.vias[static_cast<std::size_t>(o.index)]);
        else cr.tracks.push_back(b.tracks[static_cast<std::size_t>(o.index)]);
      }
      res.conn_routes.push_back(std::move(cr));
    }
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
      RouteResult::Unrouted u;
      u.net = b.nets[static_cast<std::size_t>(st.c.net)].name;
      u.a = b.footprints[static_cast<std::size_t>(pa.footprint)].reference + "." + pa.number;
      u.b = to;
      u.why = best_why[ci];
      u.reason = failure_reason_code(u.why);
      u.rips = st.rips;
      u.fails = st.fails;
      u.pad_a = st.c.pad_a;
      u.pad_b = st.c.pad_b;
      u.pos_a = pa.pos;
      u.pos_b = st.c.pad_b >= 0 ? b.pads[static_cast<std::size_t>(st.c.pad_b)].pos : pa.pos;
      for (const NetId n : best_blockers[ci]) {
        if (u.blockers.size() >= 6) break;
        u.blockers.push_back(b.nets[static_cast<std::size_t>(n)].name);
      }
      res.unrouted.push_back(std::move(u));
    }
    res.seconds = elapsed();
    res.nogood_skips = nogood_skips;
    std::fprintf(stderr, "searches: %ld ok (%ld expansions), %ld failed (%ld expansions); fields %ld GPU + %ld CPU (%.2f s, %ld GPU fallbacks, %ld cache hits)\n",
                 n_ok, exp_ok, n_fail, exp_fail, field_runs, field_cpu_runs, field_seconds, field_gpu_fail, field_hits);
    std::fprintf(stderr, "clean-up: %d connections improved\n", res.optimized);
    std::fprintf(stderr, "restarts %d; legality checks %ld; rips %d, passes %d, boxed-in %d, nogood skips %ld, history cells %zu, transactions %d kept of %d\n", res.restarts, obs->checks, res.rips,
                 res.passes, res.enclosed, nogood_skips, history.size(), res.txn_kept, res.txn_tried);
    if (confined_tried) std::fprintf(stderr, "global corridors: %ld of %ld confined searches committed\n", confined_ok, confined_tried);
    if (reach_checks) std::fprintf(stderr, "reachability checks: %ld, %ld proved unreachable, %ld cells visited%s\n", reach_checks, reach_pruned, reach_visits,
                                  opt.reach_verify ? (reach_mismatch ? ", MISMATCHES" : ", verified (0 mismatches)") : "");
    if (reach_mismatch) std::fprintf(stderr, "reachability mismatches: %ld\n", reach_mismatch);
    if (late_checks) std::fprintf(stderr, "late reachability proofs: %ld, %ld proved no path, %ld cells visited\n", late_checks, late_proved, late_cells);
    if (field_late) std::fprintf(stderr, "fields built during a search: %ld\n", field_late);
    if (res.rescue_tried) std::fprintf(stderr, "pin rescues: %d tried, %d connections routed\n", res.rescue_tried, res.rescued);
    if (res.plane_fallbacks) std::fprintf(stderr, "plane fallbacks: %d pads joined to a nearby pad on the plane\n", res.plane_fallbacks);
    if (res.endgame_tried) std::fprintf(stderr, "end-game retry: %d tried, %d routed\n", res.endgame_tried, res.endgame_fixed);
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
    res.time_limited = time_stopped;
    return std::move(res);
  }
};

// Does the board look like a ball-grid breakout (D98)? Two copper layers, a ball-grid-like package, and at least 70 % of the nets with two
// or more pads having a pad on it. The router decides again, per connection, in package_boxes(); this is the portfolio's early guess.
static bool looks_like_breakout(const model::Board& b) {
  if (b.copper_count() != 2) return false;
  std::map<int, std::vector<int>> by;
  for (std::size_t i = 0; i < b.pads.size(); ++i)
    if (b.pads[i].footprint >= 0 && b.pads[i].type == model::PadType::Smd) by[b.pads[i].footprint].push_back(static_cast<int>(i));
  std::set<int> grid_fp;
  for (const auto& [fp, ids] : by) {
    if (ids.size() < 30) continue;
    const model::LayerMask m = b.pads[static_cast<std::size_t>(ids[0])].copper;
    std::set<Coord> gx, gy;
    bool same = m != 0;
    for (int i : ids) {
      same = same && b.pads[static_cast<std::size_t>(i)].copper == m;
      gx.insert(b.pads[static_cast<std::size_t>(i)].pos.x / 10'000);
      gy.insert(b.pads[static_cast<std::size_t>(i)].pos.y / 10'000);
    }
    if (same && gx.size() >= 5 && gy.size() >= 5 && 10 * ids.size() >= 4 * gx.size() * gy.size()) grid_fp.insert(fp);
  }
  if (grid_fp.empty()) return false;
  std::map<int, std::pair<int, int>> nets;  // net -> (pads, pads on a ball-grid package)
  for (const auto& pd : b.pads) {
    if (pd.net <= 0) continue;
    ++nets[pd.net].first;
    if (grid_fp.count(pd.footprint)) ++nets[pd.net].second;
  }
  int multi = 0, touching = 0;
  for (const auto& [n, c] : nets)
    if (c.first >= 2) {
      ++multi;
      if (c.second > 0) ++touching;
    }
  return multi > 0 && 10 * touching >= 7 * multi;
}

// Saved search state (resume): text lines, "S <rounds done> <routed>", then per connection "R pad_a pad_b ntracks nvias",
// "T ax ay bx by width layer net" and "V x y size drill top bottom type net". Connections are keyed by pad pair, so the state
// applies to the same input board in any later run (RouterOptions::warm matches the same way).
static bool save_state(const std::string& path, const std::vector<ConnRoute>& routes, int rounds_done, int routed) {
  std::ostringstream o;
  o << "S " << rounds_done << ' ' << routed << '\n';
  for (const auto& cr : routes) {
    o << "R " << cr.pad_a << ' ' << cr.pad_b << ' ' << cr.tracks.size() << ' ' << cr.vias.size() << '\n';
    for (const auto& t : cr.tracks)
      o << "T " << t.a.x << ' ' << t.a.y << ' ' << t.b.x << ' ' << t.b.y << ' ' << t.width << ' ' << t.layer << ' ' << t.net << '\n';
    for (const auto& v : cr.vias)
      o << "V " << v.pos.x << ' ' << v.pos.y << ' ' << v.size << ' ' << v.drill << ' ' << v.layer_top << ' ' << v.layer_bottom << ' '
        << static_cast<int>(v.type) << ' ' << v.net << '\n';
  }
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << o.str();
    if (!f) return false;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

static bool load_state(const std::string& path, std::vector<ConnRoute>& routes, int& rounds_done, int& routed) {
  std::ifstream f(path);
  if (!f) return false;
  std::string tag;
  routes.clear();
  while (f >> tag) {
    if (tag == "S") {
      f >> rounds_done >> routed;
    } else if (tag == "R") {
      ConnRoute cr;
      std::size_t nt = 0, nv = 0;
      f >> cr.pad_a >> cr.pad_b >> nt >> nv;
      for (std::size_t i = 0; i < nt && f >> tag; ++i) {
        model::Track t;
        f >> t.a.x >> t.a.y >> t.b.x >> t.b.y >> t.width >> t.layer >> t.net;
        cr.tracks.push_back(t);
      }
      for (std::size_t i = 0; i < nv && f >> tag; ++i) {
        model::Via v;
        int ty = 0;
        f >> v.pos.x >> v.pos.y >> v.size >> v.drill >> v.layer_top >> v.layer_bottom >> ty >> v.net;
        v.type = static_cast<model::ViaType>(ty);
        cr.vias.push_back(v);
      }
      routes.push_back(std::move(cr));
    } else {
      return false;
    }
  }
  return !routes.empty();
}

PortfolioResult route_portfolio(const model::Board& board, const model::DesignRules& rules, const RouterOptions& base_in, int variants,
                                const std::vector<int>& pick, int threads) {
  RouterOptions base = base_in;
  if (base.relax) {  // relax, route, legalise (doc 05 §41.3): the stages and the breathing are defaults of one switch
    const double c_board = nm_to_mm(std::max(rules.classes.empty() ? Coord{0} : rules.classes.front().clearance, rules.minimums.clearance));
    if (base.ramp_from_mm <= 0) base.ramp_from_mm = 0.6 * c_board;
    if (base.ramp_step_mm == RouterOptions{}.ramp_step_mm) base.ramp_step_mm = 0.2 * c_board;
    if (base.round_work <= 0) base.round_work = 20'000'000;
    if (base.rounds <= 0) base.rounds = 200;
    base.legalize = true;
    if (base.relax_late) base.ramp_late = true;
    if (base.ramp_cycle <= 0) base.ramp_cycle = 2;
    if (base.ramp_fresh <= 0) base.ramp_fresh = 3;
    if (base.ramp_stall == RouterOptions{}.ramp_stall) base.ramp_stall = 2;
    if (base.ramp_rounds == RouterOptions{}.ramp_rounds) base.ramp_rounds = 3;
  }
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
  const bool breakout_list = variants > 1 && base.breakout_portfolio && looks_like_breakout(board);
  // With advice the list is built that much longer: the advised variants are the next ones of the list (doc 17 §4), the very variants
  // a plain run with that many more variants would run, so what advice adds is the hints and not a different draw of seeds.
  const int advised_n = base.advice.active() ? std::clamp(base.advice.variants, 1, 8) : 0;
  const int list_n = std::min(variants + advised_n, 64);
  if (breakout_list) {
    // Breakout portfolio (D101): every variant has fast bends and the breakout profile (transactions etc.); variant 0 is the plain
    // profile, the others draw order (shortest first, deep pins first, jittered), via cost, crossing cost, direction strength and
    // package penalty from sets with a counter-based generator, so the list is the same on every run.
    add("breakout profile", [&](RouterOptions& o) { o.bend_states = false; });
    for (int k = 1; static_cast<int>(vs.size()) < list_n; ++k) {
      const RngStream rg(base.seed, 0xB0B0u, static_cast<std::uint64_t>(k));
      auto pick_of = [&](std::uint64_t slot, auto... v) {
        const std::array<double, sizeof...(v)> a{static_cast<double>(v)...};
        return a[static_cast<std::size_t>(rg.uniform(slot) * static_cast<double>(a.size())) % a.size()];
      };
      add("breakout seeded " + std::to_string(k), [&](RouterOptions& o) {
        o.bend_states = false;
        o.order = static_cast<int>(pick_of(0, 0, 3, 2));
        o.seed = base.seed + 100 + static_cast<std::uint64_t>(k);
        o.via_cost_mm = base.via_cost_mm * pick_of(1, 0.5, 1.0, 1.0, 1.5);
        o.soft_cost_mm = base.soft_cost_mm * pick_of(2, 1.0, 1.0, 2.0);
        o.layer_dir_pen = -pick_of(3, 0.3, 0.3, 0.6, 1.01);
        o.package_pen = pick_of(4, 3.0, 3.0, 0.0) > 0 ? -1.0 : 0.0;
        o.txn_temp = pick_of(5, 1.0, 1.5, 3.0);
        o.layer_dir_swap = k % 7 == 6;
      });
    }
  } else {
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
  // (package_pen below: tracks inside ball-grid packages cost more, doc 16 F3; +4 boards of 57 at 3 pitches on srj20)
  // Beyond the designed eight: seeded diversifications (fast bends, jittered order; via cost, crossing cost and
  // escape planning cycle with the index).
  for (int k = 0; static_cast<int>(vs.size()) < list_n; ++k) {
    static const double kVia[3] = {0.4, 1.0, 2.5}, kSoft[3] = {0.5, 1.0, 2.0};
    add("seeded " + std::to_string(k), [&](RouterOptions& o) {
      o.bend_states = false;
      o.order = k % 4 == 3 ? 0 : 2;
      o.seed = base.seed + 10 + static_cast<std::uint64_t>(k);
      o.via_cost_mm = base.via_cost_mm * kVia[k % 3];
      o.soft_cost_mm = base.soft_cost_mm * kSoft[(k / 3) % 3];
      o.escape_plan = k % 2 == 0;
      if (k % 2 == 1) o.package_pen = 3.0;
      static const double kDir[3] = {-0.3, -0.6, -1.01};  // negative: automatic (breakout boards only) at that strength (doc 16 §11)
      o.layer_dir_pen = kDir[(k / 2) % 3];
      o.layer_dir_swap = k % 5 == 4;
    });
  }
    if (base.escape_all)  // --escape-all (D156, M22 item 6): the escape plan in every variant, not only in two of the designed eight
      for (auto& v : vs) v.o.escape_plan = true;
    // Strict searches from the crowded end (doc 05 §55, D143) in half the designed eight: on in every variant it routed 1,383 -> 1,204
    // of the 33 failing tier B-D boards but lost two of the clean ones; in these four (chosen on those 71 boards) it loses none.
    if (base.crowded_portfolio)
      for (const std::size_t i : {1u, 5u, 6u, 7u})
        if (i < vs.size() && !vs[i].o.crowded_source) {
          vs[i].o.crowded_source = true;
          vs[i].name += ", crowded source";
        }
  }  // designed variants
  std::vector<int> chosen;
  if (!pick.empty()) {
    for (int i : pick)
      if (i >= 0 && i < static_cast<int>(vs.size())) chosen.push_back(i);
  } else {
    for (int i = 0; i < std::clamp(variants, 1, static_cast<int>(vs.size())); ++i) chosen.push_back(i);
  }
  std::vector<Variant> advised_src;  // the variants that carry the advice
  {
    std::vector<Variant> sel;
    for (std::size_t k = 0; k < chosen.size(); ++k) {
      sel.push_back(vs[static_cast<std::size_t>(chosen[k])]);
      if (k > 0) sel.back().o.sink = nullptr;
      else sel.back().o.sink = base.sink;
    }
    if (advised_n > 0) {
      std::vector<char> taken(vs.size(), 0);
      for (int c : chosen) taken[static_cast<std::size_t>(c)] = 1;
      for (std::size_t i = 0; i < vs.size() && static_cast<int>(advised_src.size()) < advised_n; ++i)
        if (!taken[i]) advised_src.push_back(vs[i]);
      // The list had no more variants to give: advise copies of the first ones that run.
      for (std::size_t j = 0; static_cast<int>(advised_src.size()) < advised_n && j < sel.size(); ++j) advised_src.push_back(sel[j]);
    }
    vs = std::move(sel);
  }
  // Advice (doc 17): extra variants that carry the hints. They come after all the others and their indices start at kAdvisedIndex, so
  // on a tie in the total order the unadvised variant wins: advice can only win by routing more, or the same with fewer vias or
  // shorter copper (rule 4). Without advice nothing here runs, and the portfolio is what it was.
  if (base.advice.active()) {
    const auto& a = base.advice;
    for (std::size_t j = 0; j < advised_src.size(); ++j) {
      Variant v = advised_src[j];
      RouterOptions& o = v.o;
      o.sink = nullptr;
      o.priority.insert(o.priority.end(), a.priority.begin(), a.priority.end());
      for (const auto& f : a.first_nets)
        if (std::find(o.first_nets.begin(), o.first_nets.end(), f) == o.first_nets.end()) o.first_nets.push_back(f);
      o.no_track_layers |= a.no_track_layers;
      if (o.layer_cost_pm.size() < a.layer_cost_pm.size()) o.layer_cost_pm.resize(a.layer_cost_pm.size(), 1000);
      for (std::size_t l = 0; l < a.layer_cost_pm.size(); ++l) o.layer_cost_pm[l] = std::max(o.layer_cost_pm[l], a.layer_cost_pm[l]);
      o.extra_zones.insert(o.extra_zones.end(), a.zones.begin(), a.zones.end());
      o.extra_rules.insert(o.extra_rules.end(), a.rules.begin(), a.rules.end());
      v.name += " + advice";
      vs.push_back(std::move(v));
      chosen.push_back(kAdvisedIndex + static_cast<int>(j));
    }
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
    std::vector<double> spent(vs.size(), 0.0);     // wall time per variant, summed over its rungs
    std::vector<WorkProfile> work(vs.size());      // each variant's work, summed over the rungs it ran (a rung starts afresh)
    auto better_r = [&](const RouteResult& a, std::size_t ia, const RouteResult& c, std::size_t ic) {
      if (a.routed != c.routed) return a.routed > c.routed;
      if (a.bus_left != c.bus_left) return a.bus_left < c.bus_left;
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
        spent[alive[k]] += out[k].seconds;
        for (int p = 0; p < kWorkPhases; ++p) work[alive[k]].phase[static_cast<std::size_t>(p)] += out[k].work.phase[static_cast<std::size_t>(p)];
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
      pr.seconds.push_back(spent[i]);  // the job log and summary read one entry per variant
      pr.work.push_back(work[i]);
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
  // Total order on (routed desc, vias asc, length asc, variant index asc): the winner depends neither on the order
  // in which variants finished nor on their positions in `pick`.
  auto better = [&](std::size_t i, std::size_t j) {
    const auto& a = rs[i];
    const auto& c = rs[j];
    if (a.routed != c.routed) return a.routed > c.routed;
    if (a.bus_left != c.bus_left) return a.bus_left < c.bus_left;  // buses: fewer connections off their lanes (0 without buses)
    if (a.vias.size() != c.vias.size()) return a.vias.size() < c.vias.size();
    const double la = length(a), lc = length(c);
    if (la != lc) return la < lc;
    return chosen[i] < chosen[j];
  };
  // Rounds (doc 16 §12): the budget is split; after each round every variant continues from the best result so far, so the
  // searches of all variants work on the most promising state instead of on their own. Selection is by `better`, so the
  // outcome does not depend on thread timing.
  const int rounds = base.rounds > 0 ? base.rounds : (breakout_list ? 4 : 1);
  std::vector<ConnRoute> warm_routes;
  int resumed_rounds = 0, saved_routed = -1;
  bool resumed = false;
  if (!base.state_in.empty() && load_state(base.state_in, warm_routes, resumed_rounds, saved_routed)) resumed = true;
  if (base.round_work > 0)
    for (auto& v : vs) v.o.work_budget = base.round_work;
  else if (rounds > 1)
    for (auto& v : vs) {
      if (base.work_budget > 0) v.o.work_budget = std::max<long>(1, base.work_budget / rounds);
      else v.o.time_limit_s = base.time_limit_s / rounds;
    }
  // Clearance continuation (doc 16 §12.3): stage k routes under rules whose clearances are lowered by (board clearance - c_k)
  // for c_0 = ramp_from_mm, c_1 = c_0 + step, ... below the board's own; the stages hand their routes on, only those legal
  // under the next stage's rules taken over (state_recheck). The last stage always runs under the board's rules, so what
  // comes out is legal, and rounds after it continue as usual.
  const bool islands = base.island_slack >= 0 && base.round_work > 0;
  std::vector<std::vector<ConnRoute>> own_routes(vs.size());  // islands: a variant's own final state from the last round
  if (islands && base.ramp_from_mm <= 0)
    for (auto& v : vs) v.o.keep_final = true;
  std::vector<model::DesignRules> ramp_rules;
  if (base.ramp_from_mm > 0 && base.ramp_step_mm > 0 && base.round_work > 0) {
    const Coord c_board = std::max(rules.classes.empty() ? Coord{0} : rules.classes.front().clearance, rules.minimums.clearance);
    const Coord step = std::max<Coord>(mm_to_nm(base.ramp_step_mm), 1'000);
    for (Coord c = mm_to_nm(base.ramp_from_mm); c < c_board; c += step) {
      model::DesignRules rr = rules;
      const Coord delta = c_board - c;
      for (auto& cl : rr.classes) cl.clearance = std::max<Coord>(cl.clearance - delta, 0);
      rr.minimums.clearance = std::max<Coord>(rr.minimums.clearance - delta, 0);
      ramp_rules.push_back(std::move(rr));
    }
    for (auto& v : vs) v.o.state_recheck = true;
  }
  std::size_t stage = base.ramp_late ? ramp_rules.size() : 0;  // late: the board's own rules first
  int rounds_at_stage = 0, stage_best = -1;
  int cycles = 0;                        // breathing cycles started
  bool fresh_start = false;              // the next round starts from nothing (relax_late)
  bool ramp_started = !base.ramp_late;  // late ramp: the plain rounds have not yet stalled
  bool last_relaxed = !ramp_rules.empty() && !base.ramp_late;  // the last round ran under lowered clearances
  RouteResult best_final;                   // best result under the board's rules seen so far (ramp)
  int best_final_routed = -1, final_stall = 0;
  const auto t_start = std::chrono::steady_clock::now();
  bool rounds_timed_out = false;  // the time limit ended the rounds (reported as RouteResult::time_limited)
  for (int round = 0; round < rounds; ++round) {
    if (round > 0 && base.round_work > 0 &&
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count() > base.time_limit_s) {
      rounds_timed_out = true;
      break;
    }
    if (round > 0 || resumed)
      for (std::size_t vi = 0; vi < vs.size(); ++vi) {
        auto& v = vs[vi];
        if (base.round_work > 0) v.o.seed += 7919u * static_cast<unsigned>(round > 0 ? 1 : resumed_rounds + 1);  // a new draw each round (a resumed run continues the sequence)
        v.o.warm = islands && !own_routes[vi].empty() ? &own_routes[vi] : fresh_start ? nullptr : &warm_routes;
        v.o.warm_tracks.clear();  // copper of --warm-pcb is only the first round's start; later rounds continue from the best state
        v.o.warm_vias.clear();
        v.o.sink = nullptr;
        v.o.deadline = nullptr;
      }
    const model::DesignRules& round_rules = stage < ramp_rules.size() ? ramp_rules[stage] : rules;
    last_relaxed = stage < ramp_rules.size();
    std::atomic<std::size_t> next{0};
    pool.clear();
    for (std::size_t w = 0; w < workers; ++w)
      pool.emplace_back([&] {
        for (std::size_t i; (i = next.fetch_add(1)) < vs.size();) rs[i] = Router(board, round_rules, vs[i].o).run();
      });
    for (auto& t : pool) t.join();
    fresh_start = false;
    {
      std::size_t bi = 0;
      for (std::size_t i = 1; i < rs.size(); ++i)
        if (better(i, bi)) bi = i;
      // Save the best state of this round unless it is worse than the one this run started from.
      // (A relaxed ramp stage always saves: its counts are not comparable with those under the board's rules.)
      const bool relaxed = stage < ramp_rules.size();
      if (!base.state_out.empty() && (relaxed || rs[bi].routed >= saved_routed)) {
        if (save_state(base.state_out, rs[bi].conn_routes, resumed_rounds + round + 1, rs[bi].routed)) saved_routed = relaxed ? -1 : rs[bi].routed;
      }
      const bool complete = rs[bi].connections > 0 && rs[bi].routed == rs[bi].connections;
      if (stage < ramp_rules.size()) {  // relaxed stage: done when complete or after ramp_rounds rounds, then the next one
        std::fprintf(stderr, "ramp stage %zu/%zu (clearance -%.4f mm): %d of %d routed%s\n", stage + 1, ramp_rules.size(),
                     nm_to_mm(std::max(rules.classes.front().clearance, rules.minimums.clearance) -
                              std::max(ramp_rules[stage].classes.front().clearance, ramp_rules[stage].minimums.clearance)),
                     rs[bi].routed, rs[bi].connections, complete ? " (complete)" : "");
        // A stage ends when complete, or after ramp_rounds rounds in a row that did not route more than the best round of the stage.
        if (rs[bi].routed > stage_best) {
          stage_best = rs[bi].routed;
          rounds_at_stage = 0;
        } else {
          ++rounds_at_stage;
        }
        if (complete || rounds_at_stage >= std::max(base.ramp_rounds, 1)) {
          ++stage;
          rounds_at_stage = 0;
          stage_best = -1;
        }
        warm_routes = rs[bi].conn_routes;
        continue;
      }
      if (islands) {  // each variant keeps its own state if it is close enough to the best, else it starts from the best again
        std::string fin;
        for (std::size_t vi = 0; vi < vs.size(); ++vi) fin += " " + std::to_string(ramp_rules.empty() ? rs[vi].final_routed : rs[vi].routed);
        std::fprintf(stderr, "island round %d: best %d of %d, %s states:%s\n", round + 1, rs[bi].routed, rs[bi].connections,
                     ramp_rules.empty() ? "final" : "best", fin.c_str());
        // With a ramp every variant's best state of the stage is its own (16 different solutions of the stage, a beam); without one the
        // state the search ended in, which wanders across the plateau.
        const bool use_best = !ramp_rules.empty();
        for (std::size_t vi = 0; vi < vs.size(); ++vi) {
          const int have = use_best ? rs[vi].routed : rs[vi].final_routed;
          auto& src = use_best ? rs[vi].conn_routes : rs[vi].final_routes;
          if (have + base.island_slack >= rs[bi].routed && !src.empty()) own_routes[vi] = src;
          else own_routes[vi].clear();
        }
      }
      if (!ramp_rules.empty() || islands) {
        if (rs[bi].routed > best_final_routed) {
          best_final = rs[bi];
          best_final_routed = rs[bi].routed;
          final_stall = 0;
        } else {
          ++final_stall;
        }
        if (!ramp_rules.empty() && !complete && base.ramp_cycle > 0 && final_stall >= std::max(base.ramp_stall, 1)) {  // breathe: loosen again from the best state
          const std::size_t back = std::min<std::size_t>(static_cast<std::size_t>(base.ramp_cycle), ramp_rules.size());
          stage = ramp_rules.size() - back;
          rounds_at_stage = 0;
          stage_best = -1;
          final_stall = 0;
          ++cycles;
          if ((base.relax_late && !ramp_started) || (base.ramp_fresh > 0 && cycles % base.ramp_fresh == 0)) {  // stuck in a basin: the stages start from nothing
            stage = 0;
            warm_routes.clear();
            fresh_start = true;
          } else {
            warm_routes = best_final.conn_routes;
          }
          ramp_started = true;
          std::fprintf(stderr, "ramp: %d of %d routed under the board's rules, back to stage %zu/%zu\n", best_final_routed, rs[bi].connections, stage + 1,
                       ramp_rules.size());
          continue;
        }
      }
      if (complete) break;  // complete under the board's rules: nothing left to improve
      if (round + 1 < rounds) warm_routes = islands ? best_final.conn_routes : rs[bi].conn_routes;
    }
  }
  if (!ramp_rules.empty() && last_relaxed) {
    // Out of time or rounds while relaxed: one round with next to no budget under the board's rules keeps what is legal.
    for (auto& v : vs) {
      v.o.warm = &warm_routes;
      v.o.sink = nullptr;
      v.o.deadline = nullptr;
      v.o.work_budget = 1;
      v.o.time_limit_s = 0;
    }
    std::atomic<std::size_t> next{0};
    pool.clear();
    for (std::size_t w = 0; w < workers; ++w)
      pool.emplace_back([&] {
        for (std::size_t i; (i = next.fetch_add(1)) < vs.size();) rs[i] = Router(board, rules, vs[i].o).run();
      });
    for (auto& t : pool) t.join();
  }
  PortfolioResult pr;
  std::size_t best = 0;
  pr.indices = chosen;
  for (std::size_t i = 0; i < rs.size(); ++i) {
    pr.variants.push_back(vs[i].name);
    pr.routed.push_back(rs[i].routed);
    pr.seconds.push_back(rs[i].seconds);
    pr.work.push_back(rs[i].work);
    if (i > 0 && better(i, best)) best = i;
  }
  pr.best_variant = static_cast<int>(best);
  if (!bufs.empty())
    for (auto& m : bufs[best]->msgs) base.sink->publish(std::move(m));
  // Any variant stopped by time, not work, could have beaten the winner on a faster machine: the result depends on machine speed.
  bool timed = rounds_timed_out;
  for (const auto& r : rs) timed = timed || r.time_limited;
  pr.best = std::move(rs[best]);
  if (best_final_routed > pr.best.routed) pr.best = std::move(best_final);  // breathing ended relaxed or worse: the best legal one
  pr.best.time_limited = timed;
  return pr;
}

int portfolio_size() { return 8; }

bool route_result_better(const RouteResult& a, const RouteResult& b) {
  if (a.routed != b.routed) return a.routed > b.routed;
  if (a.bus_left != b.bus_left) return a.bus_left < b.bus_left;
  if (a.vias.size() != b.vias.size()) return a.vias.size() < b.vias.size();
  auto length = [](const RouteResult& r) {
    double l = 0;
    for (const auto& t : r.tracks) l += std::hypot(static_cast<double>(t.b.x - t.a.x), static_cast<double>(t.b.y - t.a.y));
    return l;
  };
  return length(a) < length(b);
}

WorkCounts& WorkCounts::operator+=(const WorkCounts& o) {
  expansions += o.expansions;
  flood_cells += o.flood_cells;
  searches += o.searches;
  cell_checks += o.cell_checks;
  fixed_checks += o.fixed_checks;
  field_cells += o.field_cells;
  fields += o.fields;
  commits += o.commits;
  rips += o.rips;
  return *this;
}

WorkCounts WorkProfile::total() const {
  WorkCounts t;
  for (const auto& p : phase) t += p;
  return t;
}

const char* work_phase_name(WorkPhase p) {
  switch (p) {
    case WorkPhase::Setup: return "setup";
    case WorkPhase::FirstPass: return "first_pass";
    case WorkPhase::Negotiation: return "negotiation";
    case WorkPhase::Cleanup: return "cleanup";
  }
  return "?";
}

Router::Router(const model::Board& board, const model::DesignRules& rules, RouterOptions opt) : in_(board), rules_(rules), opt_(opt) {}

RouteResult Router::run() {
  if (!opt_.extra_rules.empty()) {  // advice regions: the rules of this router only, the caller's stay as they are
    model::DesignRules r = rules_;
    r.custom.insert(r.custom.end(), opt_.extra_rules.begin(), opt_.extra_rules.end());
    Impl impl(in_, r, opt_);
    return impl.run();
  }
  Impl impl(in_, rules_, opt_);
  return impl.run();
}

}  // namespace tmk::route

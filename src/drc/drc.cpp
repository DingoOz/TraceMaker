// SPDX-License-Identifier: GPL-3.0-or-later
#include "drc/drc.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <set>
#include <tuple>
#include <nlohmann/json.hpp>

#include "drc/connectivity.hpp"
#include "drc/copper.hpp"
#include "drc/rule_engine.hpp"
#include "index/uniform_grid.hpp"

namespace tmk::drc {

std::map<std::string, int> DrcReport::counts() const {
  std::map<std::string, int> c;
  for (const auto& v : violations) ++c[v.type];
  if (!unconnected.empty()) c["unconnected_items"] = static_cast<int>(unconnected.size());
  return c;
}

namespace {


class Checker {
 public:
  Checker(const model::Board& b, const model::DesignRules& r, const DrcOptions& o)
      : b_(b), r_(r), o_(o), cm_(build_copper(b)), re_(b, r, cm_) {
    re_.use_zone_clearance_overrides();
  }

  DrcReport run() {
    rep_.warnings = r_.warnings;
    for (const auto& w : re_.warnings()) rep_.warnings.push_back(w);
    // TM_DEBUG_TIMING: seconds per phase on stderr (where a large board's time goes; doc 07 §DRC broad-phase).
    const bool timing = std::getenv("TM_DEBUG_TIMING") != nullptr;
    auto t0 = std::chrono::steady_clock::now();
    auto tick = [&](const char* phase) {
      if (!timing) return;
      const auto t1 = std::chrono::steady_clock::now();
      std::fprintf(stderr, "drc %-22s %8.3f s\n", phase, std::chrono::duration<double>(t1 - t0).count());
      t0 = t1;
    };
    if (timing) std::fprintf(stderr, "drc items %zu, holes %zu\n", cm_.items.size(), cm_.holes.size());
    build_grid();
    tick("grid");
    graph_ = item_graph(b_, cm_, *grid_);
    tick("item graph");
    zones_ = std::make_unique<ZoneFills>(cm_);
    tick("zone fills index");
    if (o_.propagate_nets) {
      // KiCad's DRC sees the board after net propagation: a stray via or track on another net's copper has
      // taken that net, so it is not a short (verified with scripts/drc_broken_parity.py).
      const auto nets = propagate_nets(cm_, graph_, *zones_, *grid_);
      for (std::size_t i = 0; i < nets.size(); ++i) cm_.items[i].net = nets[i];
      for (auto& h : cm_.holes)
        if (h.item >= 0) h.net = cm_.items[static_cast<std::size_t>(h.item)].net;
    }
    // A via written without a drill (old boards) has its net class's via drill (PCB_VIA::GetDrillValue).
    via_drill_.assign(b_.vias.size(), 0);
    for (auto& h : cm_.holes)
      if (h.via >= 0 && h.item >= 0 && b_.vias[static_cast<std::size_t>(h.via)].drill <= 0) {
        const auto& nc = re_.netclass(cm_.items[static_cast<std::size_t>(h.item)]);
        const Coord d = b_.vias[static_cast<std::size_t>(h.via)].type == model::ViaType::Micro ? nc.uvia_drill : nc.via_drill;
        via_drill_[static_cast<std::size_t>(h.via)] = d;
        h.shape = geom::Shape::point(h.pos, d / 2);
      }
    tick("net propagation");
    check_pairs();
    tick("check_pairs");
    check_items();
    tick("check_items");
    check_pad_rings();
    tick("check_pad_rings");
    check_holes();
    tick("check_holes");
    check_via_hole_pairs();
    tick("check_via_hole_pairs");
    check_edges();
    tick("check_edges");
    check_keepouts();
    tick("check_keepouts");
    check_connectivity();
    tick("check_connectivity");
    // Project severities: drop ignored types, apply warning/error levels.
    auto sev = [&](const std::string& t) -> const std::string* {
      const auto it = r_.severities.find(t);
      return it == r_.severities.end() ? nullptr : &it->second;
    };
    std::erase_if(rep_.violations, [&](const Violation& v) { const auto* s = sev(v.type); return s && *s == "ignore"; });
    for (auto& v : rep_.violations)
      if (const auto* s = sev(v.type)) v.severity = *s;
    if (const auto* s = sev("unconnected_items"); s && *s == "ignore") rep_.unconnected.clear();
    std::stable_sort(rep_.violations.begin(), rep_.violations.end(), [](const Violation& a, const Violation& c) { return a.type < c.type; });
    return std::move(rep_);
  }

 private:
  std::string describe(const CopperItem& it) const {
    const std::string net = it.net ? " [" + b_.nets[static_cast<std::size_t>(it.net)].name + "]" : "";
    switch (it.kind) {
      case ItemKind::Pad: {
        const auto& p = b_.pads[static_cast<std::size_t>(it.index)];
        return "Pad " + p.number + " of " + b_.footprints[static_cast<std::size_t>(p.footprint)].reference + net;
      }
      case ItemKind::Track: return "Track" + net + " on " + b_.copper_name(b_.tracks[static_cast<std::size_t>(it.index)].layer);
      case ItemKind::Arc: return "Arc" + net;
      case ItemKind::Via: return "Via" + net;
      case ItemKind::Zone: return "Zone" + net;
      case ItemKind::Graphic: return "Copper graphic";
    }
    return "?";
  }
  void add(std::string type, const CopperItem* a, const CopperItem* c, Coord actual, Coord required, int layer, std::string sev = "error") {
    Violation v;
    v.type = std::move(type);
    v.severity = std::move(sev);
    v.actual = actual;
    v.required = required;
    v.layer = layer;
    if (a) v.items.push_back({describe(*a), a->pos});
    if (c) v.items.push_back({describe(*c), c->pos});
    rep_.violations.push_back(std::move(v));
  }

  void build_grid() {
    geom::Box all;
    for (const auto& it : cm_.items) all.add(it.box);
    for (const auto& e : cm_.edges) all.add(e.box);
    if (all.empty()) all = geom::Box{0, 0, 1, 1};
    reach_ = re_.max_clearance() + 1;
    all = all.inflated(reach_);
    const Coord cell = std::max<Coord>(1'000'000, 2 * reach_);
    grid_ = std::make_unique<index::UniformGrid>(all, cell, cm_.items.size());
    for (std::size_t i = 0; i < cm_.items.size(); ++i) grid_->insert(static_cast<int>(i), cm_.items[i].box);
  }

  static bool shapes_closer(const CopperItem& a, const CopperItem& c, Coord t) {
    for (const auto& s : a.shapes)
      for (const auto& u : c.shapes)
        if (geom::closer_than(s, u, t)) return true;
    return false;
  }
  static double shapes_gap(const CopperItem& a, const CopperItem& c) {
    double g = 1e30;
    for (const auto& s : a.shapes)
      for (const auto& u : c.shapes) g = std::min(g, geom::gap(s, u));
    return g;
  }

  // Pair tests that go through a zone fill's edge index when one side is a fill: the same answers as
  // shapes_closer / shapes_gap (geom::PolygonIndex is tested against them), at the cost of the nearby edges
  // instead of every edge of the fill. On vme-wren the linear tests took 395 of the DRC's 477 s.
  const geom::PolygonIndex* fill_index(const CopperItem& z) const {
    if (z.kind != ItemKind::Zone || o_.linear_zone_tests) return nullptr;
    const int slot = zones_->slot[static_cast<std::size_t>(&z - cm_.items.data())];
    return slot < 0 ? nullptr : &zones_->index[static_cast<std::size_t>(slot)];
  }
  bool pair_closer(const CopperItem& a, const CopperItem& c, Coord t) const {
    const geom::PolygonIndex* za = fill_index(a);
    const geom::PolygonIndex* zc = za ? nullptr : fill_index(c);
    if (!za && !zc) return shapes_closer(a, c, t);
    for (const auto& s : (za ? c : a).shapes)
      if ((za ? za : zc)->shape_closer(s, t)) return true;
    return false;
  }
  // Gap of a pair known to be closer than t (pair_closer(a, c, t) holds).
  double pair_gap(const CopperItem& a, const CopperItem& c, Coord t) const {
    const geom::PolygonIndex* za = fill_index(a);
    const geom::PolygonIndex* zc = za ? nullptr : fill_index(c);
    if (!za && !zc) return shapes_gap(a, c);
    double g = 1e30;
    // Shapes of the item farther than t from the fill cannot hold the minimum; the bound keeps their search local.
    for (const auto& s : (za ? c : a).shapes) g = std::min(g, (za ? za : zc)->shape_gap(s, std::max<Coord>(t, 0) + s.r + 1));
    return g;
  }

  void check_pairs() {
    const auto n = cm_.items.size();
    for (std::size_t i = 0; i < n; ++i) {
      const CopperItem& a = cm_.items[i];
      grid_->query(a.box.inflated(reach_), [&](int j) {
        if (static_cast<std::size_t>(j) <= i) return;
        const CopperItem& c = cm_.items[static_cast<std::size_t>(j)];
        const model::LayerMask common = a.layers & c.layers;
        if (!common) return;
        if (a.net == c.net && a.net != 0) return;
        if (a.kind == ItemKind::Zone && c.kind == ItemKind::Zone) return;
        const auto track_like = [](const CopperItem& x) { return x.kind == ItemKind::Track || x.kind == ItemKind::Arc || x.kind == ItemKind::Via; };
        const bool conn_pair = (track_like(a) && (track_like(c) || c.kind == ItemKind::Pad)) || (track_like(c) && a.kind == ItemKind::Pad);
        // KiCad tests a track or via only against items of another net code: two net-less ones are never checked.
        if (conn_pair && a.net == c.net) return;
        if (a.footprint >= 0 && a.footprint == c.footprint) {
          // A footprint's own copper graphics are not checked against its pads when those carry a net, or in
          // net-tie footprints (KiCad: One-Air-Max USB1 shield copper); net-less pads are (microwave POLY).
          if ((a.kind == ItemKind::Graphic || c.kind == ItemKind::Graphic) &&
              (a.net != 0 || c.net != 0 || !b_.footprints[static_cast<std::size_t>(a.footprint)].net_tie_groups.empty())) return;
          // Pads with the same number in one footprint are one electrical pad (split thermal pads), even when KiCad
          // gave each piece its own "unconnected-" net (KiCad: jetson-agx-thor-baseboard U30 pads 57/58).
          if (a.kind == ItemKind::Pad && c.kind == ItemKind::Pad) {
            const auto& pa = b_.pads[static_cast<std::size_t>(a.index)];
            const auto& pc = b_.pads[static_cast<std::size_t>(c.index)];
            if (!pa.number.empty() && pa.number == pc.number) return;
          }
        }
        if (net_tie_exclusion(a, c) || net_tie_exclusion(c, a)) return;
        const int layer = std::countr_zero(common);
        // Track centre lines crossing.
        if (a.kind == ItemKind::Track && c.kind == ItemKind::Track) {
          const auto& ta = b_.tracks[static_cast<std::size_t>(a.index)];
          const auto& tc = b_.tracks[static_cast<std::size_t>(c.index)];
          if (geom::segments_intersect(ta.a, ta.b, tc.a, tc.b)) {
            add("tracks_crossing", &a, &c, 0, 0, layer);
            return;
          }
        }
        const Coord req = re_.clearance(a, c, layer);
        if (!pair_closer(a, c, req - o_.epsilon)) return;
        const double g = pair_gap(a, c, req - o_.epsilon);
        // KiCad (drc_test_provider_copper_clearance.cpp): copper overlapping a zone fill is a clearance violation
        // with zero gap; two pads short only when both have a net; a track or via overlapping a pad, track or via
        // of another net shorts even when one side is net-less. Copper graphics: overlapping copper of two nets,
        // or of two net-less items, is a short; when only one side has a net it is a clearance violation.
        bool short_ = false;
        double g_rep = g;  // the gap KiCad reports
        if (g <= 0) {
          if (const Coord inside = circle_inside_rect(a, c); inside > 0) g_rep = static_cast<double>(inside);
          if (g_rep > 0) short_ = false;  // KiCad's quirk below: reported as clearance
          else if (a.kind == ItemKind::Zone || c.kind == ItemKind::Zone) short_ = false;
          else if (conn_pair) short_ = true;
          else if (a.kind == ItemKind::Pad && c.kind == ItemKind::Pad) short_ = a.net && c.net;
          else short_ = (a.net && c.net) || (!a.net && !c.net);
        }
        // KiCad tests a via against other items once per copper layer (testTrackClearances iterates the via's
        // layers), so a via colliding with a through pad or another via is reported on every common layer.
        int reps = 1;
        if ((a.kind == ItemKind::Via || c.kind == ItemKind::Via) && std::popcount(common) > 1) {
          reps = 0;
          for (model::LayerMask m = common; m; m &= m - 1) {
            const int l = std::countr_zero(m);
            if (l == layer || pair_closer(a, c, re_.clearance(a, c, l) - o_.epsilon)) ++reps;
          }
        }
        // KiCad tests an item against a whole zone on a layer (testItemAgainstZone): one report even when the
        // item touches several islands of the fill.
        if (a.kind == ItemKind::Zone || c.kind == ItemKind::Zone) {
          const CopperItem& z = a.kind == ItemKind::Zone ? a : c;
          const std::size_t other = a.kind == ItemKind::Zone ? static_cast<std::size_t>(j) : i;
          if (!zone_reported_.insert({"clearance", other, z.index, layer}).second) return;
        }
        for (int k = 0; k < reps; ++k) {
          if (short_) add("shorting_items", &a, &c, 0, req, layer);
          else add("clearance", &a, &c, static_cast<Coord>(std::max(0.0, g_rep)), req, layer);
        }
      });
    }
  }

  // KiCad's circle-against-rectangle collision (geometry/shape_collisions.cpp) reports, for a circle whose centre
  // lies inside an axis-aligned rectangle, the distance from the circle to the nearest side as the gap, not 0. A
  // via (or round pad) sitting wholly inside another net's plain rectangular pad is therefore a clearance
  // violation in KiCad, not a short (sonde xilinx J2). Returns that gap, or 0 when the quirk does not apply.
  Coord circle_inside_rect(const CopperItem& a, const CopperItem& c) const {
    auto circle = [&](const CopperItem& x) {
      if (x.kind == ItemKind::Via) return true;
      if (x.kind != ItemKind::Pad) return false;
      const auto& p = b_.pads[static_cast<std::size_t>(x.index)];
      return p.shape == model::PadShape::Circle;
    };
    auto rect = [&](const CopperItem& x) {
      if (x.kind != ItemKind::Pad) return false;
      const auto& p = b_.pads[static_cast<std::size_t>(x.index)];
      return p.shape == model::PadShape::Rect && std::fmod(geom::norm_deg(p.angle), 90.0) == 0.0;
    };
    const CopperItem* ci = circle(a) && rect(c) ? &a : circle(c) && rect(a) ? &c : nullptr;
    if (!ci || ci->shapes.size() != 1) return 0;
    const CopperItem& ri = ci == &a ? c : a;
    const auto& p = b_.pads[static_cast<std::size_t>(ri.index)];
    const bool turned = std::fmod(geom::norm_deg(p.angle), 180.0) != 0.0;
    const double hx = static_cast<double>(turned ? p.size_y : p.size_x) / 2, hy = static_cast<double>(turned ? p.size_x : p.size_y) / 2;
    const model::Point ctr = p.pos + geom::rotate(p.drill_offset, p.angle);
    const double dx = std::fabs(static_cast<double>(ci->shapes[0].pts[0].x - ctr.x)), dy = std::fabs(static_cast<double>(ci->shapes[0].pts[0].y - ctr.y));
    if (dx > hx || dy > hy) return 0;  // centre outside the rectangle
    const double side = std::min(hx - dx, hy - dy) - static_cast<double>(ci->shapes[0].r);
    return side > 0 ? static_cast<Coord>(side) : 0;
  }

  // DRC_ENGINE::IsNetTieExclusion: copper of a net may run into the items of a net-tie footprint where it lies
  // on one of that footprint's net-tie pads of the same net (tinytapeout-demo NT1). KiCad tests the collision
  // point; here the item must overlap such a pad.
  bool net_tie_exclusion(const CopperItem& x, const CopperItem& y) {
    if (x.net == 0 || y.footprint < 0 || y.footprint == x.footprint) return false;
    const auto& fp = b_.footprints[static_cast<std::size_t>(y.footprint)];
    if (fp.net_tie_groups.empty()) return false;
    if (fp_pads_.empty()) {
      fp_pads_.assign(b_.footprints.size(), {});
      for (std::size_t k = 0; k < cm_.items.size(); ++k)
        if (cm_.items[k].kind == ItemKind::Pad && cm_.items[k].footprint >= 0)
          fp_pads_[static_cast<std::size_t>(cm_.items[k].footprint)].push_back(static_cast<int>(k));
    }
    for (const int k : fp_pads_[static_cast<std::size_t>(y.footprint)]) {
      const CopperItem& p = cm_.items[static_cast<std::size_t>(k)];
      if (p.net != x.net || !(p.layers & x.layers)) continue;
      const auto& num = b_.pads[static_cast<std::size_t>(p.index)].number;
      bool tied = false;
      for (const auto& g : fp.net_tie_groups)
        if (std::find(g.begin(), g.end(), num) != g.end()) tied = true;
      if (tied && shapes_closer(x, p, o_.epsilon)) return true;
    }
    return false;
  }

  void check_items() {
    for (const auto& it : cm_.items) {
      if (it.kind == ItemKind::Track || it.kind == ItemKind::Arc) {
        const int layer = std::countr_zero(it.layers);
        const auto [mn, mx] = re_.track_width(it, layer);
        if (it.width < mn || (mx > 0 && it.width > mx)) add("track_width", &it, nullptr, it.width, mn, layer);
      } else if (it.kind == ItemKind::Via) {
        const auto& v = b_.vias[static_cast<std::size_t>(it.index)];
        if (const Coord mn = re_.via_diameter_min(it); v.size < mn) add("via_diameter", &it, nullptr, v.size, mn, -1);
        const Coord drill = v.drill > 0 ? v.drill : via_drill_[static_cast<std::size_t>(it.index)];
        if (const Coord mn = re_.annular_width_min(it); (v.size - drill) / 2 < mn) add("annular_width", &it, nullptr, (v.size - drill) / 2, mn, -1);
      }
    }
  }

  void check_pad_rings() {
    for (const auto& it : cm_.items) {
      if (it.kind != ItemKind::Pad) continue;
      const auto& p = b_.pads[static_cast<std::size_t>(it.index)];
      if (p.type != model::PadType::ThruHole || p.drill_x <= 0) continue;
      const Coord ring = std::min((p.size_x - p.drill_x) / 2, (p.size_y - (p.drill_oval ? p.drill_y : p.drill_x)) / 2);
      const Coord mn = re_.annular_width_min(it);
      if (ring < mn) add("annular_width", &it, nullptr, std::max<Coord>(ring, 0), mn, -1);
    }
  }

  void check_holes() {
    const auto& holes = cm_.holes;
    // Hole size.
    for (const auto& h : holes) {
      const CopperItem* owner = h.item >= 0 ? &cm_.items[static_cast<std::size_t>(h.item)] : nullptr;
      const Coord d = h.shape.r * 2;
      if (owner && owner->kind == ItemKind::Via && d < re_.hole_size_min(owner)) add("drill_out_of_range", owner, nullptr, d, re_.hole_size_min(owner), -1);
    }
    // Hole to hole and hole to copper.
    geom::Box all;
    for (const auto& h : holes) all.add(h.shape.box);
    if (all.empty()) return;
    index::UniformGrid hg(all.inflated(reach_), std::max<Coord>(1'000'000, 2 * reach_), holes.size());
    for (std::size_t i = 0; i < holes.size(); ++i) hg.insert(static_cast<int>(i), holes[i].shape.box);
    for (std::size_t i = 0; i < holes.size(); ++i) {
      const Hole& a = holes[i];
      const CopperItem* ao = a.item >= 0 ? &cm_.items[static_cast<std::size_t>(a.item)] : nullptr;
      hg.query(a.shape.box.inflated(reach_), [&](int j) {
        if (static_cast<std::size_t>(j) <= i) return;
        const Hole& c = holes[static_cast<std::size_t>(j)];
        const CopperItem* co = c.item >= 0 ? &cm_.items[static_cast<std::size_t>(c.item)] : nullptr;
        if (a.pad >= 0 && c.pad >= 0) {
          const auto& pa = b_.pads[static_cast<std::size_t>(a.pad)];
          const auto& pc = b_.pads[static_cast<std::size_t>(c.pad)];
          if (a.pad == c.pad || (pa.footprint == pc.footprint && pa.number == pc.number && !pa.number.empty())) return;
        }
        // KiCad's hole-to-hole test ignores slots (pad drills that are not round: milled after drilling).
        auto slot = [&](const Hole& h) {
          if (h.pad < 0) return false;
          const auto& p = b_.pads[static_cast<std::size_t>(h.pad)];
          return p.drill_oval && p.drill_x != p.drill_y;
        };
        if (slot(a) || slot(c)) return;
        // KiCad tests via holes, then pad holes, clearing its list of tested pairs in between: a via and a pad
        // are reported twice (drc_test_provider_hole_to_hole.cpp).
        const int reps = (a.via >= 0) != (c.via >= 0) ? 2 : 1;
        if (a.shape.pts.size() == 1 && c.shape.pts.size() == 1 && a.shape.pts[0] == c.shape.pts[0]) {
          for (int k = 0; k < reps; ++k) {
            Violation v;
            v.type = "holes_co_located";
            v.severity = "warning";
            v.items.push_back({ao ? describe(*ao) : "Hole", a.pos});
            v.items.push_back({co ? describe(*co) : "Hole", c.pos});
            rep_.violations.push_back(std::move(v));
          }
          return;
        }
        const Coord req = re_.hole_to_hole(ao, co);
        if (req > 0 && geom::closer_than(a.shape, c.shape, req - o_.epsilon)) {
          for (int k = 0; k < reps; ++k) {
            Violation v;
            v.type = "hole_to_hole";
            v.items.push_back({ao ? describe(*ao) : "Hole", a.pos});
            v.items.push_back({co ? describe(*co) : "Hole", c.pos});
            v.required = req;
            rep_.violations.push_back(std::move(v));
          }
        }
      });
      // Hole to copper of other nets.
      grid_->query(a.shape.box.inflated(reach_), [&](int j) {
        const CopperItem& c = cm_.items[static_cast<std::size_t>(j)];
        if (j == a.item) return;
        if (c.net == a.net && a.net != 0) return;
        if (c.kind == ItemKind::Zone) {
          // testItemAgainstZone: the hole of a via or pad against another net's fill on each of the owner's
          // layers, only with a positive hole clearance (a fill poured before a via was added can cover it).
          if (!ao || !(ao->layers & c.layers) || (c.net != 0 && c.net == a.net)) return;
          const int zl = std::countr_zero(c.layers);
          const Coord zreq = re_.hole_clearance(ao, c, zl);
          const geom::PolygonIndex* zi = fill_index(c);
          if (zreq > 0 && (zi ? zi->shape_closer(cshape(a), zreq - o_.epsilon) : geom::closer_than(cshape(a), c.shapes.front(), zreq - o_.epsilon)) &&
              zone_reported_.insert({"hole", static_cast<std::size_t>(a.item), c.index, zl}).second)
            add("hole_clearance", ao, &c, -1, zreq, zl);
          return;
        }
        // A via against a pad with copper or another via: per layer, in check_via_hole_pairs.
        if ((a.via >= 0 && (c.kind == ItemKind::Via || c.kind == ItemKind::Pad)) || (a.pad >= 0 && a.item >= 0 && c.kind == ItemKind::Via)) return;
        if (a.pad >= 0 && c.footprint >= 0 && c.footprint == b_.pads[static_cast<std::size_t>(a.pad)].footprint) return;  // own footprint
        if (a.pad >= 0 && c.kind == ItemKind::Pad && b_.pads[static_cast<std::size_t>(c.index)].footprint == b_.pads[static_cast<std::size_t>(a.pad)].footprint && !a.plated) return;
        const int layer = std::countr_zero(c.layers);
        const Coord req = re_.hole_clearance(ao, c, layer);
        // KiCad tests a track or via against holes (and a via's hole against copper) even at zero clearance:
        // copper may not reach into a hole (testSingleLayerItemAgainstItem). Pad-to-pad hole tests need a clearance.
        const bool zero_ok = (c.kind == ItemKind::Track || c.kind == ItemKind::Arc || c.kind == ItemKind::Via || a.via >= 0) && c.net != a.net;
        if (req <= 0 && !(zero_ok && req == 0)) return;
        const Coord thr = req > 0 ? req - o_.epsilon : 0;
        for (const auto& s : c.shapes)
          if (geom::closer_than(cshape(a), s, thr)) {
            add("hole_clearance", &c, ao, -1, req, layer);
            break;
          }
      });
    }
  }

  // The hole a hole-clearance test sees. KiCad's PCB_VIA::GetEffectiveHoleShape uses the drill as written, so a
  // via written without one (old boards) has a point hole there, while its drill size and hole-to-hole tests use
  // the net class drill (GetDrillValue; verified on PCBench tinyFISH).
  geom::Shape cshape(const Hole& h) const {
    if (h.via >= 0 && b_.vias[static_cast<std::size_t>(h.via)].drill <= 0) return geom::Shape::point(h.pos, 0);
    return h.shape;
  }

  // Holes between a via and a pad with copper, or two vias. KiCad tests them per copper layer: the track loop
  // (testSingleLayerItemAgainstItem) reports one hole violation per layer when either hole reaches the other's
  // copper, even at zero clearance; the pad loop (testPadAgainstItem) adds one per layer for the via's hole
  // against the pad's copper when the hole clearance is positive.
  void check_via_hole_pairs() {
    std::vector<int> hole_of(cm_.items.size(), -1);
    for (std::size_t h = 0; h < cm_.holes.size(); ++h)
      if (cm_.holes[h].item >= 0) hole_of[static_cast<std::size_t>(cm_.holes[h].item)] = static_cast<int>(h);
    auto hits = [&](const Hole& h, const CopperItem& c, Coord req) {
      for (const auto& s : c.shapes)
        if (geom::closer_than(cshape(h), s, req > 0 ? req - o_.epsilon : 0)) return true;
      return false;
    };
    for (std::size_t i = 0; i < cm_.items.size(); ++i) {
      const CopperItem& v = cm_.items[i];
      if (v.kind != ItemKind::Via || hole_of[i] < 0) continue;
      const Hole& vh = cm_.holes[static_cast<std::size_t>(hole_of[i])];
      grid_->query(v.box.inflated(reach_), [&](int jj) {
        const auto j = static_cast<std::size_t>(jj);
        const CopperItem& c = cm_.items[j];
        if (j == i || (c.kind != ItemKind::Pad && c.kind != ItemKind::Via)) return;
        if (c.kind == ItemKind::Via && j < i) return;  // each via pair once
        if (c.net == v.net) return;                    // KiCad tests only items of another net code here
        const Hole* ch = hole_of[j] >= 0 ? &cm_.holes[static_cast<std::size_t>(hole_of[j])] : nullptr;  // SMD pads: none
        int n = 0;
        for (model::LayerMask m = v.layers & c.layers; m; m &= m - 1) {
          const int l = std::countr_zero(m);
          const Coord req_v = re_.hole_clearance(&v, c, l);  // the via's hole against c's copper
          const Coord req_c = re_.hole_clearance(&c, v, l);  // c's hole against the via's copper
          if ((req_v >= 0 && hits(vh, c, req_v)) || (ch && req_c >= 0 && hits(*ch, v, req_c))) ++n;
          if (c.kind == ItemKind::Pad && req_v > 0 && hits(vh, c, req_v)) ++n;
        }
        for (int k = 0; k < n; ++k) add("hole_clearance", &c, &v, -1, re_.hole_clearance(&v, c, std::countr_zero(v.layers & c.layers)), -1);
      });
    }
  }

  void check_edges() {
    if (cm_.edges.empty() && cm_.margins.empty()) return;
    std::vector<const geom::Shape*> edges;
    for (const auto& e : cm_.edges) edges.push_back(&e);
    for (const auto& e : cm_.margins) edges.push_back(&e);
    for (const auto& it : cm_.items) {
      const int layer = std::countr_zero(it.layers);
      const Coord req = re_.edge_clearance(it, layer);
      if (req < 0) continue;  // KiCad tests at zero clearance too: copper may not cross the edge
      for (const geom::Shape* ep : edges) {
        const geom::Shape& e = *ep;
        if (!e.box.inflated(req).intersects(it.box)) continue;
        bool hit = false;
        const Coord thr = req > 0 ? req - o_.epsilon : 1;  // 0: touching or crossing
        if (const geom::PolygonIndex* zi = fill_index(it)) hit = zi->shape_closer(e, thr);
        else
          for (const auto& s : it.shapes)
            if (geom::closer_than(s, e, thr)) { hit = true; break; }
        if (hit) {
          add("copper_edge_clearance", &it, nullptr, -1, req, layer);
          break;
        }
      }
    }
  }

  void check_keepouts() {
    for (const auto& z : b_.zones) {
      if (!z.rule_area || z.outline.empty() || z.outline.front().size() < 3) continue;
      const geom::Shape area = geom::Shape::polygon(z.outline.front(), 0);
      for (const auto& it : cm_.items) {
        if (!(it.layers & z.copper)) continue;
        const bool forbidden = ((it.kind == ItemKind::Track || it.kind == ItemKind::Arc) && z.keepout_tracks) ||
                               (it.kind == ItemKind::Via && z.keepout_vias) || (it.kind == ItemKind::Pad && z.keepout_pads);
        if (!forbidden || !area.box.intersects(it.box)) continue;
        bool hit = false;
        for (const auto& s : it.shapes)
          if (geom::closer_than(s, area, 1)) { hit = true; break; }
        if (hit) add("items_not_allowed", &it, nullptr, -1, -1, std::countr_zero(it.layers & z.copper));
      }
    }
  }

  // Connectivity (see drc/connectivity.cpp) and the checks that depend on it.
  void check_connectivity() {
    const auto n = cm_.items.size();
    const Connectivity con = compute_connectivity(b_, cm_, *grid_, true);
    struct Roots {
      const std::vector<int>& r;
      int find(int i) const { return r[static_cast<std::size_t>(i)]; }
    } uf{con.root};
    std::map<model::NetId, std::vector<int>> roots;  // net -> distinct cluster roots
    std::map<int, int> root_item;
    // KiCad's ratsnest joins every copper cluster of a net (pads, and also stray tracks/vias), not only
    // clusters that contain pads (verified on fuzzed boards). Zone fills and graphics do not form clusters.
    for (std::size_t i = 0; i < n; ++i) {
      const CopperItem& it = cm_.items[i];
      if (it.net == 0 || it.kind == ItemKind::Zone || it.kind == ItemKind::Graphic) continue;
      const int r = uf.find(static_cast<int>(i));
      auto& v = roots[it.net];
      if (std::find(v.begin(), v.end(), r) == v.end()) {
        v.push_back(r);
        root_item[r] = static_cast<int>(i);
      }
    }
    for (const auto& [net, rs] : roots)
      for (std::size_t k = 1; k < rs.size(); ++k) {
        Violation v;
        v.type = "unconnected_items";
        v.description = "Missing connection in net " + b_.nets[static_cast<std::size_t>(net)].name;
        v.items.push_back({describe(cm_.items[static_cast<std::size_t>(root_item[rs[k - 1]])]), cm_.items[static_cast<std::size_t>(root_item[rs[k - 1]])].pos});
        v.items.push_back({describe(cm_.items[static_cast<std::size_t>(root_item[rs[k]])]), cm_.items[static_cast<std::size_t>(root_item[rs[k]])].pos});
        rep_.unconnected.push_back(std::move(v));
      }
    if (!o_.dangling) return;
    for (std::size_t i = 0; i < n; ++i) {
      const CopperItem& it = cm_.items[i];
      if ((it.kind == ItemKind::Track || it.kind == ItemKind::Arc) && track_dangling(i))
        add("track_dangling", &it, nullptr, -1, -1, std::countr_zero(it.layers), "warning");
      else if (it.kind == ItemKind::Via && via_dangling(i))
        add("via_dangling", &it, nullptr, -1, -1, -1, "warning");
    }
  }

  // CONNECTIVITY_DATA::TestTrackEndpointDangling (KiCad 10): every item linked to the track in the item graph,
  // of any net, counts for the end(s) it covers within half the track width. An item covering both ends counts
  // for the nearer one only, except zones, pads and vias, which make the track not dangling.
  bool track_dangling(std::size_t i) const {
    const CopperItem& t = cm_.items[i];
    model::Point ends[2];
    if (t.kind == ItemKind::Track) {
      ends[0] = b_.tracks[static_cast<std::size_t>(t.index)].a;
      ends[1] = b_.tracks[static_cast<std::size_t>(t.index)].b;
    } else {
      ends[0] = b_.arcs[static_cast<std::size_t>(t.index)].a;
      ends[1] = b_.arcs[static_cast<std::size_t>(t.index)].b;
    }
    const geom::Shape disk[2] = {geom::Shape::point(ends[0], t.width / 2), geom::Shape::point(ends[1], t.width / 2)};
    int count[2] = {0, 0};
    for (const int jj : graph_.adj[i]) {
      const CopperItem& c = cm_.items[static_cast<std::size_t>(jj)];
      if (!(c.layers & t.layers)) continue;
      bool hit[2] = {false, false};
      for (int k = 0; k < 2; ++k)
        for (const auto& s : c.shapes)
          if (geom::closer_than(disk[k], s, 0)) { hit[k] = true; break; }  // strict, like SHAPE::Collide(point)
      if (hit[0] && hit[1]) {
        if (c.kind == ItemKind::Zone || c.kind == ItemKind::Pad || c.kind == ItemKind::Via) return false;
        // getMinDist: distance to the other track's nearer end point, or to the item's position (truncated).
        Coord d[2];
        for (int k = 0; k < 2; ++k) {
          auto dist = [&](model::Point p) {
            return static_cast<Coord>(std::hypot(static_cast<double>(p.x - ends[k].x), static_cast<double>(p.y - ends[k].y)));
          };
          if (c.kind == ItemKind::Track) {
            const auto& u = b_.tracks[static_cast<std::size_t>(c.index)];
            d[k] = std::min(dist(u.a), dist(u.b));
          } else if (c.kind == ItemKind::Arc) {
            const auto& u = b_.arcs[static_cast<std::size_t>(c.index)];
            d[k] = std::min(dist(u.a), dist(u.b));
          } else {
            d[k] = dist(c.pos);
          }
        }
        ++count[d[0] < d[1] ? 0 : 1];
      } else if (hit[0]) {
        ++count[0];
      } else if (hit[1]) {
        ++count[1];
      }
      if (count[0] > 0 && count[1] > 0) return false;
    }
    // Zone fills last (the outcome does not depend on the order): one covering both ends makes the track not
    // dangling, one covering an end counts for it.
    for (int k = 0; k < 2; ++k)
      for (const int z : zones_touching(cm_, *zones_, *grid_, disk[k], t.layers, 0)) {
        if (k == 0 && geom::closer_than(disk[1], cm_.items[static_cast<std::size_t>(z)].shapes.front(), 0)) return false;
        ++count[k];
      }
    return !(count[0] > 0 && count[1] > 0);
  }

  // A via is dangling when every item linked to it starts on the same copper layer (KiCad compares
  // CN_ITEM::Layer(), the first layer of each item: a through pad or via counts as F.Cu), or when nothing is
  // linked to it and it has a net.
  bool via_dangling(std::size_t i) const {
    const CopperItem& v = cm_.items[i];
    int first = -1;
    auto other_layer = [&](int j) {
      const int l = std::countr_zero(cm_.items[static_cast<std::size_t>(j)].layers);
      if (first < 0) first = l;
      return l != first;
    };
    for (const int j : graph_.adj[i])
      if (other_layer(j)) return false;
    for (const auto& s : v.shapes)
      for (const int z : zones_touching(cm_, *zones_, *grid_, s, v.layers)) {
        if (v.free_via && cm_.items[static_cast<std::size_t>(z)].net != v.net) continue;  // neither can change net
        if (other_layer(z)) return false;
      }
    return first < 0 ? v.net > 0 : true;
  }

  const model::Board& b_;
  const model::DesignRules& r_;
  const DrcOptions& o_;
  CopperModel cm_;
  RuleEngine re_;
  std::unique_ptr<index::UniformGrid> grid_;
  ItemGraph graph_;
  std::vector<Coord> via_drill_;  // net-class drill of vias written without one (0 otherwise)
  std::vector<std::vector<int>> fp_pads_;  // copper pad items per footprint (built on first use)
  std::set<std::tuple<std::string, std::size_t, int, int>> zone_reported_;  // (check, item, zone, layer)
  std::unique_ptr<ZoneFills> zones_;
  Coord reach_ = 0;
  DrcReport rep_;

};

}  // namespace

DrcReport run_drc(const model::Board& b, const model::DesignRules& r, const DrcOptions& opt) { return Checker(b, r, opt).run(); }

void write_drc_json(const DrcReport& rep, const std::string& path) {
  using nlohmann::json;
  auto item_json = [](const ViolationItem& it) {
    return json{{"description", it.description}, {"pos", {{"x", nm_to_mm(it.pos.x)}, {"y", nm_to_mm(it.pos.y)}}}};
  };
  auto vjson = [&](const Violation& v) {
    json items = json::array();
    for (const auto& it : v.items) items.push_back(item_json(it));
    json j{{"type", v.type}, {"severity", v.severity}, {"description", v.description}, {"items", items}};
    if (v.required >= 0) j["required_mm"] = nm_to_mm(v.required);
    if (v.actual >= 0) j["actual_mm"] = nm_to_mm(v.actual);
    return j;
  };
  json d{{"source", "tracemaker"}, {"coordinate_units", "mm"}, {"violations", json::array()}, {"unconnected_items", json::array()}};
  for (const auto& v : rep.violations) d["violations"].push_back(vjson(v));
  for (const auto& v : rep.unconnected) d["unconnected_items"].push_back(vjson(v));
  d["warnings"] = rep.warnings;
  std::ofstream(path) << d.dump(1);
}

}  // namespace tmk::drc

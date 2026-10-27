// SPDX-License-Identifier: GPL-3.0-or-later
#include "drc/refill.hpp"

#include <algorithm>
#include <cmath>
#include <future>
#include <map>
#include <numbers>
#include <set>
#include <tuple>

#include "drc/connectivity.hpp"
#include "drc/copper.hpp"
#include "drc/rule_engine.hpp"
#include "geom/clip.hpp"
#include "index/uniform_grid.hpp"

namespace tmk::drc {

namespace {

using geom::Point;
using geom::Ring;
using geom::Rings;
using model::ZoneConnect;

constexpr Coord kMaxError = 5'000;      // BOARD_DESIGN_SETTINGS::m_MaxError default (ARC_HIGH_DEF)
constexpr Coord kExtraMargin = 500;     // ADVANCED_CFG::m_ExtraClearance (0.0005 mm)
constexpr Coord kMinWidthEpsilon = 1'000;  // fillCopperZone: features of exactly min_width survive pruning

Rings shapes_rings(const std::vector<geom::Shape>& shapes, Coord grow) {
  Rings out;
  for (const auto& s : shapes) {
    auto r = geom::shape_rings(s, grow, kMaxError);
    out.insert(out.end(), std::make_move_iterator(r.begin()), std::make_move_iterator(r.end()));
  }
  return out;
}

void append(Rings& to, Rings from) { to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end())); }

geom::Box rings_box(const Rings& rs) {
  geom::Box b;
  for (const auto& r : rs)
    for (const auto& p : r) b.add(p);
  return b;
}

// ZONE::HigherPriority for two copper zones that are no teardrops.
bool higher_priority(const model::Zone& a, const model::Zone& b) {
  if (a.priority != b.priority) return a.priority > b.priority;
  return a.uuid > b.uuid;
}

// EvalZoneConnection: the pad's own setting, else its footprint's, else the zone's; thru_hole_only is thermal
// for plated through holes and solid for every other pad.
ZoneConnect zone_connection(const model::Board& b, const model::Pad& pad, const model::Zone& zone) {
  ZoneConnect c = pad.zone_connect;
  if (c == ZoneConnect::Inherited && pad.footprint >= 0) c = b.footprints[static_cast<std::size_t>(pad.footprint)].zone_connect;
  if (c == ZoneConnect::Inherited) c = zone.connect;
  if (c == ZoneConnect::ThtThermal) c = pad.type == model::PadType::ThruHole ? ZoneConnect::Thermal : ZoneConnect::Full;
  return c;
}

Coord thermal_gap(const model::Pad& pad, const model::Zone& zone) { return pad.thermal_gap > 0 ? pad.thermal_gap : zone.thermal_gap; }

// The four spokes of a thermal pad (buildThermalSpokes), each as its polygon and its outer test point.
struct Spoke {
  Ring ring;
  Point test;
};
void spokes(const model::Pad& pad, const model::Zone& zone, std::vector<Spoke>& out) {
  Coord w = zone.thermal_bridge_width;
  if (pad.thermal_bridge_width > 0) w = std::max(pad.thermal_bridge_width, zone.min_thickness);
  w = std::min(w, std::min(pad.size_x, pad.size_y));  // a spoke wider than the pad is no thermal relief
  if (w < zone.min_thickness) return;
  const Coord half_w = w / 2;
  const bool circular = pad.shape == model::PadShape::Circle || (pad.shape == model::PadShape::Oval && pad.size_x == pad.size_y);
  double angle = pad.thermal_bridge_angle;
  if (angle < 0) angle = pad.shape == model::PadShape::Circle ? 45 : 90;
  // Box of the pad's copper and hole at the origin, unrotated, grown past the thermal gap.
  model::Pad local = pad;
  local.pos = {};
  local.angle = 0;
  local.drill_offset = {};
  geom::Box box;
  for (const auto& s : pad_shapes(local)) box.add(s.box);
  if (pad.drill_x > 0) box.add(geom::Box{-pad.drill_x / 2, -pad.drill_y / 2, pad.drill_x / 2, pad.drill_y / 2});
  box = box.inflated(thermal_gap(pad, zone) + kMaxError + zone.min_thickness / 2);
  const Point centre{(box.x0 + box.x1) / 2, (box.y0 + box.y1) / 2};
  const double hx = static_cast<double>(box.x1 - box.x0) / 2, hy = static_cast<double>(box.y1 - box.y0) / 2;
  const double base = circular ? 0 : angle;
  const Point shape_pos = pad.pos + geom::rotate(pad.drill_offset, pad.angle);
  for (int k = 0; k < 4; ++k) {
    const double a = geom::norm_deg(base + 90.0 * k);
    Point side, hit;
    if (a == 90.0 || a == 270.0) {
      side = {half_w, 0};
      hit = {0, geom::kiround((a == 90.0 ? 1 : -1) * hy)};  // EDA_ANGLE: dx = cos, dy = sin
    } else if (a == 0.0 || a == 180.0) {
      side = {0, half_w};
      hit = {geom::kiround((a == 0.0 ? 1 : -1) * hx), 0};
    } else {
      const double rad = a * std::numbers::pi / 180.0, dx = std::cos(rad), dy = std::sin(rad);
      const double tx = hx / std::abs(dx), ty = hy / std::abs(dy);
      if (tx < ty) {
        side = {0, geom::kiround(static_cast<double>(half_w) / std::cos(rad))};
        hit = {geom::kiround(dx * tx), geom::kiround(dy * tx)};
      } else {
        side = {geom::kiround(static_cast<double>(half_w) / std::sin(rad)), 0};
        hit = {geom::kiround(dx * ty), geom::kiround(dy * ty)};
      }
    }
    Ring r{centre + side, centre - side, centre + hit - side, centre + hit, centre + hit + side};
    Point test = centre + hit;
    auto place = [&](Point p) {
      if (circular) p = geom::rotate(p, angle);
      return shape_pos + geom::rotate(p, pad.angle);
    };
    for (auto& p : r) p = place(p);
    // Outer orientation: under the non-zero rule a reversed ring would cancel the fill it overlaps.
    if (geom::area(r) < 0) std::reverse(r.begin(), r.end());
    out.push_back({std::move(r), place(test)});
  }
}

class Filler {
 public:
  Filler(const model::Board& b, const model::DesignRules& r) : b_(b), re_(b, r), cm_(build_copper(b)) {
    re_.use_zone_clearance_overrides();
    for (std::size_t i = 0; i < cm_.items.size(); ++i) {
      const auto& it = cm_.items[i];
      if (it.kind == ItemKind::Pad) pad_item_.emplace(it.index, static_cast<int>(i));
    }
    for (std::size_t i = 0; i < cm_.holes.size(); ++i)
      if (cm_.holes[i].pad >= 0) pad_hole_.emplace(cm_.holes[i].pad, static_cast<int>(i));
    // Board area: every Edge.Cuts loop under the even-odd rule (outline less cut-outs, BOARD::GetBoardPolygonOutlines).
    // An outline that does not close is not used (ZONE_FILLER: m_brdOutlinesValid), at KiCad's chaining epsilon.
    bool closed = false;
    const auto loops = edge_loops(cm_.edges, 10'000, &closed);
    if (closed && !loops.empty()) board_ = geom::even_odd(loops);
    else warnings_.push_back("zone refill: the board outline does not close; fills are not clipped to it");
    for (const auto& c : r.custom)
      for (const auto& k : c.constraints)
        if (k.type == "zone_connection" || k.type == "thermal_relief_gap" || k.type == "thermal_spoke_width" ||
            k.type == "physical_clearance")
          warnings_.push_back("zone refill: rule '" + c.name + "': " + k.type + " is not applied");
    worst_ = re_.max_clearance() + kExtraMargin;
    // KiCad breaks priority ties by zone id; files from KiCad 5 and older give every zone the same one, and KiCad's
    // own refill of such boards then disagrees with this one (two PCBench boards, doc 05 §36).
    std::map<std::string, std::set<model::NetId>> nets_by_id;
    for (const auto& z : b.zones)
      if (fillable(z)) nets_by_id[z.uuid].insert(z.net);
    for (const auto& [id, nets] : nets_by_id)
      if (nets.size() > 1) {
        warnings_.push_back("zone refill: zones of different nets share one id (KiCad 5 or older file); KiCad's refill may differ");
        break;
      }
  }

  RefillResult run() {
    RefillResult res;
    res.board = b_;
    const int layers = b_.copper_count();
    fills_.assign(b_.zones.size(), std::vector<Rings>(static_cast<std::size_t>(layers)));
    // Zones to fill on each layer, highest priority first: a zone knocks out the fills of higher-priority zones of
    // other nets, so those are filled before it (ZONE_FILLER::Fill's dependency order).
    std::vector<std::vector<int>> order(static_cast<std::size_t>(layers));
    for (std::size_t z = 0; z < b_.zones.size(); ++z) {
      const auto& zone = b_.zones[z];
      if (!fillable(zone)) continue;
      if (zone.hatched)
        warnings_.push_back("zone refill: hatched zone" + (zone.name.empty() ? std::string{} : " '" + zone.name + "'") +
                            ": the hatch is not redrawn, its stored fill is cut to the new solid fill");
      ++res.zones;
      for (int l = 0; l < layers; ++l)
        if (zone.copper & model::layer_bit(l)) order[static_cast<std::size_t>(l)].push_back(static_cast<int>(z));
    }
    for (auto& o : order)
      std::stable_sort(o.begin(), o.end(), [&](int a, int c) { return higher_priority(b_.zones[static_cast<std::size_t>(a)], b_.zones[static_cast<std::size_t>(c)]); });
    // Layers are independent: one task per layer, results stored by (zone, layer).
    std::vector<std::future<void>> tasks;
    for (int l = 0; l < layers; ++l)
      tasks.push_back(std::async(std::launch::async, [this, l, &order] {
        for (const int z : order[static_cast<std::size_t>(l)]) fills_[static_cast<std::size_t>(z)][static_cast<std::size_t>(l)] = fill(z, l);
      }));
    for (auto& t : tasks) t.get();

    // Write the fills into the copy, one fractured polygon per island, then remove islands without a pad.
    std::vector<std::uint8_t> refilled(b_.zones.size(), 0);
    for (const auto& o : order)
      for (const int z : o) refilled[static_cast<std::size_t>(z)] = 1;
    for (std::size_t z = 0; z < b_.zones.size(); ++z) {
      auto& zone = res.board.zones[z];
      if (zone.teardrop && !zone.rule_area) zone.fills.clear();
      if (!refilled[z]) continue;
      zone.fills.clear();
      for (int l = 0; l < layers; ++l)
        for (const auto& poly : geom::polygons(fills_[z][static_cast<std::size_t>(l)])) zone.fills.emplace_back(l, geom::fracture(poly));
    }
    res.islands_removed = remove_islands(res.board, refilled);
    res.warnings = std::move(warnings_);
    return res;
  }

 private:
  static bool fillable(const model::Zone& z) {
    return !z.rule_area && !z.teardrop && z.copper != 0 && !z.outline.empty() && !z.outline.front().empty();
  }

  Rings outline_rings(const model::Zone& z) const {
    Rings rs;
    for (const auto& o : z.outline)
      if (o.size() >= 3) rs.push_back(o);
    return geom::even_odd(rs);
  }

  CopperItem zone_item(int z, int layer) const {
    const auto& zone = b_.zones[static_cast<std::size_t>(z)];
    CopperItem it;
    it.kind = ItemKind::Zone;
    it.index = z;
    it.sub = -1;  // the zone itself, not one of its fill polygons (the rule engine caches only those)
    it.net = zone.net;
    it.layers = model::layer_bit(layer);
    it.shapes = {geom::Shape::polygon(zone.outline.front(), 0)};
    it.box = it.shapes.front().box;
    it.footprint = zone.footprint;
    return it;
  }

  Rings fill(int z, int layer) const {
    const auto& zone = b_.zones[static_cast<std::size_t>(z)];
    const model::LayerMask lb = model::layer_bit(layer);
    const CopperItem zit = zone_item(z, layer);
    const geom::Box zbox = zit.box.inflated(worst_);
    Rings extents = outline_rings(zone);
    if (!board_.empty()) extents = geom::intersect(extents, board_);
    if (extents.empty()) return {};
    Rings fill = extents;
    const bool zone_net = zone.net != 0;

    // Thermal reliefs (knockoutThermalReliefs).
    std::vector<int> thermal_pads, no_connection;
    Rings relief;
    // Coincident pads (USB-C's paired GND pins) count once, as KiCad's PAD_KNOCKOUT_KEY: otherwise each copy's
    // spokes would hold the other's test point and pass the spoke-to-spoke test.
    std::set<std::tuple<Coord, Coord, Coord, Coord, int, double, model::NetId>> seen;
    for (std::size_t i = 0; i < b_.pads.size(); ++i) {
      const auto& pad = b_.pads[i];
      const bool npth_hole = pad.type == model::PadType::NpThruHole && pad.drill_x > 0;
      if (!(pad.copper & lb) && !npth_hole) continue;
      const auto pi = pad_item_.find(static_cast<int>(i));
      const auto hi = pad_hole_.find(static_cast<int>(i));
      geom::Box pbox = pi != pad_item_.end() ? cm_.items[static_cast<std::size_t>(pi->second)].box : geom::Box{};
      if (hi != pad_hole_.end()) pbox.add(cm_.holes[static_cast<std::size_t>(hi->second)].shape.box);
      if (!pbox.intersects(zbox)) continue;
      if (pad.shape != model::PadShape::Custom) {
        Coord sx = pad.size_x, sy = pad.size_y;
        if (pad.shape == model::PadShape::Circle) sx = sy = std::max({pad.size_x, pad.size_y, pad.drill_x, pad.drill_y});
        if (!seen.emplace(pad.pos.x, pad.pos.y, sx, sy, static_cast<int>(pad.shape), pad.angle, pad.net).second) continue;
      }
      if (pad.net != zone.net || !zone_net) {
        no_connection.push_back(static_cast<int>(i));
        continue;
      }
      ZoneConnect c = zone_connection(b_, pad, zone);
      if (c == ZoneConnect::Thermal && !(pad.copper & lb)) c = ZoneConnect::None;
      if (c == ZoneConnect::Thermal) {
        if (pi == pad_item_.end()) continue;
        const auto& shapes = cm_.items[static_cast<std::size_t>(pi->second)].shapes;
        if (geom::intersect(shapes_rings(shapes, 0), fill).empty()) continue;
        thermal_pads.push_back(static_cast<int>(i));
        append(relief, shapes_rings(shapes, thermal_gap(pad, zone)));
      } else if (c == ZoneConnect::None) {
        const Coord gap = std::max<Coord>(zone.clearance, 0);
        if ((pad.copper & lb) && pi != pad_item_.end()) append(relief, shapes_rings(cm_.items[static_cast<std::size_t>(pi->second)].shapes, gap));
        else if (hi != pad_hole_.end()) append(relief, geom::shape_rings(cm_.holes[static_cast<std::size_t>(hi->second)].shape, gap, kMaxError));
      }
    }
    if (!relief.empty()) fill = geom::subtract(fill, relief);

    // Clearances (buildCopperItemClearances).
    Rings holes;
    for (const int i : no_connection) {
      const auto& pad = b_.pads[static_cast<std::size_t>(i)];
      const auto pi = pad_item_.find(i);
      const auto hi = pad_hole_.find(i);
      const CopperItem* item = pi != pad_item_.end() ? &cm_.items[static_cast<std::size_t>(pi->second)] : nullptr;
      const bool flash = (pad.copper & lb) && item;
      const bool has_hole = hi != pad_hole_.end();
      Coord gap = -1;
      if (item && (flash || (has_hole && pad.type == model::PadType::ThruHole))) gap = re_.clearance(zit, *item, layer);
      if (flash && gap >= 0) append(holes, shapes_rings(item->shapes, gap + kExtraMargin));
      if (has_hole) {
        if (pad.type == model::PadType::NpThruHole) gap = -1;
        CopperItem probe;  // NPTH pads without copper have no item: the rule engine sees a bare pad
        probe.kind = ItemKind::Pad;
        probe.index = i;
        probe.net = pad.net;
        probe.footprint = pad.footprint;
        probe.pos = pad.pos;
        const CopperItem& owner = item ? *item : probe;
        gap = std::max({gap, re_.physical_hole_clearance(&owner, zit, layer), re_.hole_clearance(&owner, zit, layer)});
        if (pad.type == model::PadType::NpThruHole && pad.drill_x != pad.drill_y) gap = std::max(gap, re_.edge_clearance(zit, layer));
        if (gap >= 0) append(holes, geom::shape_rings(cm_.holes[static_cast<std::size_t>(hi->second)].shape, gap + kExtraMargin, kMaxError));
      }
    }
    for (std::size_t i = 0; i < cm_.items.size(); ++i) {
      const auto& it = cm_.items[i];
      if (it.kind != ItemKind::Track && it.kind != ItemKind::Arc && it.kind != ItemKind::Via && it.kind != ItemKind::Graphic) continue;
      if (!(it.layers & lb) || !it.box.intersects(zbox)) continue;
      const bool same_net = zone_net && it.net == zone.net;
      Coord gap = same_net ? -1 : re_.clearance(zit, it, layer);
      if (it.kind != ItemKind::Via) {
        if (gap >= 0) append(holes, shapes_rings(it.shapes, gap + kExtraMargin));
        continue;
      }
      if (gap > 0) append(holes, shapes_rings(it.shapes, gap + kExtraMargin));
      const auto& via = b_.vias[static_cast<std::size_t>(it.index)];
      gap = std::max(gap, re_.physical_hole_clearance(&it, zit, layer));
      if (!same_net) gap = std::max(gap, re_.hole_clearance(&it, zit, layer));
      if (gap >= 0) holes.push_back(geom::circle_ring(via.pos, via.drill / 2 + gap + kExtraMargin, kMaxError));
    }
    // The board edge at the edge clearance, line widths ignored; Margin graphics like the edge.
    {
      const Coord gap = re_.edge_clearance(zit, layer);
      if (gap >= 0)
        for (const auto* list : {&cm_.edges, &cm_.margins})
          for (const auto& e : *list)
            if (e.box.intersects(zbox)) append(holes, geom::shape_rings(e, gap + kExtraMargin, kMaxError));
    }
    // Keep-outs that forbid pours, at their outline; higher-priority zones of other nets, at clearance from
    // their new fill.
    for (std::size_t o = 0; o < b_.zones.size(); ++o) {
      if (static_cast<int>(o) == z) continue;
      const auto& other = b_.zones[o];
      if (!(other.copper & lb)) continue;
      if (other.rule_area) {
        if (other.keepout_pour && !other.outline.empty()) append(holes, outline_rings(other));
        continue;
      }
      if (other.teardrop || other.net == zone.net || !higher_priority(other, zone)) continue;
      const Rings& of = fills_[o][static_cast<std::size_t>(layer)];
      if (of.empty() || !rings_box(of).intersects(zbox)) continue;
      const Coord gap = std::max<Coord>(0, re_.clearance(zit, zone_item(static_cast<int>(o), layer), layer));
      append(holes, geom::offset(of, gap + kExtraMargin + kMaxError, geom::Corners::Round, kMaxError));
    }
    holes = geom::unite(holes);

    // Spokes that reach the fill once clearances and minimum-width pruning are applied.
    const Coord half_min = zone.min_thickness / 2;
    const bool prune = half_min - kMinWidthEpsilon > kMinWidthEpsilon;
    std::vector<Spoke> sp;
    for (const int i : thermal_pads) spokes(b_.pads[static_cast<std::size_t>(i)], zone, sp);
    if (!sp.empty()) {
      Rings test = geom::subtract(fill, holes);
      if (prune) {
        test = geom::offset(test, -(half_min - kMinWidthEpsilon), geom::Corners::Chamfer, kMaxError);
        test = geom::offset(test, half_min - kMinWidthEpsilon, geom::Corners::Chamfer, kMaxError);
      }
      Rings keep;
      for (std::size_t s = 0; s < sp.size(); ++s) {
        bool ok = geom::contains(test, sp[s].test);
        for (std::size_t o = 0; !ok && o < sp.size(); ++o)
          ok = o != s && geom::contains({sp[o].ring}, sp[s].test) && geom::contains({sp[s].ring}, sp[o].test);
        if (ok) keep.push_back(sp[s].ring);
      }
      if (!keep.empty()) {
        keep.insert(keep.begin(), fill.begin(), fill.end());
        fill = geom::unite(keep);
      }
    }
    fill = geom::subtract(fill, holes);

    // Minimum width: deflate, drop blobs narrower than min_thickness on both axes, re-inflate.
    if (prune) {
      fill = geom::offset(fill, -(half_min - kMinWidthEpsilon), geom::Corners::Chamfer, kMaxError);
      Rings kept;
      for (auto& poly : geom::polygons(fill)) {
        geom::Box ob;
        for (const auto& p : poly.outer) ob.add(p);
        if (std::max(ob.x1 - ob.x0, ob.y1 - ob.y0) < zone.min_thickness) continue;
        kept.push_back(std::move(poly.outer));
        append(kept, std::move(poly.holes));
      }
      fill = geom::offset(kept, half_min - kMinWidthEpsilon, geom::Corners::Round, kMaxError);
    }
    // Trim what re-inflating added outside the zone, into clearances, and into thermal pads' holes.
    for (const int i : thermal_pads)
      if (const auto hi = pad_hole_.find(i); hi != pad_hole_.end())
        append(holes, geom::shape_rings(cm_.holes[static_cast<std::size_t>(hi->second)].shape, 0, kMaxError));
    fill = geom::subtract(geom::intersect(fill, extents), holes);
    // Same-net zones of explicitly higher priority own their area (subtractHigherPriorityZones).
    Rings owned;
    for (std::size_t o = 0; o < b_.zones.size(); ++o) {
      const auto& other = b_.zones[o];
      if (static_cast<int>(o) == z || other.rule_area || other.teardrop || other.net != zone.net || !(other.copper & lb)) continue;
      if (other.priority > zone.priority) append(owned, outline_rings(other));
    }
    if (!owned.empty()) fill = geom::subtract(fill, owned);
    // Hatched fills are not modelled. The stored hatch is kept only where the solid fill for the current copper
    // still is: copper the refill would knock out must not carry a connection (rule 6: be conservative). A
    // hatched zone stored without a fill gets none.
    if (zone.hatched) {
      Rings stored;
      for (const auto& [l, pts] : zone.fills)
        if (l == layer && pts.size() >= 3) stored.push_back(pts);
      fill = geom::intersect(fill, geom::unite(stored));
    }
    return fill;
  }

  // Islands whose cluster holds no pad (CN_CLUSTER::IsOrphaned), removed per zone and layer by island_removal_mode,
  // unless every island of the zone on every layer is one (an unconnected pour is kept as drawn).
  int remove_islands(model::Board& out, const std::vector<std::uint8_t>& refilled) const {
    const CopperModel cm = build_copper(out);
    geom::Box all;
    for (const auto& it : cm.items) all.add(it.box);
    if (all.empty()) return 0;
    index::UniformGrid grid(all.inflated(1), 1'000'000, cm.items.size());
    for (std::size_t i = 0; i < cm.items.size(); ++i) grid.insert(static_cast<int>(i), cm.items[i].box);
    const ZoneFills fills(cm);
    const Connectivity con = compute_connectivity(out, cm, grid, true, &fills);
    std::set<int> with_pad;
    for (std::size_t i = 0; i < cm.items.size(); ++i)
      if (cm.items[i].kind == ItemKind::Pad) with_pad.insert(con.root[i]);
    // Orphaned fill polygons per zone, and per (zone, layer) whether all of its polygons are.
    std::vector<std::vector<int>> orphan(out.zones.size());
    std::vector<std::vector<int>> count(out.zones.size(), std::vector<int>(static_cast<std::size_t>(out.copper_count()), 0));
    std::vector<std::vector<int>> orphans(out.zones.size(), std::vector<int>(static_cast<std::size_t>(out.copper_count()), 0));
    for (std::size_t i = 0; i < cm.items.size(); ++i) {
      const auto& it = cm.items[i];
      if (it.kind != ItemKind::Zone || !refilled[static_cast<std::size_t>(it.index)]) continue;
      const auto z = static_cast<std::size_t>(it.index);
      const auto l = static_cast<std::size_t>(out.zones[z].fills[static_cast<std::size_t>(it.sub)].first);
      ++count[z][l];
      if (!with_pad.count(con.root[i])) {
        orphan[z].push_back(it.sub);
        ++orphans[z][l];
      }
    }
    int removed = 0;
    for (std::size_t z = 0; z < out.zones.size(); ++z) {
      if (orphan[z].empty()) continue;
      bool all_isolated = true;
      for (std::size_t l = 0; l < count[z].size(); ++l)
        if (count[z][l] > 0 && orphans[z][l] != count[z][l]) all_isolated = false;
      if (all_isolated) continue;
      auto& zone = out.zones[z];
      if (zone.island_removal == 1) continue;
      std::vector<std::uint8_t> drop(zone.fills.size(), 0);
      for (const int k : orphan[z]) {
        const auto& pts = zone.fills[static_cast<std::size_t>(k)].second;
        if (zone.island_removal == 2 && std::abs(geom::area(pts)) >= zone.island_area_min) continue;
        drop[static_cast<std::size_t>(k)] = 1;
        ++removed;
      }
      std::vector<std::pair<int, std::vector<Point>>> kept;
      for (std::size_t k = 0; k < zone.fills.size(); ++k)
        if (!drop[k]) kept.push_back(std::move(zone.fills[k]));
      zone.fills = std::move(kept);
    }
    return removed;
  }

  const model::Board& b_;
  RuleEngine re_;
  CopperModel cm_;
  std::map<int, int> pad_item_, pad_hole_;  // pad index -> copper item / hole
  Rings board_;
  Coord worst_ = 0;
  std::vector<std::vector<Rings>> fills_;  // by zone, by layer
  std::vector<std::string> warnings_;
};

}  // namespace

RefillResult refill_zones(const model::Board& b, const model::DesignRules& r) { return Filler(b, r).run(); }

std::vector<geom::Ring> thermal_spokes(const model::Pad& pad, const model::Zone& zone) {
  std::vector<Spoke> sp;
  spokes(pad, zone, sp);
  std::vector<geom::Ring> out;
  for (auto& s : sp) out.push_back(std::move(s.ring));
  return out;
}

namespace {

// Points where two closed rings cross (SHAPE_LINE_CHAIN::Intersect with aExcludeColinearAndTouching): proper
// crossings only, each rounded to the nanometre; `near` limits the first ring's edges to those meeting that box.
std::vector<Point> crossings(const Ring& a, const Ring& b, const geom::Box& near) {
  std::vector<Point> out;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const Point p = a[i], q = a[(i + 1) % a.size()];
    geom::Box eb;
    eb.add(p);
    eb.add(q);
    if (!eb.intersects(near)) continue;
    for (std::size_t j = 0; j < b.size(); ++j) {
      const Point c = b[j], d = b[(j + 1) % b.size()];
      const auto o1 = geom::orient(p, q, c), o2 = geom::orient(p, q, d), o3 = geom::orient(c, d, p), o4 = geom::orient(c, d, q);
      if (o1 == 0 || o2 == 0 || o3 == 0 || o4 == 0 || (o1 > 0) == (o2 > 0) || (o3 > 0) == (o4 > 0)) continue;
      const long double t = static_cast<long double>(o3) / static_cast<long double>(o3 - o4);
      out.push_back({geom::kiround(static_cast<double>(p.x + t * (q.x - p.x))), geom::kiround(static_cast<double>(p.y + t * (q.y - p.y)))});
    }
  }
  return out;
}

}  // namespace

std::vector<StarvedThermal> starved_thermals(const model::Board& b, const model::DesignRules& r) {
  std::vector<StarvedThermal> out;
  const int min_spokes = r.minimums.min_resolved_spokes;  // the board setup's MIN_RESOLVED_SPOKES (custom rules are not read)
  if (const auto sev = r.severities.find("starved_thermal"); min_spokes <= 0 || (sev != r.severities.end() && sev->second == "ignore")) return out;
  const CopperModel cm = build_copper(b);
  geom::Box all;
  for (const auto& it : cm.items) all.add(it.box);
  if (all.empty()) return out;
  index::UniformGrid grid(all.inflated(1), 1'000'000, cm.items.size());
  for (std::size_t i = 0; i < cm.items.size(); ++i) grid.insert(static_cast<int>(i), cm.items[i].box);
  const ZoneFills zf(cm);
  // Items each fill polygon touches directly (CN_ZONE_LAYER::HasSingleConnection counts its graph neighbours): items of
  // its net, and items of any net that can change net; other zones' fills of the net that overlap it.
  std::vector<int> touching(cm.items.size(), 0);
  std::map<std::pair<int, int>, int> fill_item;  // (zone, fill index) -> copper item
  for (std::size_t i = 0; i < cm.items.size(); ++i) {
    const auto& it = cm.items[i];
    if (it.kind == ItemKind::Zone) fill_item[{it.index, it.sub}] = static_cast<int>(i);
    if (it.kind != ItemKind::Zone && !in_item_graph(it)) continue;
    std::set<int> hit;
    for (const auto& s : it.shapes)
      for (const int z : zones_touching(cm, zf, grid, s, it.layers, 1))  // touching counts, as SHAPE::Collide at 0
        if (z != static_cast<int>(i) && (cm.items[static_cast<std::size_t>(z)].net == it.net || (it.kind != ItemKind::Zone && can_change_net(it)))) hit.insert(z);
    for (const int z : hit) ++touching[static_cast<std::size_t>(z)];
  }
  for (std::size_t pi = 0; pi < b.pads.size(); ++pi) {
    const auto& pad = b.pads[pi];
    if (pad.net <= 0) continue;
    for (std::size_t zi = 0; zi < b.zones.size(); ++zi) {
      const auto& zone = b.zones[zi];
      if (zone.rule_area || zone.teardrop || zone.net != pad.net || zone.fills.empty()) continue;
      if (zone_connection(b, pad, zone) != ZoneConnect::Thermal) continue;
      for (int layer = 0; layer < b.copper_count(); ++layer) {
        const model::LayerMask lb = model::layer_bit(layer);
        if (!(pad.copper & lb) || !(zone.copper & lb)) continue;
        // The pad grown by half its thermal gap: every spoke crosses this outline twice.
        const auto pad_copper = pad_shapes(pad);
        const Rings grown = geom::unite(shapes_rings(pad_copper, thermal_gap(pad, zone) / 2));
        if (grown.empty()) continue;
        const Ring& outline = grown.front();
        const geom::Box near = rings_box({outline});
        int spokes_ok = 0, island = 0;
        for (std::size_t k = 0; k < zone.fills.size(); ++k) {
          const auto& [l, pts] = zone.fills[k];
          if (l != layer || pts.size() < 3) continue;
          auto xs = crossings(pts, outline, near);
          std::sort(xs.begin(), xs.end(), [](Point a, Point c) { return std::pair(a.x, a.y) < std::pair(c.x, c.y); });
          xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
          if (xs.size() < 2) continue;
          const auto f = fill_item.find({static_cast<int>(zi), static_cast<int>(k)});
          const bool single = f != fill_item.end() && touching[static_cast<std::size_t>(f->second)] == 1;
          (single ? island : spokes_ok) += static_cast<int>(xs.size() / 2);
        }
        if (spokes_ok == 0 && island == 0) continue;  // not connected at all: the connectivity check's business
        if (spokes_ok < min_spokes) {
          // Tracks of the net from inside the outline into the fill are spokes too. KiCad takes every track connected
          // to the pad, on any layer, and tests only its far end against this layer's fill.
          for (const auto& t : b.tracks) {
            if (t.net != pad.net || !(pad.copper & model::layer_bit(t.layer))) continue;
            const auto seg = geom::Shape::segment(t.a, t.b, t.width / 2);
            if (std::none_of(pad_copper.begin(), pad_copper.end(), [&](const geom::Shape& ps) { return geom::closer_than(seg, ps, 1); })) continue;
            const bool a_in = geom::point_in_polygon(t.a, outline), b_in = geom::point_in_polygon(t.b, outline);
            if (a_in == b_in) continue;
            const Point other = a_in ? t.b : t.a;
            for (const auto& [l, pts] : zone.fills)
              if (l == layer && geom::point_in_polygon(other, pts)) {
                ++spokes_ok;
                break;
              }
          }
        }
        if (spokes_ok == 0) std::swap(spokes_ok, island);  // only islands: left to the connectivity check
        if (spokes_ok < min_spokes) out.push_back({static_cast<int>(pi), static_cast<int>(zi), layer, spokes_ok, island});
      }
    }
  }
  return out;
}

}  // namespace tmk::drc

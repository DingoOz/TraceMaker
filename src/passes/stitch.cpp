// SPDX-License-Identifier: GPL-3.0-or-later
#include "passes/stitch.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>

#include "drc/connectivity.hpp"
#include "drc/copper.hpp"
#include "drc/refill.hpp"
#include "route/obstacles.hpp"

namespace tmk::passes {
namespace {

using model::NetId;
using model::Point;

// A refill knocks a via out of other nets' fills at the clearance plus KiCad's extra margin (0.5 um) and arc error
// (5 um). Vias are placed this much clearer than the rules ask, so the knock-out of a via beside a fill never
// reaches it, and the hole of a via through a fill is no larger than the one the placement tested.
constexpr Coord kPlaceMargin = 10'000;

// Copper text is in the obstacle model as an estimated box per line, not as KiCad's glyphs (a descender or a
// trailing empty line can reach some tens of micrometres past it). A router's via rarely sits against text; a grid
// puts one wherever it fits, so stitching vias keep this much further away from the boxes than the clearance.
constexpr Coord kTextMargin = 250'000;

// Sixteen directions, x 1000, for the candidate sites around a signal via (integers: the same sites everywhere).
constexpr int kDirs[16][2] = {{1000, 0},  {924, 383},   {707, 707},   {383, 924},   {0, 1000},  {-383, 924},  {-707, 707},  {-924, 383},
                              {-1000, 0}, {-924, -383}, {-707, -707}, {-383, -924}, {0, -1000}, {383, -924},  {707, -707},  {924, -383}};

bool within(Point a, Point b, Coord d) {
  const geom::i128 dx = a.x - b.x, dy = a.y - b.y;
  return dx * dx + dy * dy <= static_cast<geom::i128>(d) * d;
}

// Fill polygons per zone.
std::vector<std::size_t> fill_pieces(const model::Board& b) {
  std::vector<std::size_t> n;
  for (const auto& z : b.zones) n.push_back(z.rule_area ? 0 : z.fills.size());
  return n;
}

}  // namespace

StitchResult stitch_vias(const model::Board& board, const model::DesignRules& rules, const StitchOptions& opt) {
  StitchResult res;
  for (const auto& r : opt.returns) res.returns.push_back({r.signal, r.plane, r.source, r.distance, 0, 0, 0});
  if (board.copper_count() < 2) return res;
  // The fills KiCad will draw for the routed board; the ones stored in the file may be older than the routing.
  auto rf = drc::refill_zones(board, rules);
  res.warnings = rf.warnings;
  const auto pieces_before = fill_pieces(rf.board);
  model::Board w = std::move(rf.board);
  route::Obstacles obs(w, rules);
  // A stitching via never lands in a surface-mount pad, its own net's included (solder would wick into the hole).
  obs.set_vias_off_pads(true, INT64_MAX);
  // Fills are judged by fills_allow below, not as plain obstacles: a via inside its own net's fill may pass
  // through the middle of another net's plane on a layer in between.
  obs.set_soft_zones(true);
  const drc::ZoneFills zf(obs.copper());
  const int nl = w.copper_count();

  // Solid fills per net and layer. Hatched zones are left alone: the refill does not redraw them.
  std::map<NetId, std::vector<std::vector<int>>> fills;  // net -> layer -> fill items
  for (std::size_t i = 0; i < obs.copper().items.size(); ++i) {
    const auto& it = obs.copper().items[i];
    if (it.kind != drc::ItemKind::Zone || it.net <= 0 || w.zones[static_cast<std::size_t>(it.index)].hatched) continue;
    auto& per = fills[it.net];
    per.resize(static_cast<std::size_t>(nl));
    for (int l = 0; l < nl; ++l)
      if (it.layers & model::layer_bit(l)) per[static_cast<std::size_t>(l)].push_back(static_cast<int>(i));
  }
  // Layers on which the disk (p, r) lies wholly inside a fill of `net`: centre inside, no fill edge within r.
  auto layers_inside = [&](NetId net, Point p, Coord r) {
    const auto f = fills.find(net);
    if (f == fills.end()) return 0;
    int n = 0;
    const geom::Shape centre = geom::Shape::point(p, 0);
    for (int l = 0; l < nl; ++l) {
      bool in = false;
      for (const int id : f->second[static_cast<std::size_t>(l)]) {
        const auto& it = obs.copper().items[static_cast<std::size_t>(id)];
        if (p.x - r < it.box.x0 || p.x + r > it.box.x1 || p.y - r < it.box.y0 || p.y + r > it.box.y1) continue;
        if (!zf.index[static_cast<std::size_t>(zf.slot[static_cast<std::size_t>(id)])].shape_closer(centre, 1)) continue;
        const auto& pts = it.shapes[0].pts;
        bool edge_near = false;
        for (std::size_t k = 0, j = pts.size() - 1; k < pts.size() && !edge_near; j = k++) edge_near = geom::point_seg_closer(p, pts[j], pts[k], r + 1);
        if (!edge_near) {
          in = true;
          break;
        }
      }
      n += in;
    }
    return n;
  };
  // What already ties a net's layers together: its through vias and plated through-hole pads.
  std::map<NetId, std::vector<Point>> ties;
  for (const auto& v : w.vias)
    if (v.net > 0 && v.layer_top != v.layer_bottom) ties[v.net].push_back(v.pos);
  for (const auto& p : w.pads)
    if (p.net > 0 && p.type == model::PadType::ThruHole) ties[p.net].push_back(p.pos);
  const auto ties_before = ties;
  auto tied = [](const std::map<NetId, std::vector<Point>>& t, NetId net, Point p, Coord d) {
    const auto it = t.find(net);
    if (it == t.end()) return false;
    for (const Point q : it->second)
      if (within(p, q, d)) return true;
    return false;
  };
  // The via a net's class gives, as the router sizes it (class values raised to the board minimums).
  auto via_for = [&](NetId net, Point p) {
    const auto& nc = rules.class_for(w.nets[static_cast<std::size_t>(net)].name);
    model::Via v;
    v.pos = p;
    v.drill = std::max(nc.via_drill, rules.minimums.through_hole_diameter);
    v.size = std::max({nc.via_diameter, rules.minimums.via_diameter, v.drill + 2 * rules.minimums.via_annular_width});
    v.layer_top = 0;
    v.layer_bottom = nl - 1;
    v.net = net;
    v.free = true;  // KiCad must not give the via another net when it touches no track
    return v;
  };
  auto spend = [&] {
    if (res.checks >= opt.work_limit) res.out_of_work = true;
    else ++res.checks;
    return !res.out_of_work;
  };
  // Other nets' fills. A through via may pass through another net's plane (KiCad's refill cuts the hole around
  // it), but only in the middle of solid copper: the whole knock-out, with the zone's minimum width as a web
  // around it, must lie inside the fill and away from every edge of it, so the hole cannot cut the fill in two,
  // sever a thermal spoke or leave a sliver. Anywhere else the fill counts as fixed copper: the via keeps the full
  // clearance. Hatched fills are never cut (the refill does not redraw them).
  Coord reach = std::max(obs.rules().max_clearance(), rules.minimums.hole_clearance);
  for (const auto& z : w.zones) reach = std::max(reach, z.clearance);  // a zone's own clearance widens its knock-outs
  reach += kPlaceMargin;
  std::vector<int> near;
  auto fills_allow = [&](NetId net, Point p, Coord r) {
    near.clear();
    const geom::Shape disk = geom::Shape::point(p, r);
    obs.grid().query(disk.box.inflated(reach + 1), [&](int id) {
      const auto& it = obs.copper().items[static_cast<std::size_t>(id)];
      if (it.kind == drc::ItemKind::Zone && it.net != net && !it.removed) near.push_back(id);
    });
    std::sort(near.begin(), near.end());
    for (const int id : near) {
      const auto& it = obs.copper().items[static_cast<std::size_t>(id)];
      const auto& index = zf.index[static_cast<std::size_t>(zf.slot[static_cast<std::size_t>(id)])];
      if (!index.shape_closer(disk, reach)) continue;  // clear of it by more than any clearance: the fill is not touched
      const auto& zone = w.zones[static_cast<std::size_t>(it.index)];
      if (zone.hatched || !index.shape_closer(geom::Shape::point(p, 0), 1)) return false;
      const Coord hole = r + reach + zone.min_thickness;
      const auto& pts = it.shapes[0].pts;
      for (std::size_t k = 0, j = pts.size() - 1; k < pts.size(); j = k++)
        if (geom::point_seg_closer(p, pts[j], pts[k], hole + 1)) return false;
    }
    return true;
  };
  // New vias keep apart by both knock-outs and a web, so two holes in one plane never merge. The web is the
  // largest minimum width of any zone: the refill prunes copper narrower than that, and a grid of holes whose
  // webs are pruned leaves a plane in loose pieces between them, which the refill then drops as islands.
  Coord web = 250'000;
  for (const auto& z : w.zones)
    if (!z.rule_area) web = std::max(web, z.min_thickness);
  auto apart = [&](Point p, Coord r) {
    for (const auto& v : res.vias)
      if (within(p, v.pos, 2 * (r + reach) + web)) return false;
    return true;
  };
  auto text_near = [&](Point p, Coord r) {
    const geom::Shape disk = geom::Shape::point(p, r);
    for (const auto& [layer, box] : obs.copper_texts())
      if (geom::closer_than(disk, box, reach + kTextMargin)) return true;
    return false;
  };
  // The exact check (rule 1) against everything but the fills, the fill test above, then the two-layer test; a via
  // that passes is entered in the model at once.
  auto try_place = [&](NetId net, Point p, bool return_path, bool* blocked) {
    const model::Via v = via_for(net, p);
    if (!obs.rules().via_allowed(net) || !spend()) return false;
    if (!apart(p, v.size / 2) || text_near(p, v.size / 2) || obs.via_state(p, v.size, v.drill, net, kPlaceMargin, false) != 0 ||
        !fills_allow(net, p, v.size / 2)) {
      if (blocked) *blocked = true;
      return false;
    }
    if (return_path && (!spend() || layers_inside(net, p, v.size / 2) < 2)) return false;
    w.vias.push_back(v);
    obs.add_via(static_cast<int>(w.vias.size()) - 1, 0);
    ties[net].push_back(p);
    res.vias.push_back(v);
    res.return_path.push_back(return_path);
    return true;
  };

  if (opt.planes && opt.pitch > 0) {
    geom::Box frame = w.edge_bbox();
    if (frame.empty()) frame = obs.bounds();
    for (const auto& [net, per] : fills) {
      int layers = 0;
      geom::Box area;
      for (const auto& ids : per) {
        layers += !ids.empty();
        for (const int id : ids) area.add(obs.copper().items[static_cast<std::size_t>(id)].box);
      }
      if (layers < 2 || area.empty()) continue;
      const Coord r = via_for(net, {}).size / 2;
      // One grid for the whole board, anchored at the outline's corner, so two nets' stitches never interleave oddly.
      const Coord x0 = frame.x0 + opt.pitch / 2, y0 = frame.y0 + opt.pitch / 2;
      auto first = [&](Coord lo, Coord o) { return lo <= o ? Coord{0} : (lo - o + opt.pitch - 1) / opt.pitch; };
      for (Coord j = first(area.y0, y0); y0 + j * opt.pitch <= area.y1 && !res.out_of_work; ++j)
        for (Coord i = first(area.x0, x0); x0 + i * opt.pitch <= area.x1; ++i) {
          const Point p{x0 + i * opt.pitch, y0 + j * opt.pitch};
          if (!spend()) break;
          if (layers_inside(net, p, r) < 2) continue;
          ++res.sites;
          if (tied(ties, net, p, opt.pitch / 2)) {
            ++res.site_served;
            continue;
          }
          bool blocked = false;
          if (!try_place(net, p, false, &blocked) && blocked) ++res.site_blocked;
        }
    }
  }

  for (std::size_t k = 0; k < opt.returns.size(); ++k) {
    const auto& rn = opt.returns[k];
    auto& out = res.returns[k];
    if (rn.signal <= 0 || rn.plane <= 0 || rn.signal == rn.plane || rn.distance <= 0) continue;
    std::vector<const model::Via*> sv;
    for (const auto& v : board.vias)
      if (v.net == rn.signal) sv.push_back(&v);
    std::sort(sv.begin(), sv.end(), [](const model::Via* a, const model::Via* b) { return std::tie(a->pos.x, a->pos.y) < std::tie(b->pos.x, b->pos.y); });
    const model::Via proto = via_for(rn.plane, {});
    const Coord gap = std::max(rules.class_for(w.nets[static_cast<std::size_t>(rn.signal)].name).clearance,
                               rules.class_for(w.nets[static_cast<std::size_t>(rn.plane)].name).clearance);
    for (const model::Via* v : sv) {
      ++out.signal_vias;
      out.stitched_before += tied(ties_before, rn.plane, v->pos, rn.distance);
      if (tied(ties, rn.plane, v->pos, rn.distance)) {
        ++out.stitched_after;
        continue;
      }
      // Rings around the signal via, nearest first: the closer the return via, the smaller the loop.
      const Coord r0 = v->size / 2 + proto.size / 2 + gap + kPlaceMargin, step = std::max<Coord>(proto.size / 2, 100'000);
      bool placed = false;
      for (Coord r = r0; r <= rn.distance && !placed && !res.out_of_work; r += step)
        for (int d = 0; d < 16 && !placed && !res.out_of_work; ++d) {
          Point p{v->pos.x + r * kDirs[d][0] / 1000, v->pos.y + r * kDirs[d][1] / 1000};
          p.x -= p.x % 1'000;  // whole micrometres
          p.y -= p.y % 1'000;
          if (!within(p, v->pos, rn.distance)) continue;
          placed = try_place(rn.plane, p, true, nullptr);
        }
      out.stitched_after += placed;
    }
  }

  // Second refill, with the vias (rule 1 for the fills): a via stays only if its own net's fill meets it on two
  // layers. Removing one changes the fills again, so repeat until nothing is dropped.
  for (int round = 0; !res.vias.empty(); ++round) {
    model::Board t = board;
    t.vias.insert(t.vias.end(), res.vias.begin(), res.vias.end());
    const auto rf2 = drc::refill_zones(t, rules);
    const auto cm = drc::build_copper(rf2.board);
    geom::Box all;
    for (const auto& it : cm.items) all.add(it.box);
    index::UniformGrid grid(all.inflated(1'000'000), 1'000'000, cm.items.size());
    for (std::size_t i = 0; i < cm.items.size(); ++i) grid.insert(static_cast<int>(i), cm.items[i].box);
    const drc::ZoneFills zf2(cm);
    std::vector<model::Via> keep;
    std::vector<std::uint8_t> keep_kind;
    for (std::size_t i = 0; i < res.vias.size(); ++i) {
      const auto& v = res.vias[i];
      int joined = 0;
      const geom::Shape disk = geom::Shape::point(v.pos, v.size / 2);
      for (int l = v.layer_top; l <= v.layer_bottom; ++l) {
        bool own = false;
        for (const int id : drc::zones_touching(cm, zf2, grid, disk, model::layer_bit(l))) own = own || cm.items[static_cast<std::size_t>(id)].net == v.net;
        joined += own;
      }
      if (joined >= 2) keep.push_back(v), keep_kind.push_back(res.return_path[i]);
    }
    const bool stable = keep.size() == res.vias.size();
    // A zone of another net in more pieces than before the vias: something was cut. The placement rules should
    // make that impossible; if it happens, the whole pass is withdrawn rather than guessing which via did it
    // (rule 4). A stitched net's own zone may gain a piece: an island the refill used to drop for want of a
    // connection, which a via now ties to its net.
    bool cut = false;
    if (stable) {
      const auto pieces_after = fill_pieces(rf2.board);
      std::set<NetId> stitched;
      for (const auto& v : res.vias) stitched.insert(v.net);
      for (std::size_t z = 0; z < pieces_after.size() && z < pieces_before.size(); ++z)
        cut = cut || (pieces_after[z] > pieces_before[z] && !stitched.count(board.zones[z].net));
    }
    if (cut || (!stable && round >= 3)) {
      res.discarded = true;
      res.dropped += static_cast<int>(res.vias.size());
      keep.clear();
      keep_kind.clear();
    } else {
      res.dropped += static_cast<int>(res.vias.size() - keep.size());
    }
    res.vias = std::move(keep);
    res.return_path = std::move(keep_kind);
    if (stable || res.discarded) break;
  }
  if (res.dropped > 0 || res.discarded) {
    // Signal vias whose stitch was withdrawn are no longer stitched.
    std::map<NetId, std::vector<Point>> now = ties_before;
    for (const auto& v : res.vias) now[v.net].push_back(v.pos);
    for (std::size_t k = 0; k < opt.returns.size(); ++k) {
      const auto& rn = opt.returns[k];
      res.returns[k].stitched_after = 0;
      for (const auto& v : board.vias)
        if (v.net == rn.signal && rn.signal > 0) res.returns[k].stitched_after += tied(now, rn.plane, v.pos, rn.distance);
    }
  }
  return res;
}

}  // namespace tmk::passes

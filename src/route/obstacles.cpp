#include "route/obstacles.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace tmk::route {

using geom::Point;
using geom::Shape;

Obstacles::Obstacles(model::Board& board, const model::DesignRules& rules) : b_(board), r_(rules), cm_(drc::build_copper(board)) {
  re_ = std::make_unique<drc::RuleEngine>(b_, r_, cm_);
  reach_ = std::max<Coord>(re_->max_clearance(), 1'000'000) + 2'000'000;  // clearance + generous track/via size
  bounds_ = b_.edge_bbox();
  for (const auto& it : cm_.items) bounds_.add(it.box);
  if (bounds_.empty()) bounds_ = geom::Box{0, 0, 100'000'000, 100'000'000};
  bounds_ = bounds_.inflated(5'000'000);
  const Coord cell = 1'000'000;
  grid_ = std::make_unique<index::UniformGrid>(bounds_, cell, cm_.items.size() + 1024);
  for (std::size_t i = 0; i < cm_.items.size(); ++i) grid_->insert(static_cast<int>(i), cm_.items[i].box);
  rgrid_ = std::make_unique<index::UniformGrid>(bounds_, cell, 1024);
  hgrid_ = std::make_unique<index::UniformGrid>(bounds_, cell, cm_.holes.size() + 1024);
  for (std::size_t i = 0; i < cm_.holes.size(); ++i) hgrid_->insert(static_cast<int>(i), cm_.holes[i].shape.box);
  // Board edge as individual segments in a grid, so edge tests cost O(nearby segments).
  for (const auto& e : cm_.edges)
    for (std::size_t k = 0; k + 1 < e.pts.size(); ++k) edge_segs_.push_back(Shape::segment(e.pts[k], e.pts[k + 1], 0));
  egrid_ = std::make_unique<index::UniformGrid>(bounds_, cell, edge_segs_.size() + 16);
  for (std::size_t i = 0; i < edge_segs_.size(); ++i) egrid_->insert(static_cast<int>(i), edge_segs_[i].box);
  // Board outline: chain all Edge.Cuts pieces into closed loops; the loop with the largest area is the
  // outline, the others are cut-outs. If any pad centre falls outside, the outline is not trusted (edge
  // clearance still applies through the edge segments).
  {
    std::vector<std::vector<Point>> pieces;
    for (const auto& e : cm_.edges) pieces.push_back(e.pts);
    auto near = [](Point a, Point c) { return std::llabs(a.x - c.x) < 2000 && std::llabs(a.y - c.y) < 2000; };
    std::vector<std::uint8_t> used(pieces.size(), 0);
    std::vector<std::vector<Point>> loops;
    for (std::size_t s0 = 0; s0 < pieces.size(); ++s0) {
      if (used[s0] || pieces[s0].size() < 2) continue;
      used[s0] = 1;
      std::vector<Point> chain = pieces[s0];
      for (bool grown = true; grown && !near(chain.front(), chain.back());) {
        grown = false;
        for (std::size_t k = 0; k < pieces.size(); ++k) {
          if (used[k] || pieces[k].size() < 2) continue;
          if (near(chain.back(), pieces[k].front())) chain.insert(chain.end(), pieces[k].begin() + 1, pieces[k].end());
          else if (near(chain.back(), pieces[k].back())) chain.insert(chain.end(), pieces[k].rbegin() + 1, pieces[k].rend());
          else continue;
          used[k] = 1;
          grown = true;
          break;
        }
      }
      if (chain.size() >= 4 && near(chain.front(), chain.back())) loops.push_back(std::move(chain));
    }
    auto area = [](const std::vector<Point>& l) {
      long double a = 0;
      for (std::size_t i = 0, j = l.size() - 1; i < l.size(); j = i++)
        a += static_cast<long double>(l[j].x) * static_cast<long double>(l[i].y) - static_cast<long double>(l[i].x) * static_cast<long double>(l[j].y);
      return std::fabs(a) / 2;
    };
    std::size_t best = loops.size();
    for (std::size_t i = 0; i < loops.size(); ++i)
      if (best == loops.size() || area(loops[i]) > area(loops[best])) best = i;
    if (best < loops.size()) {
      outline_ = loops[best];
      for (std::size_t i = 0; i < loops.size(); ++i)
        if (i != best) cutouts_.push_back(loops[i]);
      for (const auto& p : b_.pads)
        if (!geom::point_in_polygon(p.pos, outline_)) {
          outline_.clear();
          cutouts_.clear();
          break;
        }
    }
  }
  for (const auto& z : b_.zones)
    if (z.rule_area && z.keepout_tracks && !z.outline.empty() && z.outline.front().size() >= 3)
      keepouts_.emplace_back(Shape::polygon(z.outline.front(), 0), &z);
}

bool Obstacles::inside_board(Point p, Coord margin) const {
  if (outline_.empty()) return true;
  if (!geom::point_in_polygon(p, outline_)) return false;
  for (const auto& c : cutouts_)
    if (geom::point_in_polygon(p, c)) return false;
  if (margin <= 0) return true;
  const Shape pt = Shape::point(p, 0);
  bool ok = true;
  egrid_->query(pt.box.inflated(margin + 1), [&](int id) {
    if (ok && geom::closer_than(pt, edge_segs_[static_cast<std::size_t>(id)], margin)) ok = false;
  });
  return ok;
}

int Obstacles::copper_state(const Shape& s, const drc::CopperItem& probe, int layer, bool ignore_routed, std::vector<int>* owners) const {
  int state = 0;
  grid_->query(s.box.inflated(re_->max_clearance() + 1), [&](int id) {
    if (state == 2) return;
    const auto& it = cm_.items[static_cast<std::size_t>(id)];
    if (it.removed || !(it.layers & model::layer_bit(layer))) return;
    if (it.net == probe.net && probe.net != 0) return;
    Coord req = re_->clearance(probe, it, layer);
    // Keep new copper out of foreign pads' solder-mask openings on outer layers (KiCad solder_mask_bridge).
    if (it.kind == drc::ItemKind::Pad && (layer == 0 || layer == b_.copper_count() - 1)) {
      const auto& pad = b_.pads[static_cast<std::size_t>(it.index)];
      const char* mask = layer == 0 ? "F.Mask" : "B.Mask";
      bool has_mask = false;
      for (const auto& ln : pad.layers)
        if (ln == mask || ln == "*.Mask" || ln == "F&B.Mask") has_mask = true;
      if (has_mask) {
        Coord mm = pad.mask_margin;
        if (mm == INT64_MIN) mm = b_.footprints[static_cast<std::size_t>(pad.footprint)].mask_margin;
        if (mm == INT64_MIN) mm = b_.pad_to_mask_clearance;
        req = std::max(req, mm + 1'000);
      }
    }
    for (const auto& u : it.shapes)
      if (geom::closer_than(s, u, req)) {
        if (ignore_routed && it.owner >= 0) {
          state = 1;
          if (owners) owners->push_back(it.owner);
        } else {
          state = 2;
        }
        return;
      }
  });
  return state;
}

int Obstacles::holes_edges_state(const Shape& s, model::NetId net, int layer, bool is_via_hole, Coord hole_r, bool ignore_routed,
                                 std::vector<int>* owners) const {
  // Copper (or a new via's hole) against other items' holes.
  int state = 0;
  const Coord hc = std::max<Coord>(r_.minimums.hole_clearance, 0);
  const Coord h2h = std::max<Coord>(r_.minimums.hole_to_hole, 0);
  hgrid_->query(s.box.inflated(std::max(hc, h2h) + 1), [&](int id) {
    if (state == 2) return;
    const auto& h = cm_.holes[static_cast<std::size_t>(id)];
    if (h.removed) return;
    bool hit = false;
    if (!(h.net == net && net != 0 && h.plated) && geom::closer_than(s, h.shape, hc)) hit = true;
    if (!hit && is_via_hole) {
      const Shape hole = Shape::point(s.pts[0], hole_r);
      if (geom::closer_than(hole, h.shape, h2h) || hole.pts[0] == h.shape.pts[0]) hit = true;
    }
    if (!hit) return;
    const int owner = h.item >= 0 ? cm_.items[static_cast<std::size_t>(h.item)].owner : -1;
    if (ignore_routed && owner >= 0) {
      state = 1;
      if (owners) owners->push_back(owner);
    } else {
      state = 2;
    }
  });
  if (state == 2) return 2;
  // Board edge.
  const Coord ec = std::max<Coord>(r_.minimums.copper_edge_clearance, 0);
  bool edge_ok = true;
  egrid_->query(s.box.inflated(ec + 1), [&](int id) {
    if (edge_ok && geom::closer_than(s, edge_segs_[static_cast<std::size_t>(id)], ec)) edge_ok = false;
  });
  if (!edge_ok) return 2;
  // Keepouts.
  for (const auto& [area, z] : keepouts_)
    if ((z->copper & model::layer_bit(layer)) && geom::closer_than(s, area, 1)) return 2;
  return state;
}

namespace {
drc::CopperItem make_probe(drc::ItemKind kind, const Shape& s, model::NetId net, int layer, Coord width, Point pos) {
  drc::CopperItem probe;
  probe.kind = kind;
  probe.net = net;
  probe.layers = layer >= 0 ? model::layer_bit(layer) : 0;
  probe.width = width;
  probe.shapes = {s};
  probe.box = s.box;
  probe.pos = pos;
  return probe;
}
int worst(int a, int c) { return std::max(a, c); }
}  // namespace

int Obstacles::disk_state(Point p, int layer, Coord hw, model::NetId net, Coord margin, bool ignore_routed, std::vector<int>* owners) const {
  ++checks;
  if (!inside_board(p, 0)) {
    ++rej_outside;
    return 2;
  }
  const Shape s = Shape::point(p, hw + margin);
  const auto probe = make_probe(drc::ItemKind::Track, s, net, layer, 2 * hw, p);
  int st = copper_state(s, probe, layer, ignore_routed, owners);
  if (st == 2) {
    ++rej_copper;
    return 2;
  }
  st = worst(st, holes_edges_state(s, net, layer, false, 0, ignore_routed, owners));
  if (st == 2) ++rej_other;
  return st;
}

int Obstacles::segment_state(Point a, Point b, int layer, Coord width, model::NetId net, bool ignore_routed, std::vector<int>* owners) const {
  const Shape s = Shape::segment(a, b, width / 2);
  const auto probe = make_probe(drc::ItemKind::Track, s, net, layer, width, a);
  const int st = copper_state(s, probe, layer, ignore_routed, owners);
  if (st == 2) return 2;
  return worst(st, holes_edges_state(s, net, layer, false, 0, ignore_routed, owners));
}

int Obstacles::via_state(Point p, Coord d, Coord drill, model::NetId net, Coord margin, bool ignore_routed, std::vector<int>* owners) const {
  if (!inside_board(p, 0)) return 2;
  const Shape s = Shape::point(p, d / 2 + margin);
  int st = 0;
  for (int l = 0; l < b_.copper_count() && st != 2; ++l) {
    const auto probe = make_probe(drc::ItemKind::Via, s, net, l, d, p);
    st = worst(st, copper_state(s, probe, l, ignore_routed, owners));
    if (st != 2) st = worst(st, holes_edges_state(s, net, l, l == 0, drill / 2 + margin, ignore_routed, owners));
  }
  return st;
}

int Obstacles::add_track(int index, int owner) {
  const auto& t = b_.tracks[static_cast<std::size_t>(index)];
  drc::CopperItem it;
  it.kind = drc::ItemKind::Track;
  it.index = index;
  it.net = t.net;
  it.layers = model::layer_bit(t.layer);
  it.shapes = {Shape::segment(t.a, t.b, t.width / 2)};
  it.pos = t.a;
  it.width = t.width;
  it.box = it.shapes[0].box;
  it.owner = owner;
  const int id = static_cast<int>(cm_.items.size());
  grid_->insert(id, it.box);
  rgrid_->insert(id, it.box);
  cm_.items.push_back(std::move(it));
  return id;
}

int Obstacles::add_via(int index, int owner) {
  const auto& v = b_.vias[static_cast<std::size_t>(index)];
  drc::CopperItem it;
  it.kind = drc::ItemKind::Via;
  it.index = index;
  it.net = v.net;
  for (int l = v.layer_top; l <= v.layer_bottom; ++l) it.layers |= model::layer_bit(l);
  it.shapes = {Shape::point(v.pos, v.size / 2)};
  it.pos = v.pos;
  it.width = v.size;
  it.box = it.shapes[0].box;
  drc::Hole h;
  h.shape = Shape::point(v.pos, v.drill / 2);
  h.item = static_cast<int>(cm_.items.size());
  h.via = index;
  h.net = v.net;
  h.pos = v.pos;
  it.owner = owner;
  const int id = static_cast<int>(cm_.items.size());
  grid_->insert(id, it.box);
  rgrid_->insert(id, it.box);
  cm_.items.push_back(std::move(it));
  hgrid_->insert(static_cast<int>(cm_.holes.size()), h.shape.box);
  cm_.holes.push_back(std::move(h));
  return id;
}

void Obstacles::remove_item(int item) {
  auto& it = cm_.items[static_cast<std::size_t>(item)];
  if (it.removed) return;
  it.removed = true;
  grid_->erase(item, it.box);
  rgrid_->erase(item, it.box);
  if (it.kind == drc::ItemKind::Via)
    for (auto& h : cm_.holes)
      if (h.item == item) h.removed = true;
}

std::int32_t Obstacles::fixed_code(Point p, int layer, Coord hw, Coord margin, model::NetId probe_net) const {
  if (!inside_board(p, 0)) return kBlocked;
  const Shape s = Shape::point(p, hw + margin);
  const auto probe = make_probe(drc::ItemKind::Track, s, probe_net, layer, 2 * hw, p);
  std::int32_t code = kFree;
  auto add_net = [&](model::NetId n) {
    if (n == 0) code = kBlocked;
    else if (code == kFree) code = n;
    else if (code != n) code = kBlocked;
  };
  grid_->query(s.box.inflated(re_->max_clearance() + 1), [&](int id) {
    if (code == kBlocked) return;
    const auto& it = cm_.items[static_cast<std::size_t>(id)];
    if (it.owner >= 0 || it.removed || !(it.layers & model::layer_bit(layer))) return;
    if (it.net != 0 && code == it.net) return;  // already known: only legal for this net
    Coord req = re_->clearance(probe, it, layer);
    if (it.kind == drc::ItemKind::Pad && (layer == 0 || layer == b_.copper_count() - 1)) {
      const auto& pad = b_.pads[static_cast<std::size_t>(it.index)];
      const char* mask = layer == 0 ? "F.Mask" : "B.Mask";
      bool has_mask = false;
      for (const auto& ln : pad.layers)
        if (ln == mask || ln == "*.Mask" || ln == "F&B.Mask") has_mask = true;
      if (has_mask) {
        Coord mm = pad.mask_margin;
        if (mm == INT64_MIN) mm = b_.footprints[static_cast<std::size_t>(pad.footprint)].mask_margin;
        if (mm == INT64_MIN) mm = b_.pad_to_mask_clearance;
        req = std::max(req, mm + 1'000);
      }
    }
    for (const auto& u : it.shapes)
      if (geom::closer_than(s, u, req)) {
        add_net(it.net);
        return;
      }
  });
  if (code == kBlocked) return code;
  const Coord hc = std::max<Coord>(r_.minimums.hole_clearance, 0);
  hgrid_->query(s.box.inflated(hc + 1), [&](int id) {
    if (code == kBlocked) return;
    const auto& h = cm_.holes[static_cast<std::size_t>(id)];
    if (h.removed || (h.item >= 0 && cm_.items[static_cast<std::size_t>(h.item)].owner >= 0)) return;
    if (geom::closer_than(s, h.shape, hc)) {
      if (h.plated && h.net != 0) add_net(h.net);
      else code = kBlocked;
    }
  });
  if (code == kBlocked) return code;
  const Coord ec = std::max<Coord>(r_.minimums.copper_edge_clearance, 0);
  bool edge_ok = true;
  egrid_->query(s.box.inflated(ec + 1), [&](int id) {
    if (edge_ok && geom::closer_than(s, edge_segs_[static_cast<std::size_t>(id)], ec)) edge_ok = false;
  });
  if (!edge_ok) return kBlocked;
  for (const auto& [area, z] : keepouts_)
    if ((z->copper & model::layer_bit(layer)) && geom::closer_than(s, area, 1)) return kBlocked;
  return code;
}

std::int32_t Obstacles::fixed_via_code(Point p, Coord d, Coord drill, Coord margin, model::NetId probe_net) const {
  std::int32_t code = kFree;
  for (int l = 0; l < b_.copper_count(); ++l) {
    const std::int32_t c = fixed_code(p, l, d / 2, margin, probe_net);
    if (c == kBlocked) return kBlocked;
    if (c != kFree) {
      if (code == kFree) code = c;
      else if (code != c) return kBlocked;
    }
  }
  // Hole to hole against fixed holes (any net).
  const Coord h2h = std::max<Coord>(r_.minimums.hole_to_hole, 0);
  const Shape hole = Shape::point(p, drill / 2 + margin);
  bool ok = true;
  hgrid_->query(hole.box.inflated(h2h + 1), [&](int id) {
    const auto& h = cm_.holes[static_cast<std::size_t>(id)];
    if (!ok || h.removed || (h.item >= 0 && cm_.items[static_cast<std::size_t>(h.item)].owner >= 0)) return;
    if (geom::closer_than(hole, h.shape, h2h) || hole.pts[0] == h.shape.pts[0]) ok = false;
  });
  return ok ? code : kBlocked;
}

int Obstacles::routed_state(const Shape& s, int layer, model::NetId net, drc::ItemKind kind, bool soft, std::vector<int>* owners,
                            bool via_hole, Coord hole_r) const {
  const auto probe = make_probe(kind, s, net, layer, 2 * s.r, s.pts[0]);
  int state = 0;
  rgrid_->query(s.box.inflated(re_->max_clearance() + 1), [&](int id) {
    if (state == 2) return;
    const auto& it = cm_.items[static_cast<std::size_t>(id)];
    if (it.removed || !(it.layers & model::layer_bit(layer))) return;
    if (it.net == net && net != 0) return;
    const Coord req = re_->clearance(probe, it, layer);
    for (const auto& u : it.shapes)
      if (geom::closer_than(s, u, req)) {
        if (soft) {
          state = 1;
          if (owners) owners->push_back(it.owner);
        } else {
          state = 2;
        }
        return;
      }
  });
  if (state == 2 || !via_hole) return state;
  // A new via hole against routed vias' holes (hole to hole).
  const Coord h2h = std::max<Coord>(r_.minimums.hole_to_hole, 0);
  const Shape hole = Shape::point(s.pts[0], hole_r);
  rgrid_->query(hole.box.inflated(h2h + 1), [&](int id) {
    if (state == 2) return;
    const auto& it = cm_.items[static_cast<std::size_t>(id)];
    if (it.removed || it.kind != drc::ItemKind::Via) return;
    const Shape other = Shape::point(it.pos, b_.vias[static_cast<std::size_t>(it.index)].drill / 2);
    if (geom::closer_than(hole, other, h2h)) {
      if (soft) {
        state = 1;
        if (owners) owners->push_back(it.owner);
      } else {
        state = 2;
      }
    }
  });
  return state;
}

}  // namespace tmk::route

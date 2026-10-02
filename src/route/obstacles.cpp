#include "route/obstacles.hpp"

#include <algorithm>
#include <bit>

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
  hgrid_ = std::make_unique<index::UniformGrid>(bounds_, cell, cm_.holes.size() + 1024);
  for (std::size_t i = 0; i < cm_.holes.size(); ++i) hgrid_->insert(static_cast<int>(i), cm_.holes[i].shape.box);
  // Board edge as individual segments in a grid, so edge tests cost O(nearby segments).
  for (const auto& e : cm_.edges)
    for (std::size_t k = 0; k + 1 < e.pts.size(); ++k) edge_segs_.push_back(Shape::segment(e.pts[k], e.pts[k + 1], 0));
  egrid_ = std::make_unique<index::UniformGrid>(bounds_, cell, edge_segs_.size() + 16);
  for (std::size_t i = 0; i < edge_segs_.size(); ++i) egrid_->insert(static_cast<int>(i), edge_segs_[i].box);
  // Board outline: the Edge.Cuts piece with the largest box is used for "inside the board" tests.
  double best = -1;
  for (const auto& e : cm_.edges) {
    const double area = static_cast<double>(e.box.x1 - e.box.x0) * static_cast<double>(e.box.y1 - e.box.y0);
    if (e.pts.size() >= 4 && area > best && e.pts.front() == e.pts.back()) {
      best = area;
      outline_ = e.pts;
    }
  }
  if (outline_.empty()) {
    // Outline made of separate lines/arcs: chain them end to end.
    std::vector<std::vector<Point>> pieces;
    for (const auto& e : cm_.edges) pieces.push_back(e.pts);
    if (!pieces.empty()) {
      std::vector<Point> chain = pieces.front();
      std::vector<std::uint8_t> used(pieces.size(), 0);
      used[0] = 1;
      for (bool grown = true; grown;) {
        grown = false;
        for (std::size_t k = 0; k < pieces.size(); ++k) {
          if (used[k] || pieces[k].empty()) continue;
          auto near = [](Point a, Point c) { return std::llabs(a.x - c.x) < 2000 && std::llabs(a.y - c.y) < 2000; };
          if (near(chain.back(), pieces[k].front())) {
            chain.insert(chain.end(), pieces[k].begin() + 1, pieces[k].end());
          } else if (near(chain.back(), pieces[k].back())) {
            chain.insert(chain.end(), pieces[k].rbegin() + 1, pieces[k].rend());
          } else {
            continue;
          }
          used[k] = 1;
          grown = true;
        }
      }
      if (chain.size() >= 4 && std::llabs(chain.front().x - chain.back().x) < 2000 && std::llabs(chain.front().y - chain.back().y) < 2000)
        outline_ = std::move(chain);
    }
  }
  for (const auto& z : b_.zones)
    if (z.rule_area && z.keepout_tracks && !z.outline.empty() && z.outline.front().size() >= 3)
      keepouts_.emplace_back(Shape::polygon(z.outline.front(), 0), &z);
}

bool Obstacles::inside_board(Point p, Coord margin) const {
  if (outline_.empty()) return true;
  if (!geom::point_in_polygon(p, outline_)) return false;
  if (margin <= 0) return true;
  const Shape pt = Shape::point(p, 0);
  bool ok = true;
  egrid_->query(pt.box.inflated(margin + 1), [&](int id) {
    if (ok && geom::closer_than(pt, edge_segs_[static_cast<std::size_t>(id)], margin)) ok = false;
  });
  return ok;
}

bool Obstacles::copper_ok(const Shape& s, const drc::CopperItem& probe, int layer) const {
  bool ok = true;
  grid_->query(s.box.inflated(re_->max_clearance() + 1), [&](int id) {
    if (!ok) return;
    const auto& it = cm_.items[static_cast<std::size_t>(id)];
    if (!(it.layers & model::layer_bit(layer))) return;
    if (it.net == probe.net && probe.net != 0) return;
    const Coord req = re_->clearance(probe, it, layer);
    for (const auto& u : it.shapes)
      if (geom::closer_than(s, u, req)) {
        ok = false;
        return;
      }
  });
  return ok;
}

bool Obstacles::holes_edges_ok(const Shape& s, model::NetId net, int layer, bool is_via_hole, Coord hole_r) const {
  // Copper (or a new via's hole) against other items' holes.
  bool ok = true;
  const Coord hc = std::max<Coord>(r_.minimums.hole_clearance, 0);
  const Coord h2h = std::max<Coord>(r_.minimums.hole_to_hole, 0);
  hgrid_->query(s.box.inflated(std::max(hc, h2h) + 1), [&](int id) {
    if (!ok) return;
    const auto& h = cm_.holes[static_cast<std::size_t>(id)];
    if (h.net == net && net != 0 && h.plated) return;
    if (geom::closer_than(s, h.shape, hc)) ok = false;
    if (ok && is_via_hole) {
      const Shape hole = Shape::point(s.pts[0], hole_r);
      if (geom::closer_than(hole, h.shape, h2h) || hole.pts[0] == h.shape.pts[0]) ok = false;
    }
  });
  if (!ok) return false;
  // Board edge.
  const Coord ec = std::max<Coord>(r_.minimums.copper_edge_clearance, 0);
  bool edge_ok = true;
  egrid_->query(s.box.inflated(ec + 1), [&](int id) {
    if (edge_ok && geom::closer_than(s, edge_segs_[static_cast<std::size_t>(id)], ec)) edge_ok = false;
  });
  if (!edge_ok) return false;
  // Keepouts.
  for (const auto& [area, z] : keepouts_)
    if ((z->copper & model::layer_bit(layer)) && geom::closer_than(s, area, 1)) return false;
  return true;
}

bool Obstacles::disk_ok(Point p, int layer, Coord hw, model::NetId net, Coord margin) const {
  ++checks;
  if (!inside_board(p, 0)) {
    ++rej_outside;
    return false;
  }
  const Shape s = Shape::point(p, hw + margin);
  drc::CopperItem probe;
  probe.kind = drc::ItemKind::Track;
  probe.net = net;
  probe.layers = model::layer_bit(layer);
  probe.width = 2 * hw;
  probe.shapes = {s};
  probe.box = s.box;
  probe.pos = p;
  if (!copper_ok(s, probe, layer)) {
    ++rej_copper;
    return false;
  }
  if (!holes_edges_ok(s, net, layer, false, 0)) {
    ++rej_other;
    return false;
  }
  return true;
}

bool Obstacles::segment_ok(Point a, Point b, int layer, Coord width, model::NetId net) const {
  const Shape s = Shape::segment(a, b, width / 2);
  drc::CopperItem probe;
  probe.kind = drc::ItemKind::Track;
  probe.net = net;
  probe.layers = model::layer_bit(layer);
  probe.width = width;
  probe.shapes = {s};
  probe.box = s.box;
  probe.pos = a;
  return copper_ok(s, probe, layer) && holes_edges_ok(s, net, layer, false, 0);
}

bool Obstacles::via_ok(Point p, Coord d, Coord drill, model::NetId net, Coord margin) const {
  if (!inside_board(p, 0)) return false;
  const Shape s = Shape::point(p, d / 2 + margin);
  drc::CopperItem probe;
  probe.kind = drc::ItemKind::Via;
  probe.net = net;
  probe.width = d;
  probe.shapes = {s};
  probe.box = s.box;
  probe.pos = p;
  for (int l = 0; l < b_.copper_count(); ++l) {
    probe.layers = model::layer_bit(l);
    if (!copper_ok(s, probe, l)) return false;
    if (!holes_edges_ok(s, net, l, l == 0, drill / 2 + margin)) return false;
  }
  return true;
}

void Obstacles::add_track(int index) {
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
  grid_->insert(static_cast<int>(cm_.items.size()), it.box);
  cm_.items.push_back(std::move(it));
}

void Obstacles::add_via(int index) {
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
  grid_->insert(static_cast<int>(cm_.items.size()), it.box);
  cm_.items.push_back(std::move(it));
  hgrid_->insert(static_cast<int>(cm_.holes.size()), h.shape.box);
  cm_.holes.push_back(std::move(h));
}

}  // namespace tmk::route

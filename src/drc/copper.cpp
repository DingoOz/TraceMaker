// SPDX-License-Identifier: GPL-3.0-or-later
#include "drc/copper.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace tmk::drc {

using geom::Point;
using geom::Shape;

const char* kind_name(ItemKind k) {
  switch (k) {
    case ItemKind::Pad: return "Pad";
    case ItemKind::Track: return "Track";
    case ItemKind::Arc: return "Arc";
    case ItemKind::Via: return "Via";
    case ItemKind::Zone: return "Zone";
    case ItemKind::Graphic: return "Graphic";
  }
  return "?";
}

std::vector<Shape> pad_shapes(const model::Pad& p) {
  const Coord w = p.size_x, h = p.size_y;
  // KiCad's drill "offset" moves the copper shape away from the hole; the pad position is the hole centre.
  const Point centre = p.pos + geom::rotate(p.drill_offset, p.angle);
  auto tf = [&](Point q) { return centre + geom::rotate(q, p.angle); };
  auto poly = [&](std::vector<Point> local, Coord r) {
    for (auto& q : local) q = tf(q);
    return Shape::polygon(std::move(local), r);
  };
  auto rect = [&](Coord hw, Coord hh, Coord r) {
    if (hw <= 0 && hh <= 0) return Shape::point(centre, r);
    if (hw <= 0) return Shape::segment(tf({0, -hh}), tf({0, hh}), r);
    if (hh <= 0) return Shape::segment(tf({-hw, 0}), tf({hw, 0}), r);
    return poly({{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}}, r);
  };
  std::vector<Shape> out;
  switch (p.shape) {
    case model::PadShape::Circle: out.push_back(Shape::point(centre, w / 2)); break;
    case model::PadShape::Oval:
      if (w == h) out.push_back(Shape::point(centre, w / 2));
      else if (w > h) out.push_back(Shape::segment(tf({-(w - h) / 2, 0}), tf({(w - h) / 2, 0}), h / 2));
      else out.push_back(Shape::segment(tf({0, -(h - w) / 2}), tf({0, (h - w) / 2}), w / 2));
      break;
    case model::PadShape::Rect: out.push_back(rect(w / 2, h / 2, 0)); break;
    case model::PadShape::RoundRect:
    case model::PadShape::ChamferedRect: {
      // KiCad applies chamfers to round-rect pads too (RoyalBlue54L J2: roundrect, chamfer_ratio 0.25, all corners).
      // A chamfered round rect is modelled as the chamfered rectangle shrunk by the corner radius, then inflated by it.
      const Coord r = p.shape == model::PadShape::RoundRect
                          ? static_cast<Coord>(std::llround(p.roundrect_ratio * static_cast<double>(std::min(w, h)))) : 0;
      const Coord c = p.chamfer_corners ? static_cast<Coord>(std::llround(p.chamfer_ratio * static_cast<double>(std::min(w, h)))) : 0;
      if (c <= 0) {
        out.push_back(rect(w / 2 - r, h / 2 - r, r));
        break;
      }
      // Shrinking by r moves the chamfer line inwards by r: on the shrunk rectangle the chamfer is c - r (2 - sqrt 2).
      const Coord hw = w / 2 - r, hh = h / 2 - r;
      const Coord cc = std::max<Coord>(0, c - static_cast<Coord>(std::llround(static_cast<double>(r) * (2.0 - std::numbers::sqrt2))));
      std::vector<Point> pts;
      auto corner = [&](bool cham, Point at, Point a, Point b) {
        if (cham && cc > 0) { pts.push_back(a); pts.push_back(b); }
        else pts.push_back(at);
      };
      corner(p.chamfer_corners & 1, {-hw, -hh}, {-hw, -hh + cc}, {-hw + cc, -hh});
      corner(p.chamfer_corners & 2, {hw, -hh}, {hw - cc, -hh}, {hw, -hh + cc});
      corner(p.chamfer_corners & 8, {hw, hh}, {hw, hh - cc}, {hw - cc, hh});
      corner(p.chamfer_corners & 4, {-hw, hh}, {-hw + cc, hh}, {-hw, hh - cc});
      out.push_back(poly(pts, r));
      break;
    }
    case model::PadShape::Trapezoid: {
      const Coord hw = w / 2, hh = h / 2, dx = p.trapezoid_dx / 2, dy = p.trapezoid_dy / 2;
      out.push_back(poly({{-hw - dy, hh + dx}, {hw + dy, hh - dx}, {hw - dy, -hh + dx}, {-hw + dy, -hh - dx}}, 0));
      break;
    }
    case model::PadShape::Custom: {
      // Anchor (a small circle or rect of the pad size) plus the primitive polygons.
      out.push_back(rect(w / 2, h / 2, 0));
      for (const auto& cp : p.custom_polys)
        if (cp.size() >= 3) out.push_back(poly(cp, 0));
      break;
    }
  }
  return out;
}

namespace {
void finish(CopperItem& it) {
  it.box = geom::Box{};
  for (const auto& s : it.shapes) it.box.add(s.box);
}
}  // namespace

CopperModel build_copper(const model::Board& b) {
  CopperModel m;
  // Pads.
  for (std::size_t i = 0; i < b.pads.size(); ++i) {
    const auto& p = b.pads[i];
    const bool npth_no_copper = p.type == model::PadType::NpThruHole && std::max(p.size_x, p.size_y) <= std::max(p.drill_x, p.drill_y);
    int item = -1;
    if (p.copper && !npth_no_copper) {
      CopperItem it;
      it.kind = ItemKind::Pad;
      it.index = static_cast<int>(i);
      it.net = p.net;
      it.layers = p.copper;
      it.shapes = pad_shapes(p);
      it.footprint = p.footprint;
      it.pos = p.pos;
      finish(it);
      item = static_cast<int>(m.items.size());
      m.items.push_back(std::move(it));
    }
    if (p.drill_x > 0) {
      Hole h;
      const Point c = p.pos;  // the hole is at the pad position
      if (p.drill_oval && p.drill_x != p.drill_y) {
        const Coord dw = p.drill_x, dh = p.drill_y;
        const Point half = dw > dh ? Point{(dw - dh) / 2, 0} : Point{0, (dh - dw) / 2};
        h.shape = Shape::segment(c - geom::rotate(half, p.angle), c + geom::rotate(half, p.angle), std::min(dw, dh) / 2);
      } else {
        h.shape = Shape::point(c, p.drill_x / 2);
      }
      h.item = item;
      h.pad = static_cast<int>(i);
      h.plated = p.type != model::PadType::NpThruHole;
      h.clearance = p.clearance >= 0 ? p.clearance : b.footprints[static_cast<std::size_t>(p.footprint)].clearance;
      h.net = p.net;
      h.pos = c;
      m.holes.push_back(std::move(h));
    }
  }
  // Tracks and arcs.
  for (std::size_t i = 0; i < b.tracks.size(); ++i) {
    const auto& t = b.tracks[i];
    if (t.layer < 0) continue;
    CopperItem it;
    it.kind = ItemKind::Track;
    it.index = static_cast<int>(i);
    it.net = t.net;
    it.layers = model::layer_bit(t.layer);
    it.shapes = {Shape::segment(t.a, t.b, t.width / 2)};
    it.pos = t.a;  // KiCad reports tracks at their start point
    it.width = t.width;
    finish(it);
    m.items.push_back(std::move(it));
  }
  for (std::size_t i = 0; i < b.arcs.size(); ++i) {
    const auto& t = b.arcs[i];
    if (t.layer < 0) continue;
    CopperItem it;
    it.kind = ItemKind::Arc;
    it.index = static_cast<int>(i);
    it.net = t.net;
    it.layers = model::layer_bit(t.layer);
    it.shapes = {Shape::polyline(geom::arc_points(t.a, t.mid, t.b, 100), t.width / 2)};  // 0.1 µm chords: well inside KiCad's epsilon
    it.pos = t.a;
    {
      // KiCad reports an arc at its centre (PCB_ARC::GetPosition).
      // Circle through start (origin), mid and end, in coordinates relative to the start.
      const double bx = static_cast<double>(t.mid.x - t.a.x), by = static_cast<double>(t.mid.y - t.a.y);
      const double cx = static_cast<double>(t.b.x - t.a.x), cy = static_cast<double>(t.b.y - t.a.y);
      const double d = 2 * (bx * cy - by * cx);
      if (std::fabs(d) > 1e-6) {
        const double b2 = bx * bx + by * by, c2 = cx * cx + cy * cy;
        it.pos = {t.a.x + geom::kiround((cy * b2 - by * c2) / d), t.a.y + geom::kiround((bx * c2 - cx * b2) / d)};
      }
    }
    it.width = t.width;
    finish(it);
    m.items.push_back(std::move(it));
  }
  // Vias.
  for (std::size_t i = 0; i < b.vias.size(); ++i) {
    const auto& v = b.vias[i];
    CopperItem it;
    it.kind = ItemKind::Via;
    it.index = static_cast<int>(i);
    it.net = v.net;
    for (int l = v.layer_top; l <= v.layer_bottom; ++l) it.layers |= model::layer_bit(l);
    it.shapes = {Shape::point(v.pos, v.size / 2)};
    it.free_via = v.free;
    it.pos = v.pos;
    it.width = v.size;
    finish(it);
    Hole h;
    h.shape = Shape::point(v.pos, v.drill / 2);
    h.item = static_cast<int>(m.items.size());
    h.via = static_cast<int>(i);
    h.net = v.net;
    h.pos = v.pos;
    m.items.push_back(std::move(it));
    m.holes.push_back(std::move(h));
  }
  // Zone fills (copper zones only).
  for (std::size_t i = 0; i < b.zones.size(); ++i) {
    const auto& z = b.zones[i];
    if (z.rule_area) continue;
    for (std::size_t k = 0; k < z.fills.size(); ++k) {
      const auto& [layer, pts] = z.fills[k];
      if (layer < 0 || pts.size() < 3) continue;
      CopperItem it;
      it.kind = ItemKind::Zone;
      it.index = static_cast<int>(i);
      it.sub = static_cast<int>(k);
      it.net = z.net;
      it.layers = model::layer_bit(layer);
      it.shapes = {Shape::polygon(pts, 0)};
      it.pos = z.outline.empty() || z.outline.front().empty() ? pts.front() : z.outline.front().front();  // KiCad: first outline corner
      it.footprint = z.footprint;
      finish(it);
      m.items.push_back(std::move(it));
    }
  }
  // Copper graphics (board and footprint): lines, arcs, circles, rects, polygons.
  for (std::size_t i = 0; i < b.graphics.size(); ++i) {
    const auto& g = b.graphics[i];
    const int layer = b.copper_index(g.layer);
    const bool margin = g.layer == "Margin";
    if (layer < 0 && g.layer != "Edge.Cuts" && !margin) continue;
    std::vector<Shape> shapes;
    const Coord r = g.width / 2;
    switch (g.kind) {
      case model::Graphic::Kind::Line: shapes.push_back(Shape::segment(g.a, g.b, r)); break;
      case model::Graphic::Kind::Arc: shapes.push_back(Shape::polyline(geom::arc_points(g.a, g.c, g.b, 100), r)); break;
      case model::Graphic::Kind::Circle: {
        const Coord rad = geom::kiround(std::hypot(static_cast<double>(g.b.x - g.a.x), static_cast<double>(g.b.y - g.a.y)));
        if (g.filled && layer >= 0) shapes.push_back(Shape::point(g.a, rad + r));
        else shapes.push_back(Shape::polyline(geom::circle_points(g.a, rad, 100), r));
        break;
      }
      case model::Graphic::Kind::Rect:
      case model::Graphic::Kind::Poly:
      case model::Graphic::Kind::Curve: {
        if (g.pts.size() < 2) break;
        if (g.filled && layer >= 0 && g.pts.size() >= 3) {
          shapes.push_back(Shape::polygon(g.pts, r));
        } else {
          auto pts = g.pts;
          if (g.kind != model::Graphic::Kind::Curve) pts.push_back(pts.front());
          shapes.push_back(Shape::polyline(std::move(pts), r));
        }
        break;
      }
    }
    if (shapes.empty()) continue;
    if (layer < 0) {  // Edge.Cuts: outline pieces, compared at zero width
      for (auto& s : shapes) {
        s.r = 0;
        s.update_box();
        (margin ? m.margins : m.edges).push_back(std::move(s));
      }
      continue;
    }
    CopperItem it;
    it.kind = ItemKind::Graphic;
    it.index = static_cast<int>(i);
    it.net = g.net;
    it.layers = model::layer_bit(layer);
    it.shapes = std::move(shapes);
    it.footprint = g.footprint;
    it.pos = g.pts.empty() ? g.a : g.pts.front();
    // Footprint copper graphics belong to the net of the pad they overlap (net ties are not modelled).
    finish(it);
    m.items.push_back(std::move(it));
  }
  // Net-less copper graphics inside a footprint take the net of the footprint pad they touch.
  for (auto& g : m.items) {
    if (g.kind != ItemKind::Graphic || g.net != 0 || g.footprint < 0) continue;
    for (const auto& p : m.items) {
      if (p.kind != ItemKind::Pad || p.footprint != g.footprint || p.net == 0 || !(p.layers & g.layers)) continue;
      bool touch = false;
      for (const auto& s : g.shapes)
        for (const auto& u : p.shapes)
          if (geom::closer_than(s, u, 1)) touch = true;
      if (touch) {
        g.net = p.net;
        break;
      }
    }
  }
  return m;
}

}  // namespace tmk::drc

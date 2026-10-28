// SPDX-License-Identifier: GPL-3.0-or-later
#include "drc/copper.hpp"

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <numbers>

#include "index/uniform_grid.hpp"

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
      for (const auto& cp : p.custom_prims) {
        using K = model::Pad::CustomPrim::Kind;
        if (cp.kind == K::Disc) {
          out.push_back(Shape::point(tf(cp.pts[0]), cp.radius));
        } else if (cp.kind == K::Polyline) {
          std::vector<Point> q = cp.pts;
          for (auto& v : q) v = tf(v);
          out.push_back(Shape::polyline(std::move(q), cp.radius));
        } else if (cp.pts.size() >= 3) {
          out.push_back(poly(cp.pts, cp.radius));
        }
      }
      break;
    }
  }
  return out;
}

namespace {
void split_bare_layers(const model::Board& b, CopperModel& m);
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
        // Edge.Cuts pieces count at zero width; Margin pieces with their stroke (KiCad's actual distance to a 2 mm Margin line is 0 for
        // copper that touches the stroke: PCBench RaspberryPi-PoE_PoELLi_PI).
        if (!margin) s.r = 0;
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
  // Board copper graphics (a manual pour drawn as a polygon) are copper of the net they touch: pads, tracks and vias of exactly one net. Touching several nets, or none: net-less.
  if (b.infer_graphic_nets) {
    for (auto& g : m.items) {
      if (g.kind != ItemKind::Graphic || g.net != 0 || g.footprint >= 0) continue;
      model::NetId only = 0;
      bool many = false;
      for (const auto& p : m.items) {
        if (many) break;
        if ((p.kind != ItemKind::Pad && p.kind != ItemKind::Track && p.kind != ItemKind::Via) || p.net == 0 || !(p.layers & g.layers) || p.net == only) continue;
        bool touch = false;
        for (const auto& s : g.shapes) {
          for (const auto& u : p.shapes)
            if (geom::closer_than(s, u, 1)) touch = true;
          if (touch) break;
        }
        if (!touch) continue;
        if (only == 0) only = p.net;
        else many = true;
      }
      if (!many) g.net = only;
      if (std::getenv("TM_DEBUG_GFX")) std::fprintf(stderr, "board copper graphic %d: net %d%s\n", g.index, static_cast<int>(g.net), many ? " (several nets: net-less)" : "");
    }
  }
  split_bare_layers(b, m);
  return m;
}

namespace {

// The conditional layers of a padstack spanning `top`..`bottom`.
model::LayerMask conditional_layers(model::UnconnectedLayers mode, model::LayerMask span, int top, int bottom) {
  switch (mode) {
    case model::UnconnectedLayers::KeepAll: return 0;
    case model::UnconnectedLayers::RemoveAll: return span;
    default: return span & ~(model::layer_bit(top) | model::layer_bit(bottom));
  }
}

bool removes_layers(model::UnconnectedLayers mode) {
  return mode == model::UnconnectedLayers::RemoveAll || mode == model::UnconnectedLayers::RemoveExceptStartEnd;
}

}  // namespace

model::LayerMask conditional_layers(const model::Via& v) {
  model::LayerMask span = 0;
  for (int l = v.layer_top; l <= v.layer_bottom; ++l) span |= model::layer_bit(l);
  return conditional_layers(v.unconnected, span, v.layer_top, v.layer_bottom);
}

model::LayerMask conditional_layers(const model::Pad& p, int copper_count) {
  return p.type == model::PadType::ThruHole ? conditional_layers(p.unconnected, p.copper, 0, copper_count - 1) : 0;
}

Flashing flash_layers(const model::Board& b) {
  const int nl = b.copper_count();
  const model::LayerMask all = nl >= 64 ? ~model::LayerMask{0} : (model::LayerMask{1} << nl) - 1;
  Flashing f;
  f.vias.resize(b.vias.size());
  f.pads.resize(b.pads.size());
  // Every connectable item (CN_ITEM): its layer set for the touching test, the layers it spans for IsConnectedOnLayer, the
  // layers where it is only its hole, and its shapes.
  struct Node {
    int via = -1, pad = -1;
    model::NetId net = 0;
    model::LayerMask set = 0, cn = 0, cond = 0;
    std::vector<Shape> full;
    Shape hole;
    geom::Box box;
    int footprint = -1;
    model::UnconnectedLayers mode = model::UnconnectedLayers::KeepAll;
  };
  std::vector<Node> nodes;
  bool any = false;
  for (std::size_t i = 0; i < b.vias.size(); ++i) {
    const auto& v = b.vias[i];
    Node n;
    n.via = static_cast<int>(i);
    n.net = v.net;
    for (int l = v.layer_top; l <= v.layer_bottom; ++l) n.set |= model::layer_bit(l);
    n.cn = n.set;
    n.cond = conditional_layers(v.unconnected, n.set, v.layer_top, v.layer_bottom);
    n.mode = v.unconnected;
    n.full = {Shape::point(v.pos, v.size / 2)};
    n.hole = Shape::point(v.pos, v.drill / 2);
    f.vias[i] = n.set;
    any |= n.cond != 0;
    nodes.push_back(std::move(n));
  }
  for (std::size_t i = 0; i < b.pads.size(); ++i) {
    const auto& p = b.pads[i];
    f.pads[i] = p.copper;
    if (!p.copper) continue;
    Node n;
    n.pad = static_cast<int>(i);
    n.net = p.net;
    n.set = p.copper;
    // CN_LIST::Add(PAD*): a plated through-hole pad spans F.Cu..B.Cu, any other pad its first copper layer.
    n.cn = p.type == model::PadType::ThruHole ? all : model::layer_bit(std::countr_zero(p.copper));
    n.footprint = p.footprint;
    if (p.type == model::PadType::ThruHole) {
      n.cond = conditional_layers(p.unconnected, p.copper, 0, nl - 1);
      n.mode = p.unconnected;
      if (p.drill_oval && p.drill_x != p.drill_y) {
        const Point half = p.drill_x > p.drill_y ? Point{(p.drill_x - p.drill_y) / 2, 0} : Point{0, (p.drill_y - p.drill_x) / 2};
        n.hole = Shape::segment(p.pos - geom::rotate(half, p.angle), p.pos + geom::rotate(half, p.angle), std::min(p.drill_x, p.drill_y) / 2);
      } else {
        n.hole = Shape::point(p.pos, p.drill_x / 2);
      }
    }
    n.full = pad_shapes(p);
    any |= n.cond != 0;
    nodes.push_back(std::move(n));
  }
  if (!any) return f;
  for (const auto& t : b.tracks) {
    if (t.layer < 0) continue;
    Node n;
    n.net = t.net;
    n.set = n.cn = model::layer_bit(t.layer);
    n.full = {Shape::segment(t.a, t.b, t.width / 2)};
    nodes.push_back(std::move(n));
  }
  for (const auto& t : b.arcs) {
    if (t.layer < 0) continue;
    Node n;
    n.net = t.net;
    n.set = n.cn = model::layer_bit(t.layer);
    n.full = {Shape::polyline(geom::arc_points(t.a, t.mid, t.b, 100), t.width / 2)};
    nodes.push_back(std::move(n));
  }
  geom::Box bounds;
  for (auto& n : nodes) {
    for (const auto& s : n.full) n.box.add(s.box);
    if (n.cond) n.box.add(n.hole.box);
    bounds.add(n.box);
  }
  index::UniformGrid grid(bounds, 1'000'000, nodes.size());
  for (std::size_t i = 0; i < nodes.size(); ++i) grid.insert(static_cast<int>(i), nodes[i].box);
  // The shapes of a node on a layer of its set, as CN_VISITOR collides them.
  auto collide = [&](const Node& x, const Node& y, int l) {
    const model::LayerMask bit = model::layer_bit(l);
    const std::vector<Shape> xh{x.hole}, yh{y.hole};
    const auto& xs = (x.cond & bit) ? xh : x.full;
    const auto& ys = (y.cond & bit) ? yh : y.full;
    for (const auto& s : xs)
      for (const auto& u : ys)
        if (geom::closer_than(s, u, 0)) return true;
    return false;
  };
  for (auto& x : nodes) {
    if (!x.cond) continue;
    model::LayerMask flashed = x.set & ~x.cond;
    if (x.mode != model::UnconnectedLayers::StartEndOnly) {
      flashed |= (x.via >= 0 ? b.vias[static_cast<std::size_t>(x.via)].zone_layers : b.pads[static_cast<std::size_t>(x.pad)].zone_layers) & x.cond;
      // IsConnectedOnLayer: a layer keeps its copper when an item touching this one, of its net, spans the layer.
      grid.query(x.box, [&](int j) {
        const Node& y = nodes[static_cast<std::size_t>(j)];
        if (&y == &x || y.net != x.net || !(y.cn & x.cond & ~flashed)) return;
        if (x.pad >= 0 && y.pad >= 0 && (y.footprint == x.footprint || (removes_layers(x.mode) && removes_layers(y.mode)))) return;
        const model::LayerMask common = x.set & y.set;
        for (int l = 0; l < nl; ++l)
          if ((common & model::layer_bit(l)) && collide(x, y, l)) {
            flashed |= y.cn & x.cond;
            return;
          }
      });
    }
    (x.via >= 0 ? f.vias[static_cast<std::size_t>(x.via)] : f.pads[static_cast<std::size_t>(x.pad)]) = flashed;
  }
  return f;
}

namespace {

void split_bare_layers(const model::Board& b, CopperModel& m) {
  const bool any = std::any_of(b.vias.begin(), b.vias.end(), [](const model::Via& v) { return v.unconnected != model::UnconnectedLayers::KeepAll; }) ||
                   std::any_of(b.pads.begin(), b.pads.end(), [](const model::Pad& p) {
                     return p.type == model::PadType::ThruHole && p.unconnected != model::UnconnectedLayers::KeepAll;
                   });
  if (!any) return;
  const Flashing f = flash_layers(b);
  std::vector<int> hole_of(m.items.size(), -1);
  for (std::size_t h = 0; h < m.holes.size(); ++h)
    if (m.holes[h].item >= 0) hole_of[static_cast<std::size_t>(m.holes[h].item)] = static_cast<int>(h);
  const std::size_t n = m.items.size();
  for (std::size_t i = 0; i < n; ++i) {
    auto& it = m.items[i];
    model::LayerMask flashed = it.layers;
    if (it.kind == ItemKind::Via) flashed = f.vias[static_cast<std::size_t>(it.index)];
    else if (it.kind == ItemKind::Pad) flashed = f.pads[static_cast<std::size_t>(it.index)];
    else continue;
    const model::LayerMask bare = it.layers & ~flashed;
    if (!bare || hole_of[i] < 0) continue;
    CopperItem h = it;
    h.layers = bare;
    h.shapes = {m.holes[static_cast<std::size_t>(hole_of[i])].shape};
    h.span = it.layers;
    h.bare_of = static_cast<int>(i);
    finish(h);
    it.span = it.layers;
    it.layers = flashed;
    it.bare_part = static_cast<int>(m.items.size());
    m.items.push_back(std::move(h));
  }
}

}  // namespace

std::vector<std::vector<model::Point>> edge_loops(const std::vector<geom::Shape>& edges, Coord tol, bool* all_closed) {
  auto near = [tol](Point a, Point c) { return std::llabs(a.x - c.x) < tol && std::llabs(a.y - c.y) < tol; };
  if (all_closed) *all_closed = true;
  std::vector<std::uint8_t> used(edges.size(), 0);
  std::vector<std::vector<Point>> loops;
  for (std::size_t s0 = 0; s0 < edges.size(); ++s0) {
    if (used[s0] || edges[s0].pts.size() < 2) continue;
    used[s0] = 1;
    std::vector<Point> chain = edges[s0].pts;
    for (bool grown = true; grown && !near(chain.front(), chain.back());) {
      grown = false;
      for (std::size_t k = 0; k < edges.size(); ++k) {
        const auto& pk = edges[k].pts;
        if (used[k] || pk.size() < 2) continue;
        if (near(chain.back(), pk.front())) chain.insert(chain.end(), pk.begin() + 1, pk.end());
        else if (near(chain.back(), pk.back())) chain.insert(chain.end(), pk.rbegin() + 1, pk.rend());
        else continue;
        used[k] = 1;
        grown = true;
        break;
      }
    }
    if (chain.size() >= 4 && near(chain.front(), chain.back())) loops.push_back(std::move(chain));
    else if (all_closed) *all_closed = false;
  }
  return loops;
}

}  // namespace tmk::drc

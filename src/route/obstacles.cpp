// SPDX-License-Identifier: GPL-3.0-or-later
#include "route/obstacles.hpp"

#include <array>
#include <optional>
#include <tuple>
#include <algorithm>
#include <bit>
#include <cmath>
#include <memory>

namespace tmk::route {

using geom::Point;
using geom::Shape;

Obstacles::Obstacles(model::Board& board, const model::DesignRules& rules) : b_(board), r_(rules), cm_(drc::build_copper(board)) {
  // KiCad checks routed copper against footprint copper graphics as net-less copper (keyboard-switch
  // footprints, logos), whatever pad they touch: block them for every net.
  for (auto& it : cm_.items)
    if (it.kind == drc::ItemKind::Graphic) it.net = 0;
  re_ = std::make_unique<drc::RuleEngine>(b_, r_);
  fills_ = std::make_unique<drc::ZoneFills>(cm_);
  reach_ = std::max<Coord>(re_->max_clearance(), 1'000'000) + 2'000'000;  // clearance + generous track/via size
  bounds_ = b_.edge_bbox();
  for (const auto& it : cm_.items) bounds_.add(it.box);
  if (bounds_.empty()) bounds_ = geom::Box{0, 0, 100'000'000, 100'000'000};
  bounds_ = bounds_.inflated(5'000'000);
  const Coord cell = 1'000'000;
  grid_ = std::make_unique<index::UniformGrid>(bounds_, cell, cm_.items.size() + 1024);
  for (std::size_t i = 0; i < cm_.items.size(); ++i) grid_->insert(static_cast<int>(i), cm_.items[i].box);
  rgrid_ = std::make_unique<index::UniformGrid>(bounds_, cell, 1024);
  // Non-plated holes are board edges for KiCad's copper_edge_clearance (verified on PCBench ErgoDone).
  for (auto& h : cm_.holes)
    if (!h.plated) h.clearance = std::max(h.clearance, r_.minimums.copper_edge_clearance);
  for (const auto& h : cm_.holes) max_hole_local_ = std::max(max_hole_local_, h.clearance);
  hgrid_ = std::make_unique<index::UniformGrid>(bounds_, cell, cm_.holes.size() + 1024);
  for (std::size_t i = 0; i < cm_.holes.size(); ++i) hgrid_->insert(static_cast<int>(i), cm_.holes[i].shape.box);
  // Board edge as individual segments in a grid, so edge tests cost O(nearby segments).
  for (const auto* list : {&cm_.edges, &cm_.margins})  // Margin graphics keep copper away like the edge (KiCad)
    for (const auto& e : *list)
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
  // Solder-mask openings drawn as graphics (logos, test areas): new copper under them would bridge. Lines
  // that close into a loop also open the area they enclose (KiCad fills closed mask outlines; PCBench
  // Horticulture).
  // KiCad grows mask graphics by the board's mask expansion when testing bridges (Horticulture: a track 0.17 mm
  // from a mask line bridged with pad_to_mask_clearance 0.2).
  const Coord gexp = std::max<Coord>(0, b_.pad_to_mask_clearance);
  for (int side = 0; side < 2; ++side) {
    const char* ln = side == 0 ? "F.Mask" : "B.Mask";
    std::vector<std::vector<Point>> pieces;
    for (const auto& g : b_.graphics)
      if (g.layer == ln && g.kind == model::Graphic::Kind::Line) pieces.push_back({g.a, g.b});
      else if (g.layer == ln && g.kind == model::Graphic::Kind::Arc) pieces.push_back(geom::arc_points(g.a, g.c, g.b, 5'000));
    auto near = [](Point a, Point c) { return std::llabs(a.x - c.x) < 2000 && std::llabs(a.y - c.y) < 2000; };
    std::vector<std::uint8_t> used(pieces.size(), 0);
    for (std::size_t s0 = 0; s0 < pieces.size(); ++s0) {
      if (used[s0]) continue;
      used[s0] = 1;
      std::vector<Point> chain = pieces[s0];
      for (bool grown = true; grown && !near(chain.front(), chain.back());) {
        grown = false;
        for (std::size_t k = 0; k < pieces.size(); ++k) {
          if (used[k]) continue;
          if (near(chain.back(), pieces[k].front())) chain.insert(chain.end(), pieces[k].begin() + 1, pieces[k].end());
          else if (near(chain.back(), pieces[k].back())) chain.insert(chain.end(), pieces[k].rbegin() + 1, pieces[k].rend());
          else continue;
          used[k] = 1;
          grown = true;
          break;
        }
      }
      if (chain.size() >= 4 && near(chain.front(), chain.back())) mask_open_[side].push_back(Shape::polygon(chain, gexp));
    }
  }
  // A footprint's own mask graphic over pads of one net of that footprint is that pad's opening (DAC 2020 boards
  // draw every pad opening as an F.Mask polygon): copper of that net may enter it, like a pad aperture. Other
  // mask graphics are net-less openings that no copper may enter.
  std::vector<std::tuple<int, Shape, model::NetId>> fp_openings;
  for (const auto& g : b_.graphics) {
    const int side = g.layer == "F.Mask" ? 0 : g.layer == "B.Mask" ? 1 : -1;
    if (side < 0) continue;
    std::optional<Shape> sh;
    if ((g.kind == model::Graphic::Kind::Poly || g.kind == model::Graphic::Kind::Rect) && g.pts.size() >= 3 && (g.filled || g.kind == model::Graphic::Kind::Poly))
      sh = Shape::polygon(g.pts, g.width / 2 + gexp);
    else if (g.kind == model::Graphic::Kind::Circle && g.filled)
      sh = Shape::point(g.a, geom::kiround(std::hypot(static_cast<double>(g.b.x - g.a.x), static_cast<double>(g.b.y - g.a.y))) + g.width / 2 + gexp);
    else if (g.kind == model::Graphic::Kind::Line)
      sh = Shape::segment(g.a, g.b, g.width / 2 + gexp);
    if (!sh) continue;
    model::NetId net = 0;
    int nets = 0;
    if (g.footprint >= 0)
      for (int pi : b_.footprints[static_cast<std::size_t>(g.footprint)].pads) {
        const auto& pad = b_.pads[static_cast<std::size_t>(pi)];
        bool touches = false;
        for (const auto& ps : drc::pad_shapes(pad))
          if (geom::closer_than(*sh, ps, 1)) touches = true;
        if (!touches) continue;
        if (nets == 0 || pad.net != net) ++nets;
        net = pad.net;
      }
    if (nets == 1 && net != 0) fp_openings.emplace_back(side, *sh, net);
    else mask_open_[side].push_back(*sh);
  }
  // Inside-board raster (exact point-in-polygon only in cells the outline or a cut-out crosses).
  if (!outline_.empty()) {
    ir_w_ = static_cast<int>((bounds_.x1 - bounds_.x0) / ir_cell_) + 1;
    ir_h_ = static_cast<int>((bounds_.y1 - bounds_.y0) / ir_cell_) + 1;
    inside_raster_.assign(static_cast<std::size_t>(ir_w_) * static_cast<std::size_t>(ir_h_), 255);
    auto mark_loop = [&](const std::vector<Point>& loop) {
      for (std::size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++) {
        const Point a = loop[j], c = loop[i];
        const Coord len = std::max(std::llabs(c.x - a.x), std::llabs(c.y - a.y));
        const int n = static_cast<int>(len / (ir_cell_ / 4)) + 1;
        for (int k = 0; k <= n; ++k) {
          const Point q{a.x + (c.x - a.x) * k / n, a.y + (c.y - a.y) * k / n};
          const Coord cx = (q.x - bounds_.x0) / ir_cell_, cy = (q.y - bounds_.y0) / ir_cell_;
          for (Coord dy = -1; dy <= 1; ++dy)
            for (Coord dx = -1; dx <= 1; ++dx) {
              const Coord x = cx + dx, y = cy + dy;
              if (x >= 0 && y >= 0 && x < ir_w_ && y < ir_h_) inside_raster_[static_cast<std::size_t>(y) * static_cast<std::size_t>(ir_w_) + static_cast<std::size_t>(x)] = 2;
            }
        }
      }
    };
    mark_loop(outline_);
    for (const auto& c : cutouts_) mark_loop(c);
    // Interior cells by scanline parity (an exact point-in-polygon per cell was O(cells x vertices): minutes on
    // PCBench LeeChee_1800, 378 x 157 mm with a 572-line outline). Cells near an edge are marked 2 above and
    // tested exactly per query, so the parity here only has to be right at cell centres far from edges.
    std::vector<const std::vector<Point>*> loops{&outline_};
    for (const auto& c : cutouts_) loops.push_back(&c);
    std::vector<std::vector<double>> xs(loops.size());
    std::vector<std::size_t> at(loops.size());
    for (int y = 0; y < ir_h_; ++y) {
      const double yc = static_cast<double>(bounds_.y0 + static_cast<Coord>(y) * ir_cell_ + ir_cell_ / 2);
      for (std::size_t li = 0; li < loops.size(); ++li) {
        const auto& lp = *loops[li];
        auto& v = xs[li];
        v.clear();
        for (std::size_t i = 0, j = lp.size() - 1; i < lp.size(); j = i++) {
          const Point a = lp[j], c = lp[i];
          if ((static_cast<double>(a.y) > yc) != (static_cast<double>(c.y) > yc))
            v.push_back(static_cast<double>(a.x) + (yc - static_cast<double>(a.y)) * static_cast<double>(c.x - a.x) / static_cast<double>(c.y - a.y));
        }
        std::sort(v.begin(), v.end());
        at[li] = 0;
      }
      for (int x = 0; x < ir_w_; ++x) {
        const double xc = static_cast<double>(bounds_.x0 + static_cast<Coord>(x) * ir_cell_ + ir_cell_ / 2);
        bool in = true;
        for (std::size_t li = 0; li < loops.size(); ++li) {
          auto& k = at[li];
          while (k < xs[li].size() && xs[li][k] <= xc) ++k;
          const bool odd = ((xs[li].size() - k) & 1u) != 0;  // crossings to the right of the centre
          if (li == 0 ? !odd : odd) in = false;
        }
        auto& v = inside_raster_[static_cast<std::size_t>(y) * static_cast<std::size_t>(ir_w_) + static_cast<std::size_t>(x)];
        if (v == 255) v = in ? 1 : 0;
      }
    }
  }
  // Pad solder-mask openings per board side (independent of the pad's copper layers: an edge-connector pad on
  // B.Cu can still open the front mask).
  via_mask_ = b_.vias_tented ? 0 : b_.pad_to_mask_clearance;
  for (int side = 0; side < 2; ++side) {
    agrid_[side] = std::make_unique<index::UniformGrid>(bounds_, cell, b_.pads.size() + 16);
    const char* mask = side == 0 ? "F.Mask" : "B.Mask";
    for (const auto& pad : b_.pads) {
      bool has = false;
      for (const auto& ln : pad.layers)
        if (ln == mask || ln == "*.Mask" || ln == "F&B.Mask") has = true;
      if (!has) continue;
      Aperture a;
      a.shapes = drc::pad_shapes(pad);
      Coord mm = pad.mask_margin;
      if (mm == INT64_MIN) mm = b_.footprints[static_cast<std::size_t>(pad.footprint)].mask_margin;
      if (mm == INT64_MIN) mm = b_.pad_to_mask_clearance;
      a.margin = std::max<Coord>(mm, 0);
      a.net = pad.net;
      for (const auto& sh : a.shapes) a.box.add(sh.box);
      a.box = a.box.inflated(a.margin);
      agrid_[side]->insert(static_cast<int>(apertures_[side].size()), a.box);
      apertures_[side].push_back(std::move(a));
    }
  }
  for (auto& [side, sh, net] : fp_openings) {
    Aperture a;
    a.shapes = {sh};
    a.margin = 0;
    a.net = net;
    a.box = sh.box;
    agrid_[side]->insert(static_cast<int>(apertures_[side].size()), a.box);
    apertures_[side].push_back(std::move(a));
  }
  // Copper text: one rectangle per line, sized from approximate KiCad stroke-font advances (upper case 0.8,
  // lower case 0.68, digits 0.72, narrow glyphs 0.4 of the glyph width; line pitch 1.62 x height), justified
  // and mirrored as in the file, plus the stroke thickness and a 5% margin.
  for (const auto& t : b_.texts) {
    if (t.hidden || t.text.empty()) continue;
    const int l = b_.copper_index(t.layer);
    const int mask_side = t.layer == "F.Mask" ? 0 : t.layer == "B.Mask" ? 1 : -1;
    if (l < 0 && mask_side < 0) continue;
    std::vector<double> widths(1, 0.0);  // per line, in glyph widths
    for (std::size_t i = 0; i < t.text.size(); ++i) {
      const unsigned char ch = static_cast<unsigned char>(t.text[i]);
      if (ch == '\n') {
        widths.push_back(0.0);
        continue;
      }
      if ((ch & 0xC0) == 0x80) continue;  // UTF-8 continuation byte
      double a = 0.9;
      if (ch >= 'a' && ch <= 'z') a = (ch == 'i' || ch == 'l' || ch == 'j' || ch == 't' || ch == 'f' || ch == 'r') ? 0.5 : (ch == 'm' || ch == 'w') ? 1.0 : 0.75;
      else if (ch >= '0' && ch <= '9') a = 0.85;
      else if (ch == ' ' || ch == '.' || ch == ',' || ch == ':' || ch == ';' || ch == '\'' || ch == '!' || ch == '|' || ch == 'I') a = 0.4;
      else if (ch == 'M' || ch == 'W') a = 0.95;
      else if (ch >= 0x80) a = 0.8;
      widths.back() += a;
    }
    // The file's two font sizes are taken as the larger for the advance (their order differs between versions).
    const Coord h = std::max<Coord>(std::max(t.height, t.width), 300'000), cw = h, th = std::max<Coord>(t.thickness, 100'000);
    const double pitchl = 1.62 * static_cast<double>(h);
    const Coord H = static_cast<Coord>(static_cast<double>(widths.size() - 1) * pitchl) + h;
    const Coord ytop = t.justify_v < 0 ? 0 : t.justify_v > 0 ? -H : -H / 2;
    for (std::size_t li = 0; li < widths.size(); ++li) {
      const Coord W = static_cast<Coord>(widths[li] * static_cast<double>(cw) * 1.05);
      if (W <= 0) continue;
      Coord x0 = t.justify_h < 0 ? 0 : t.justify_h > 0 ? -W : -W / 2;
      if (t.mirror) x0 = -x0 - W;
      const Coord y0 = ytop + static_cast<Coord>(static_cast<double>(li) * pitchl), y1 = y0 + h * 105 / 100;
      // Extra 15% of the height on every side: KiCad's glyphs for bottom-justified text reach about 0.1 h above
      // this box (PCBench USBI2C01, 'JACHO' on B.Cu: 0.12 mm violation with the tighter box).
      const Coord m = th + h * 15 / 100;
      std::vector<Point> pts = {{x0 - m, y0 - m}, {x0 + W + m, y0 - m}, {x0 + W + m, y1 + m}, {x0 - m, y1 + m}};
      for (auto& p : pts) p = t.pos + geom::rotate(p, t.angle);
      if (l >= 0) texts_.emplace_back(l, Shape::polygon(pts, 0));
      else mask_open_[mask_side].push_back(Shape::polygon(pts, gexp));  // mask text: an opening, like mask graphics
    }
  }
  // Include via-only keepouts; track and via checks use their respective flags.
  for (const auto& z : b_.zones)
    if (z.rule_area && (z.keepout_tracks || z.keepout_vias) && !z.outline.empty() && z.outline.front().size() >= 3)
      keepouts_.emplace_back(Shape::polygon(z.outline.front(), 0), &z);
}

bool Obstacles::via_keeps_off(const drc::CopperItem& it) const {
  if (!vias_off_pads_ || pad_via_exempt_ || it.kind != drc::ItemKind::Pad || std::popcount(it.layers) != 1) return false;
  const auto& p = b_.pads[static_cast<std::size_t>(it.index)];
  return std::min(p.size_x, p.size_y) < vias_off_pads_max_;
}

bool Obstacles::inside_exact(Point p) const {
  if (!geom::point_in_polygon(p, outline_)) return false;
  for (const auto& c : cutouts_)
    if (geom::point_in_polygon(p, c)) return false;
  return true;
}

bool Obstacles::inside_board(Point p, Coord margin) const {
  if (outline_.empty()) return true;
  if (!inside_raster_.empty()) {
    const Coord cx = (p.x - bounds_.x0) / ir_cell_, cy = (p.y - bounds_.y0) / ir_cell_;
    if (cx < 0 || cy < 0 || cx >= ir_w_ || cy >= ir_h_) return false;
    const std::uint8_t st = inside_raster_[static_cast<std::size_t>(cy) * static_cast<std::size_t>(ir_w_) + static_cast<std::size_t>(cx)];
    if (st == 0) return false;
    if (st == 2 && !inside_exact(p)) return false;
  } else if (!inside_exact(p)) {
    return false;
  }
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
    if (soft_zones_ && it.kind == drc::ItemKind::Zone) return;
    if (it.net == probe.net && probe.net != 0 && !(probe.kind == drc::ItemKind::Via && via_keeps_off(it))) return;
    Coord req = re_->clearance(probe, it, layer);
    // Untented vias open the mask around themselves: other nets' copper must stay outside that opening.
    if (via_mask_ > 0 && (layer == 0 || layer == b_.copper_count() - 1)) {
      if (it.kind == drc::ItemKind::Via) req = std::max(req, via_mask_ + (probe.kind == drc::ItemKind::Via ? via_mask_ : 0) + 1'000);
      else if (probe.kind == drc::ItemKind::Via && it.kind != drc::ItemKind::Zone) req = std::max(req, via_mask_ + 1'000);
    }
    if (item_closer(s, it, req)) {
      if (ignore_routed && it.owner >= 0) {
        state = 1;
        if (owners) owners->push_back(it.owner);
      } else {
        state = 2;
      }
    }
  });
  return state;
}

bool Obstacles::item_closer(const Shape& s, const drc::CopperItem& it, Coord req) const {
  if (it.kind == drc::ItemKind::Zone && !linear_zone_tests_) {
    const auto i = static_cast<std::size_t>(&it - cm_.items.data());
    if (i < fills_->slot.size() && fills_->slot[i] >= 0) return fills_->index[static_cast<std::size_t>(fills_->slot[i])].shape_closer(s, req);
  }
  for (const auto& u : it.shapes)
    if (geom::closer_than(s, u, req)) return true;
  return false;
}

int Obstacles::holes_edges_state(const Shape& s, model::NetId net, int layer, bool is_via_hole, Coord hole_r, bool ignore_routed,
                                 std::vector<int>* owners) const {
  // Copper (or a new via's hole) against other items' holes.
  int state = 0;
  const Coord hc = std::max<Coord>(r_.minimums.hole_clearance, 0);
  const Coord h2h = std::max<Coord>(r_.minimums.hole_to_hole, 0);
  hgrid_->query(s.box.inflated(std::max({hc, h2h, max_hole_local_}) + 1), [&](int id) {
    if (state == 2) return;
    const auto& h = cm_.holes[static_cast<std::size_t>(id)];
    if (h.removed) return;
    bool hit = false;
    if (!(h.net == net && net != 0 && h.plated) && geom::closer_than(s, h.shape, std::max(hc, h.clearance))) hit = true;
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
  // A new via's hole against copper of other nets (hole clearance).
  if (is_via_hole && hc > 0) {
    const Shape hole = Shape::point(s.pts[0], hole_r);
    grid_->query(hole.box.inflated(hc + 1), [&](int id) {
      if (state == 2) return;
      const auto& it = cm_.items[static_cast<std::size_t>(id)];
      if (it.removed || it.kind == drc::ItemKind::Zone || (it.net == net && net != 0) || !(it.layers & model::layer_bit(layer))) return;
      for (const auto& u : it.shapes)
        if (geom::closer_than(hole, u, hc)) {
          if (ignore_routed && it.owner >= 0) {
            state = 1;
            if (owners) owners->push_back(it.owner);
          } else {
            state = 2;
          }
          return;
        }
    });
    if (state == 2) return 2;
  }
  // Physical hole clearance (custom rules): the new copper against every hole, and a new via's hole against
  // every item's copper, fixed and routed, any net.
  state = std::max(state, physical_copper_state(s, is_via_hole ? drc::ItemKind::Via : drc::ItemKind::Track, net, layer, true, true, ignore_routed, owners));
  if (state == 2) return 2;
  if (is_via_hole) {
    state = std::max(state, physical_hole_state(Shape::point(s.pts[0], hole_r), net, layer, true, true, ignore_routed, owners));
    if (state == 2) return 2;
  }
  // Board edge.
  const Coord ec = std::max<Coord>(r_.minimums.copper_edge_clearance, 0);
  bool edge_ok = true;
  egrid_->query(s.box.inflated(ec + 1), [&](int id) {
    if (edge_ok && geom::closer_than(s, edge_segs_[static_cast<std::size_t>(id)], ec)) edge_ok = false;
  });
  if (!edge_ok) return 2;
  // Pad solder-mask openings and copper text.
  bool ap_blocked = false;
  aperture_codes(s, layer, is_via_hole, [&](model::NetId n) {
    if (n == 0 || n != net) ap_blocked = true;
  });
  if (ap_blocked) return 2;
  // Keepouts and solder-mask openings.
  for (const auto& [area, z] : keepouts_)
    if ((z->copper & model::layer_bit(layer)) && (is_via_hole ? z->keepout_vias : z->keepout_tracks) && geom::closer_than(s, area, 1)) return 2;
  const int side = layer == 0 ? 0 : layer == b_.copper_count() - 1 ? 1 : -1;
  if (side >= 0)
    for (const auto& m : mask_open_[side])
      if (m.box.inflated(100'000).intersects(s.box) && geom::closer_than(s, m, 100'000)) return 2;
  return state;
}

namespace {
// Reuse rule-probe storage to avoid per-check allocation. A stack supports nested legality checks;
// pooled items have stable addresses while the pool grows.
class Probe {
 public:
  Probe(drc::ItemKind kind, const Shape& s, model::NetId net, int layer, Coord width, Point pos) {
    if (depth_ == pool_.size()) pool_.push_back(std::make_unique<drc::CopperItem>());
    item_ = pool_[depth_++].get();
    drc::CopperItem& p = *item_;
    p.kind = kind;
    p.index = -1;
    p.sub = 0;
    p.net = net;
    p.layers = layer >= 0 ? model::layer_bit(layer) : 0;
    p.shapes.resize(1);
    p.shapes[0] = s;  // reuses the pooled shape's point storage
    p.box = s.box;
    p.footprint = -1;
    p.pos = pos;
    p.width = width;
    p.owner = -1;
    p.removed = false;
    p.free_via = false;
  }
  ~Probe() { --depth_; }
  Probe(const Probe&) = delete;
  Probe& operator=(const Probe&) = delete;
  const drc::CopperItem& operator*() const { return *item_; }
  void set_layers(model::LayerMask m) const { item_->layers = m; }  // a via's span

 private:
  static thread_local std::vector<std::unique_ptr<drc::CopperItem>> pool_;
  static thread_local std::size_t depth_;
  drc::CopperItem* item_;
};
thread_local std::vector<std::unique_ptr<drc::CopperItem>> Probe::pool_;
thread_local std::size_t Probe::depth_ = 0;
// Routed hole checks do not nest, so one scratch disk per thread suffices.
const Shape& scratch_hole(Point c, Coord r) {
  static thread_local Shape h;
  h.set_point(c, r);
  return h;
}
// Track copper of a lattice disk for rule probes (rule checks do not nest).
const Shape& scratch_disk(Point c, Coord r) {
  static thread_local Shape d;
  d.set_point(c, r);
  return d;
}
int worst(int a, int c) { return std::max(a, c); }
}  // namespace

bool Obstacles::track_disallowed(const Shape& s, int layer, Coord width, model::NetId net) const {
  if (!re_->positional_disallow()) return false;
  const Probe pp(drc::ItemKind::Track, s, net, layer, width, s.pts[0]);
  return re_->probe_disallowed(*pp);
}

bool Obstacles::via_disallowed(Point p, Coord d, model::NetId net, int l0, int l1, model::ViaType type) const {
  if (!re_->positional_disallow()) return false;
  // A blind via over the whole stack is committed, written and judged by the DRC as a through via.
  if (type == model::ViaType::Blind && l0 <= 0 && l1 >= b_.copper_count() - 1) type = model::ViaType::Through;
  const Probe pp(drc::ItemKind::Via, scratch_disk(p, d / 2), net, -1, d, p);
  model::LayerMask span = 0;
  for (int l = std::max(0, l0); l <= std::min(l1, b_.copper_count() - 1); ++l) span |= model::layer_bit(l);
  pp.set_layers(span);
  return re_->probe_disallowed(*pp, type);
}

// Custom physical_hole_clearance rules hold between a hole and any other item's copper, whatever the nets
// (KiCad reports them as hole_clearance, also for a track of the hole's own net). `fixed` / `routed` select
// which existing items are tested; a conflict with routed copper is 1 when `soft` (owner appended), else 2.

// New copper `s` on `layer` against the holes of existing pads and vias.
int Obstacles::physical_copper_state(const Shape& s, drc::ItemKind kind, model::NetId net, int layer, bool fixed, bool routed, bool soft,
                                     std::vector<int>* owners) const {
  if (!re_->any_physical_hole_clearance()) return 0;
  const Probe pp(kind, s, net, layer, 2 * s.r, s.pts[0]);
  const drc::CopperItem& probe = *pp;
  int state = 0;
  hgrid_->query(s.box.inflated(re_->max_physical_hole_clearance() + 1), [&](int id) {
    if (state == 2) return;
    const auto& h = cm_.holes[static_cast<std::size_t>(id)];
    if (h.removed || h.item < 0) return;  // a hole without copper has no item for the rule's condition (as in the DRC)
    const auto& owner = cm_.items[static_cast<std::size_t>(h.item)];
    const bool is_routed = owner.owner >= 0;
    if (owner.removed || (is_routed ? !routed : !fixed) || !(owner.layers & model::layer_bit(layer))) return;
    const Coord req = re_->physical_hole_clearance(&owner, probe, layer);
    if (req <= 0 || !geom::closer_than(s, h.shape, req)) return;
    if (is_routed && soft) {
      state = 1;
      if (owners) owners->push_back(owner.owner);
    } else {
      state = 2;
    }
  });
  return state;
}

// A new via's hole against existing copper on `layer`.
int Obstacles::physical_hole_state(const Shape& hole, model::NetId net, int layer, bool fixed, bool routed, bool soft,
                                   std::vector<int>* owners) const {
  if (!re_->any_physical_hole_clearance()) return 0;
  const Probe pp(drc::ItemKind::Via, hole, net, layer, 2 * hole.r, hole.pts[0]);
  const drc::CopperItem& probe = *pp;
  int state = 0;
  grid_->query(hole.box.inflated(re_->max_physical_hole_clearance() + 1), [&](int id) {
    if (state == 2) return;
    const auto& it = cm_.items[static_cast<std::size_t>(id)];
    const bool is_routed = it.owner >= 0;
    if (it.removed || (is_routed ? !routed : !fixed) || !(it.layers & model::layer_bit(layer))) return;
    if (soft_zones_ && it.kind == drc::ItemKind::Zone) return;  // refilled after routing, around the new hole
    const Coord req = re_->physical_hole_clearance(&probe, it, layer);
    if (req <= 0) return;
    if (item_closer(hole, it, req)) {
      if (is_routed && soft) {
        state = 1;
        if (owners) owners->push_back(it.owner);
      } else {
        state = 2;
      }
    }
  });
  return state;
}

int Obstacles::disk_state(Point p, int layer, Coord hw, model::NetId net, Coord margin, bool ignore_routed, std::vector<int>* owners) const {
  ++checks;
  if (!inside_board(p, 0)) {
    ++rej_outside;
    return 2;
  }
  const Shape s = Shape::point(p, hw + margin);
  const Probe pp(drc::ItemKind::Track, s, net, layer, 2 * hw, p);
  const drc::CopperItem& probe = *pp;
  int st = copper_state(s, probe, layer, ignore_routed, owners);
  if (st == 2) {
    ++rej_copper;
    return 2;
  }
  st = worst(st, holes_edges_state(s, net, layer, false, 0, ignore_routed, owners));
  if (st != 2 && track_disallowed(scratch_disk(p, hw), layer, 2 * hw, net)) st = 2;
  if (st == 2) ++rej_other;
  return st;
}

int Obstacles::segment_state(Point a, Point b, int layer, Coord width, model::NetId net, bool ignore_routed, std::vector<int>* owners) const {
  const Shape s = Shape::segment(a, b, width / 2);
  const Probe pp(drc::ItemKind::Track, s, net, layer, width, a);
  const drc::CopperItem& probe = *pp;
  const int st = copper_state(s, probe, layer, ignore_routed, owners);
  if (st == 2) return 2;
  const int st2 = worst(st, holes_edges_state(s, net, layer, false, 0, ignore_routed, owners));
  return st2 != 2 && track_disallowed(s, layer, width, net) ? 2 : st2;
}

int Obstacles::via_state_span(Point p, Coord d, Coord drill, model::NetId net, Coord margin, bool ignore_routed, std::vector<int>* owners, int l0,
                              int l1, model::ViaType type) const {
  if (!inside_board(p, 0) || via_disallowed(p, d, net, l0, l1, type)) return 2;
  const Shape s = Shape::point(p, d / 2 + margin);
  int st = 0;
  for (int l = std::max(0, l0); l <= std::min(l1, b_.copper_count() - 1) && st != 2; ++l) {
    const Probe pp(drc::ItemKind::Via, s, net, l, d, p);
    const drc::CopperItem& probe = *pp;
    st = worst(st, copper_state(s, probe, l, ignore_routed, owners));
    if (st != 2) st = worst(st, holes_edges_state(s, net, l, true, drill / 2 + margin, ignore_routed, owners));
  }
  return st;
}

int Obstacles::via_state(Point p, Coord d, Coord drill, model::NetId net, Coord margin, bool ignore_routed, std::vector<int>* owners) const {
  if (!inside_board(p, 0) || via_disallowed(p, d, net, 0, b_.copper_count() - 1, model::ViaType::Through)) return 2;
  const Shape s = Shape::point(p, d / 2 + margin);
  int st = 0;
  for (int l = 0; l < b_.copper_count() && st != 2; ++l) {
    const Probe pp(drc::ItemKind::Via, s, net, l, d, p);
    const drc::CopperItem& probe = *pp;
    st = worst(st, copper_state(s, probe, l, ignore_routed, owners));
    if (st != 2) st = worst(st, holes_edges_state(s, net, l, true, drill / 2 + margin, ignore_routed, owners));
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

void Obstacles::aperture_codes(const Shape& s, int layer, bool via_probe, const std::function<void(model::NetId)>& hit) const {
  const int side = layer == 0 ? 0 : layer == b_.copper_count() - 1 ? 1 : -1;
  if (side >= 0) {
    const Coord extra = 1'000 + (via_probe ? via_mask_ : 0);
    agrid_[side]->query(s.box.inflated(extra + 1), [&](int id) {
      const auto& a = apertures_[side][static_cast<std::size_t>(id)];
      for (const auto& u : a.shapes)
        if (geom::closer_than(s, u, a.margin + extra)) {
          hit(a.net);
          return;
        }
    });
  }
  const Coord tc = std::max(r_.minimums.clearance, r_.default_class().clearance);
  for (const auto& [l, t] : texts_)
    if (l == layer && t.box.inflated(tc).intersects(s.box) && geom::closer_than(s, t, tc)) hit(0);
}

std::int32_t Obstacles::fixed_code(Point p, int layer, Coord hw, Coord margin, model::NetId probe_net, bool via_probe) const {
  if (!inside_board(p, 0)) return kBlocked;
  static thread_local Shape s;  // scratch disk; fixed_code does not re-enter
  s.set_point(p, hw + margin);
  const Probe pp(drc::ItemKind::Track, s, probe_net, layer, 2 * hw, p);
  const drc::CopperItem& probe = *pp;
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
    if (soft_zones_ && it.kind == drc::ItemKind::Zone) return;
    if (via_probe && via_keeps_off(it)) {
      const Coord rq = re_->clearance(probe, it, layer);
      if (item_closer(s, it, rq)) {
        code = kBlocked;
        return;
      }
    }
    if (it.net != 0 && code == it.net) return;  // already known: only legal for this net
    Coord req = re_->clearance(probe, it, layer);
    if (via_mask_ > 0 && (layer == 0 || layer == b_.copper_count() - 1) && it.kind != drc::ItemKind::Zone && it.kind != drc::ItemKind::Pad) {
      if (it.kind == drc::ItemKind::Via) req = std::max(req, via_mask_ + (via_probe ? via_mask_ : 0) + 1'000);
      else if (via_probe) req = std::max(req, via_mask_ + 1'000);
    }
    if (item_closer(s, it, req)) add_net(it.net);
  });
  if (code == kBlocked) return code;
  aperture_codes(s, layer, via_probe, add_net);
  if (code == kBlocked) return code;
  const Coord hc = std::max<Coord>(r_.minimums.hole_clearance, 0);
  hgrid_->query(s.box.inflated(std::max(hc, max_hole_local_) + 1), [&](int id) {
    if (code == kBlocked) return;
    const auto& h = cm_.holes[static_cast<std::size_t>(id)];
    if (h.removed || (h.item >= 0 && cm_.items[static_cast<std::size_t>(h.item)].owner >= 0)) return;
    if (geom::closer_than(s, h.shape, std::max(hc, h.clearance))) {
      if (h.plated && h.net != 0) add_net(h.net);
      else code = kBlocked;
    }
  });
  if (code == kBlocked) return code;
  if (physical_copper_state(s, via_probe ? drc::ItemKind::Via : drc::ItemKind::Track, probe_net, layer, true, false, false, nullptr) != 0) return kBlocked;
  const Coord ec = std::max<Coord>(r_.minimums.copper_edge_clearance, 0);
  bool edge_ok = true;
  egrid_->query(s.box.inflated(ec + 1), [&](int id) {
    if (edge_ok && geom::closer_than(s, edge_segs_[static_cast<std::size_t>(id)], ec)) edge_ok = false;
  });
  if (!edge_ok) return kBlocked;
  for (const auto& [area, z] : keepouts_)
    if ((z->copper & model::layer_bit(layer)) && (via_probe ? z->keepout_vias : z->keepout_tracks) && geom::closer_than(s, area, 1)) return kBlocked;
  const int side = layer == 0 ? 0 : layer == b_.copper_count() - 1 ? 1 : -1;
  if (side >= 0)
    for (const auto& m : mask_open_[side])
      if (m.box.inflated(100'000).intersects(s.box) && geom::closer_than(s, m, 100'000)) return kBlocked;  // 0.1 mm: KiCad flags near misses
  return code;
}

// Combine fixed-copper codes across layers in one query: any block or two different nets block.
// Layer-independent checks run once; fixed_via_code_reference retains the per-layer path.
std::int32_t Obstacles::fixed_via_code(Point p, Coord d, Coord drill, Coord margin, model::NetId probe_net) const {
  if (!inside_board(p, 0)) return kBlocked;
  const int nl = b_.copper_count();
  const Coord hw = d / 2;
  static thread_local Shape s;  // scratch disk
  s.set_point(p, hw + margin);
  std::int32_t code = kFree;
  auto add_net = [&](model::NetId n) {
    if (n == 0) code = kBlocked;
    else if (code == kFree) code = n;
    else if (code != n) code = kBlocked;
  };
  {
    std::array<std::optional<Probe>, 64> probes;  // one per layer; reverse destruction preserves the pool stack
    for (int l = 0; l < nl; ++l) probes[static_cast<std::size_t>(l)].emplace(drc::ItemKind::Track, s, probe_net, l, 2 * hw, p);
    const model::LayerMask all = nl >= 64 ? ~model::LayerMask{0} : (model::LayerMask{1} << nl) - 1;
    grid_->query(s.box.inflated(re_->max_clearance() + 1), [&](int id) {
      if (code == kBlocked) return;
      const auto& it = cm_.items[static_cast<std::size_t>(id)];
      if (it.owner >= 0 || it.removed || !(it.layers & all)) return;
      // Same order as fixed_code, so the two paths agree with the plane-aware options on (doc 05 §26): soft
      // zone fills are no obstacle, and a pad that vias keep off blocks the via whatever its net.
      if (soft_zones_ && it.kind == drc::ItemKind::Zone) return;
      if (via_keeps_off(it)) {
        for (int l = 0; l < nl; ++l) {
          if (!(it.layers & model::layer_bit(l))) continue;
          const Coord rq = re_->clearance(**probes[static_cast<std::size_t>(l)], it, l);
          if (item_closer(s, it, rq)) {
            code = kBlocked;
            return;
          }
        }
      }
      if (it.net != 0 && code == it.net) return;  // already known: only legal for this net
      for (int l = 0; l < nl; ++l) {
        if (!(it.layers & model::layer_bit(l))) continue;
        Coord req = re_->clearance(**probes[static_cast<std::size_t>(l)], it, l);
        if (via_mask_ > 0 && (l == 0 || l == nl - 1) && it.kind != drc::ItemKind::Zone && it.kind != drc::ItemKind::Pad)
          req = std::max(req, it.kind == drc::ItemKind::Via ? 2 * via_mask_ + 1'000 : via_mask_ + 1'000);
        if (item_closer(s, it, req)) {
          add_net(it.net);
          return;
        }
      }
    });
  }
  if (code == kBlocked) return code;
  for (int l = 0; l < nl && code != kBlocked; ++l) aperture_codes(s, l, true, add_net);
  if (code == kBlocked) return code;
  const Coord hc = std::max<Coord>(r_.minimums.hole_clearance, 0);
  hgrid_->query(s.box.inflated(std::max(hc, max_hole_local_) + 1), [&](int id) {
    if (code == kBlocked) return;
    const auto& h = cm_.holes[static_cast<std::size_t>(id)];
    if (h.removed || (h.item >= 0 && cm_.items[static_cast<std::size_t>(h.item)].owner >= 0)) return;
    if (geom::closer_than(s, h.shape, std::max(hc, h.clearance))) {
      if (h.plated && h.net != 0) add_net(h.net);
      else code = kBlocked;
    }
  });
  if (code == kBlocked) return code;
  for (int l = 0; l < nl; ++l)
    if (physical_copper_state(s, drc::ItemKind::Via, probe_net, l, true, false, false, nullptr) != 0) return kBlocked;
  const Coord ec = std::max<Coord>(r_.minimums.copper_edge_clearance, 0);
  bool edge_ok = true;
  egrid_->query(s.box.inflated(ec + 1), [&](int id) {
    if (edge_ok && geom::closer_than(s, edge_segs_[static_cast<std::size_t>(id)], ec)) edge_ok = false;
  });
  if (!edge_ok) return kBlocked;
  const model::LayerMask copper = nl >= 64 ? ~model::LayerMask{0} : (model::LayerMask{1} << nl) - 1;
  for (const auto& [area, z] : keepouts_)
    if (z->keepout_vias && (z->copper & copper) && geom::closer_than(s, area, 1)) return kBlocked;
  for (int side = 0; side < (nl > 1 ? 2 : 1); ++side)
    for (const auto& m : mask_open_[side])
      if (m.box.inflated(100'000).intersects(s.box) && geom::closer_than(s, m, 100'000)) return kBlocked;  // 0.1 mm: KiCad flags near misses
  return via_hole_code(p, drill, margin, probe_net, code);
}

std::int32_t Obstacles::fixed_via_code_reference(Point p, Coord d, Coord drill, Coord margin, model::NetId probe_net) const {
  std::int32_t code = kFree;
  for (int l = 0; l < b_.copper_count(); ++l) {
    const std::int32_t c = fixed_code(p, l, d / 2, margin, probe_net, true);
    if (c == kBlocked) return kBlocked;
    if (c != kFree) {
      if (code == kFree) code = c;
      else if (code != c) return kBlocked;
    }
  }
  return via_hole_code(p, drill, margin, probe_net, code);
}

// Check the via hole against fixed holes and copper, continuing from the pad's code.
std::int32_t Obstacles::via_hole_code(Point p, Coord drill, Coord margin, model::NetId probe_net, std::int32_t code) const {
  // Hole to hole against fixed holes (any net).
  const Coord h2h = std::max<Coord>(r_.minimums.hole_to_hole, 0);
  static thread_local Shape hole;  // scratch disk: the via's hole
  hole.set_point(p, drill / 2 + margin);
  bool ok = true;
  hgrid_->query(hole.box.inflated(h2h + 1), [&](int id) {
    const auto& h = cm_.holes[static_cast<std::size_t>(id)];
    if (!ok || h.removed || (h.item >= 0 && cm_.items[static_cast<std::size_t>(h.item)].owner >= 0)) return;
    if (geom::closer_than(hole, h.shape, h2h) || hole.pts[0] == h.shape.pts[0]) ok = false;
  });
  if (!ok) return kBlocked;
  for (int l = 0; l < b_.copper_count(); ++l)
    if (physical_hole_state(hole, probe_net, l, true, false, false, nullptr) != 0) return kBlocked;
  const Coord hc = std::max<Coord>(r_.minimums.hole_clearance, 0);
  if (hc > 0) {
    const Shape& h = hole;
    grid_->query(h.box.inflated(hc + 1), [&](int id) {
      if (code == kBlocked) return;
      const auto& it = cm_.items[static_cast<std::size_t>(id)];
      if (it.owner >= 0 || it.removed || it.kind == drc::ItemKind::Zone) return;
      if (it.net != 0 && code == it.net) return;
      for (const auto& u : it.shapes)
        if (geom::closer_than(h, u, hc)) {
          if (it.net == 0) code = kBlocked;
          else if (code == kFree) code = it.net;
          else if (code != it.net) code = kBlocked;
          return;
        }
    });
  }
  return code;
}

// The parts of routed_state, each 0 free, 1 conflict (owners appended), 2 blocked (when !soft).
// New copper against routed copper of other nets on `layer`.
int Obstacles::routed_copper_part(const Shape& s, int layer, model::NetId net, drc::ItemKind kind, bool soft, std::vector<int>* owners) const {
  const Probe pp(kind, s, net, layer, 2 * s.r, s.pts[0]);
  const drc::CopperItem& probe = *pp;
  int state = 0;
  rgrid_->query(s.box.inflated(re_->max_clearance() + 1), [&](int id) {
    if (state == 2) return;
    const auto& it = cm_.items[static_cast<std::size_t>(id)];
    if (it.removed || !(it.layers & model::layer_bit(layer))) return;
    if (it.net == net && net != 0) return;
    Coord req = re_->clearance(probe, it, layer);
    if (via_mask_ > 0 && (layer == 0 || layer == b_.copper_count() - 1)) {
      if (it.kind == drc::ItemKind::Via) req = std::max(req, via_mask_ + (kind == drc::ItemKind::Via ? via_mask_ : 0) + 1'000);
      else if (kind == drc::ItemKind::Via) req = std::max(req, via_mask_ + 1'000);
    }
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
  return state;
}

// New copper against routed vias' holes of other nets (hole clearance; vias span every layer).
int Obstacles::routed_via_holes_part(const Shape& s, model::NetId net, Coord hc, bool soft, std::vector<int>* owners) const {
  int state = 0;
  rgrid_->query(s.box.inflated(hc + 1), [&](int id) {
    if (state == 2) return;
    const auto& it = cm_.items[static_cast<std::size_t>(id)];
    if (it.removed || it.kind != drc::ItemKind::Via || (it.net == net && net != 0)) return;
    if (geom::closer_than_disk(s, it.pos, b_.vias[static_cast<std::size_t>(it.index)].drill / 2, hc)) {
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

// A new via's hole against routed copper of other nets on `layer` (hole clearance).
int Obstacles::routed_hole_copper_part(const Shape& h, int layer, model::NetId net, Coord hc, bool soft, std::vector<int>* owners) const {
  int state = 0;
  rgrid_->query(h.box.inflated(hc + 1), [&](int id) {
    if (state == 2) return;
    const auto& it = cm_.items[static_cast<std::size_t>(id)];
    if (it.removed || (it.net == net && net != 0) || !(it.layers & model::layer_bit(layer))) return;
    for (const auto& u : it.shapes)
      if (geom::closer_than(h, u, hc)) {
        if (soft) {
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

// A new via hole against routed vias' holes (hole to hole, any net).
int Obstacles::routed_hole_to_hole_part(const Shape& hole, bool soft, std::vector<int>* owners) const {
  const Coord h2h = std::max<Coord>(r_.minimums.hole_to_hole, 0);
  int state = 0;
  rgrid_->query(hole.box.inflated(h2h + 1), [&](int id) {
    if (state == 2) return;
    const auto& it = cm_.items[static_cast<std::size_t>(id)];
    if (it.removed || it.kind != drc::ItemKind::Via) return;
    if (geom::closer_than_disk(hole, it.pos, b_.vias[static_cast<std::size_t>(it.index)].drill / 2, h2h)) {
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

int Obstacles::routed_state(const Shape& s, int layer, model::NetId net, drc::ItemKind kind, bool soft, std::vector<int>* owners,
                            bool via_hole, Coord hole_r) const {
  int state = routed_copper_part(s, layer, net, kind, soft, owners);
  if (state == 2) return state;
  const Coord hc = std::max<Coord>(r_.minimums.hole_clearance, 0);
  if (hc > 0) {
    state = std::max(state, routed_via_holes_part(s, net, hc, soft, owners));
    if (via_hole && state != 2) state = std::max(state, routed_hole_copper_part(scratch_hole(s.pts[0], hole_r), layer, net, hc, soft, owners));
  }
  if (state != 2) state = std::max(state, physical_copper_state(s, kind, net, layer, false, true, soft, owners));
  if (state == 2 || !via_hole) return state;
  state = std::max(state, physical_hole_state(scratch_hole(s.pts[0], hole_r), net, layer, false, true, soft, owners));
  if (state == 2) return state;
  return std::max(state, routed_hole_to_hole_part(scratch_hole(s.pts[0], hole_r), soft, owners));
}

int Obstacles::routed_via_state(const Shape& s, model::LayerMask layers, model::NetId net, bool soft, Coord hole_r) const {
  if (!layers) return 0;
  const Coord hc = std::max<Coord>(r_.minimums.hole_clearance, 0);
  const Shape& hole = scratch_hole(s.pts[0], hole_r);
  int state = 0;
  for (int l = 0; l < b_.copper_count(); ++l) {
    if (!(layers & model::layer_bit(l))) continue;
    state = std::max(state, routed_copper_part(s, l, net, drc::ItemKind::Via, soft, nullptr));
    if (state == 2) return state;
    if (hc > 0) state = std::max(state, routed_hole_copper_part(hole, l, net, hc, soft, nullptr));
    if (state == 2) return state;
    state = std::max(state, physical_copper_state(s, drc::ItemKind::Via, net, l, false, true, soft, nullptr));
    if (state != 2) state = std::max(state, physical_hole_state(hole, net, l, false, true, soft, nullptr));
    if (state == 2) return state;
  }
  if (hc > 0) state = std::max(state, routed_via_holes_part(s, net, hc, soft, nullptr));
  if (state == 2) return state;
  return std::max(state, routed_hole_to_hole_part(hole, soft, nullptr));
}

void Obstacles::routed_items_in(const geom::Box& box, std::vector<int>& out) const {
  rgrid_->query(box, [&](int id) {
    const auto& it = cm_.items[static_cast<std::size_t>(id)];
    if (!it.removed && it.box.intersects(box)) out.push_back(id);
  });
}

long double zone_fill_area(const drc::CopperModel& cm, const std::vector<int>& items) {
  long double a = 0;
  for (int z : items)
    for (const auto& sh : cm.items[static_cast<std::size_t>(z)].shapes) {
      const auto& p = sh.pts;
      long double s = 0;
      for (std::size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
        s += static_cast<long double>(p[j].x) * static_cast<long double>(p[i].y) - static_cast<long double>(p[i].x) * static_cast<long double>(p[j].y);
      a += s < 0 ? -s / 2 : s / 2;
    }
  return a;
}

}  // namespace tmk::route

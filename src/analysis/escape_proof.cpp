// SPDX-License-Identifier: GPL-3.0-or-later
#include "analysis/escape_proof.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>

#include "drc/copper.hpp"
#include "drc/rule_engine.hpp"
#include "geom/shape.hpp"

namespace tmk::analysis {
namespace {

using geom::Point;
using LD = long double;

std::string mm(Coord c) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3g mm", nm_to_mm(c));
  return buf;
}

// A wall: copper of another net (or a hole, or the edge) and how far a probe's centre must stay from it.
struct Wall {
  const geom::Shape* shape;
  Coord keep;        // clearance plus the probe's half width (the shape's own radius is added when marking)
  std::string what;  // for the user: "U3.6", "the GND zone fill", "the board edge"
  bool zone = false;
  Point pos;         // where it is, to list the nearest first
};

// The grid around one pin: a cell is blocked when its centre is closer to a wall than `keep - slack`.
struct Grid {
  Coord x0 = 0, y0 = 0, cell = 0;
  int n = 0;  // n x n cells
  std::vector<std::int32_t> owner;  // -1 free, else the wall index that blocks the cell
  Point centre(int i, int j) const { return {x0 + static_cast<Coord>(i) * cell + cell / 2, y0 + static_cast<Coord>(j) * cell + cell / 2}; }
  int at(int i, int j) const { return j * n + i; }
};

// Marks the cells within `reach` of the shape's copper (its skeleton grown by its radius), and the inside of a closed shape.
void mark(Grid& g, const geom::Shape& s, Coord keep, Coord slack, int id) {
  const LD reach = LD(keep) + LD(s.r) - LD(slack);
  if (reach <= 0 && !s.closed) return;
  auto block = [&](int i, int j) {
    auto& o = g.owner[static_cast<std::size_t>(g.at(i, j))];
    if (o < 0) o = id;
  };
  auto cells = [&](Coord lo, Coord hi, Coord origin, int& a, int& b) {
    a = std::max(0, static_cast<int>(std::floor(LD(lo - origin) / LD(g.cell))));
    b = std::min(g.n - 1, static_cast<int>(std::floor(LD(hi - origin) / LD(g.cell))));
  };
  auto seg = [&](Point a, Point b) {
    const Coord r = static_cast<Coord>(std::ceil(std::max<LD>(reach, 0)));
    int i0, i1, j0, j1;
    cells(std::min(a.x, b.x) - r, std::max(a.x, b.x) + r, g.x0, i0, i1);
    cells(std::min(a.y, b.y) - r, std::max(a.y, b.y) + r, g.y0, j0, j1);
    for (int j = j0; j <= j1; ++j)
      for (int i = i0; i <= i1; ++i)
        if (geom::point_seg_dist(g.centre(i, j), a, b) < reach) block(i, j);
  };
  if (s.pts.size() == 1) {
    seg(s.pts[0], s.pts[0]);
  } else {
    for (std::size_t k = 0; k + 1 < s.pts.size(); ++k) seg(s.pts[k], s.pts[k + 1]);
    if (s.closed && s.pts.size() > 2) seg(s.pts.back(), s.pts.front());
  }
  if (s.closed && s.pts.size() > 2) {  // the inside, row by row (crossings of the row's centre line)
    Coord ylo = s.pts[0].y, yhi = ylo;
    for (const auto& p : s.pts) ylo = std::min(ylo, p.y), yhi = std::max(yhi, p.y);
    int j0, j1;
    cells(ylo, yhi, g.y0, j0, j1);
    std::vector<LD> xs;
    for (int j = j0; j <= j1; ++j) {
      const LD y = LD(g.centre(0, j).y);
      xs.clear();
      for (std::size_t k = 0; k < s.pts.size(); ++k) {
        const Point a = s.pts[k], b = s.pts[(k + 1) % s.pts.size()];
        if ((LD(a.y) <= y) != (LD(b.y) <= y)) xs.push_back(LD(a.x) + (y - LD(a.y)) * LD(b.x - a.x) / LD(b.y - a.y));
      }
      std::sort(xs.begin(), xs.end());
      for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
        int i0, i1;
        cells(static_cast<Coord>(std::ceil(xs[k])), static_cast<Coord>(std::floor(xs[k + 1])), g.x0 + g.cell / 2, i0, i1);
        for (int i = i0; i <= i1; ++i) block(i, j);
      }
    }
  }
}

}  // namespace

std::string pad_label(const model::Board& board, int pad) {
  if (pad < 0 || static_cast<std::size_t>(pad) >= board.pads.size()) return "?";
  const auto& p = board.pads[static_cast<std::size_t>(pad)];
  const std::string ref = p.footprint >= 0 && static_cast<std::size_t>(p.footprint) < board.footprints.size()
                              ? board.footprints[static_cast<std::size_t>(p.footprint)].reference
                              : std::string("?");
  return ref + "." + p.number;
}

struct EscapeAnalyser::Impl {
  const model::Board& b;
  const model::DesignRules& rules;
  EscapeOptions opt;
  drc::CopperModel cm;
  drc::RuleEngine re;
  std::vector<int> item_of_pad;  // board pad -> copper item with its copper (-1: none)
  Impl(const model::Board& board, const model::DesignRules& r, EscapeOptions o) : b(board), rules(r), opt(o), cm(drc::build_copper(board)), re(board, r) {
    item_of_pad.assign(b.pads.size(), -1);
    for (std::size_t i = 0; i < cm.items.size(); ++i) {
      const auto& it = cm.items[i];
      if (it.kind == drc::ItemKind::Pad && it.bare_of < 0 && it.index >= 0) item_of_pad[static_cast<std::size_t>(it.index)] = static_cast<int>(i);
    }
  }

  // The walls a probe (a track of `width`, or a via of diameter `width` when `via`) of `net` meets on `layer` near `box`.
  // `soft`: zone fills of other nets are no walls (route --soft-zones), nor net-less copper graphics that touch `pin` (route
  // --soft-copper-graphics gives them the pin's net).
  std::vector<Wall> walls(model::NetId net, int layer, Coord width, bool via, const geom::Box& box, bool soft, const drc::CopperItem* pin, Coord drill = 0) const {
    drc::CopperItem probe;
    probe.kind = via ? drc::ItemKind::Via : drc::ItemKind::Track;
    probe.net = net;
    probe.layers = via ? model::LayerMask((model::LayerMask{1} << b.copper_count()) - 1) : model::layer_bit(layer);
    probe.width = width;
    std::vector<Wall> out;
    const Coord reach = width / 2 + re.max_clearance() + 1'000'000;
    const geom::Box near{box.x0 - reach, box.y0 - reach, box.x1 + reach, box.y1 + reach};
    for (const auto& it : cm.items) {
      if (it.removed || it.net == net || !(it.layers & model::layer_bit(layer)) || it.shapes.empty()) continue;
      if (soft && it.kind == drc::ItemKind::Zone) continue;
      if (soft && it.kind == drc::ItemKind::Graphic && it.net == 0 && pin && touches(*pin, it)) continue;
      if (it.box.x1 < near.x0 || it.box.x0 > near.x1 || it.box.y1 < near.y0 || it.box.y0 > near.y1) continue;
      const Coord keep = width / 2 + re.clearance(probe, it, layer);
      const std::string nn = it.net > 0 && static_cast<std::size_t>(it.net) < b.nets.size() ? b.nets[static_cast<std::size_t>(it.net)].name : "no-net";
      std::string what;
      switch (it.kind) {
        case drc::ItemKind::Pad: what = pad_label(b, it.bare_of >= 0 ? cm.items[static_cast<std::size_t>(it.bare_of)].index : it.index); break;
        case drc::ItemKind::Zone: what = "the " + nn + " zone fill"; break;
        case drc::ItemKind::Via: what = "a " + nn + " via"; break;
        case drc::ItemKind::Graphic: what = "copper graphics"; break;
        default: what = "a " + nn + " track"; break;
      }
      const bool soft_wall = it.kind == drc::ItemKind::Zone || (it.kind == drc::ItemKind::Graphic && it.net == 0 && pin && touches(*pin, it));
      for (const auto& s : it.shapes) out.push_back({&s, keep, what, soft_wall, it.pos});
    }
    for (const auto& h : cm.holes) {
      if (h.removed || (h.net == net && h.net != 0)) continue;
      const drc::CopperItem* owner = h.item >= 0 ? &cm.items[static_cast<std::size_t>(h.item)] : nullptr;
      Coord keep = width / 2 + re.hole_clearance(owner, probe, layer);
      if (via) keep = std::max(keep, drill / 2 + rules.minimums.hole_to_hole);  // the via's own hole against this one
      out.push_back({&h.shape, keep, h.pad >= 0 ? pad_label(b, h.pad) + "'s hole" : std::string("a hole"), false, h.pos});
    }
    for (const auto* list : {&cm.edges, &cm.margins})
      for (const auto& e : *list) out.push_back({&e, width / 2 + re.edge_clearance(probe, layer), "the board edge", false, e.pts.empty() ? Point{} : e.pts[0]});
    return out;
  }

  static bool touches(const drc::CopperItem& a, const drc::CopperItem& b) {
    if (!(a.layers & b.layers) || a.box.x1 < b.box.x0 || b.box.x1 < a.box.x0 || a.box.y1 < b.box.y0 || b.box.y1 < a.box.y0) return false;
    for (const auto& sa : a.shapes)
      for (const auto& sb : b.shapes)
        if (geom::closer_than(sa, sb, 1)) return true;
    return false;
  }

  // The square searched: `opt.window` beyond the pin's copper on every side (a track that gets that far is out).
  Grid grid_around(Point c, const drc::CopperItem& pin) const {
    Grid g;
    g.cell = std::max<Coord>(opt.cell, 1'000);
    const Coord half = std::max({c.x - pin.box.x0, pin.box.x1 - c.x, c.y - pin.box.y0, pin.box.y1 - c.y, Coord{0}}) + opt.window;
    g.n = static_cast<int>(2 * half / g.cell) + 1;
    g.x0 = c.x - static_cast<Coord>(g.n / 2) * g.cell - g.cell / 2;
    g.y0 = c.y - static_cast<Coord>(g.n / 2) * g.cell - g.cell / 2;
    g.owner.assign(static_cast<std::size_t>(g.n) * static_cast<std::size_t>(g.n), -1);
    return g;
  }

  geom::Box box_of(const Grid& g) const { return {g.x0, g.y0, g.x0 + static_cast<Coord>(g.n) * g.cell, g.y0 + static_cast<Coord>(g.n) * g.cell}; }

  // Floods the free cells from the pin's copper: reached cells (1), and whether the flood left the window or touched other
  // copper of the net.
  struct Flood {
    std::vector<std::uint8_t> reached;
    bool out = false;
  };
  Flood flood(const Grid& g, const drc::CopperItem& pin, const Grid& goal, Coord half_width) const {
    Flood f;
    f.reached.assign(g.owner.size(), 0);
    std::deque<int> q;
    const Coord slack = g.cell * 7072 / 10000;
    for (int j = 0; j < g.n; ++j)
      for (int i = 0; i < g.n; ++i) {
        const int k = g.at(i, j);
        if (g.owner[static_cast<std::size_t>(k)] >= 0) continue;
        bool in = false;
        for (const auto& s : pin.shapes) {
          const Point c = g.centre(i, j);
          if ((s.closed && s.pts.size() > 2 && geom::point_in_polygon(c, s.pts)) ||
              [&] {
                LD best = 1e30L;
                if (s.pts.size() == 1) best = geom::point_seg_dist(c, s.pts[0], s.pts[0]);
                for (std::size_t m = 0; m + 1 < s.pts.size(); ++m) best = std::min(best, geom::point_seg_dist(c, s.pts[m], s.pts[m + 1]));
                if (s.closed && s.pts.size() > 2) best = std::min(best, geom::point_seg_dist(c, s.pts.back(), s.pts.front()));
                return best <= LD(s.r) + LD(half_width) + LD(slack);
              }())
            in = true;
        }
        if (in) {
          f.reached[static_cast<std::size_t>(k)] = 1;
          q.push_back(k);
        }
      }
    while (!q.empty()) {
      const int k = q.front();
      q.pop_front();
      const int i = k % g.n, j = k / g.n;
      if (i == 0 || j == 0 || i == g.n - 1 || j == g.n - 1 || goal.owner[static_cast<std::size_t>(k)] >= 0) {
        f.out = true;
        return f;
      }
      for (int dj = -1; dj <= 1; ++dj)
        for (int di = -1; di <= 1; ++di) {
          const int m = g.at(i + di, j + dj);
          if (f.reached[static_cast<std::size_t>(m)] || g.owner[static_cast<std::size_t>(m)] >= 0) continue;
          f.reached[static_cast<std::size_t>(m)] = 1;
          q.push_back(m);
        }
    }
    return f;
  }

  PinEscape analyse(int pad, bool soft) const;
};

EscapeAnalyser::EscapeAnalyser(const model::Board& board, const model::DesignRules& rules, EscapeOptions opt) : impl_(new Impl(board, rules, opt)) {}
EscapeAnalyser::~EscapeAnalyser() { delete impl_; }

PinEscape EscapeAnalyser::Impl::analyse(int pad, bool soft) const {
  const Impl& m = *this;
  PinEscape r;
  r.pad = pad;
  if (pad < 0 || static_cast<std::size_t>(pad) >= m.b.pads.size()) return r;
  const int item_i = m.item_of_pad[static_cast<std::size_t>(pad)];
  const auto& P = m.b.pads[static_cast<std::size_t>(pad)];
  if (item_i < 0 || P.net <= 0) return r;
  const auto& pin = m.cm.items[static_cast<std::size_t>(item_i)];
  const std::string net_name = static_cast<std::size_t>(P.net) < m.b.nets.size() ? m.b.nets[static_cast<std::size_t>(P.net)].name : std::string();
  const auto& nc = m.rules.class_for(net_name);
  r.analysed = true;
  r.width = nc.track_width;
  const Coord slack = std::max<Coord>(m.opt.cell, 1'000) * 7072 / 10000;
  const int nl = m.b.copper_count();

  // Goal cells: other copper of the net (reaching it, the net goes on from there).
  auto goal_grid = [&](const Grid& like, int layer, Coord half_width) {
    Grid g = like;
    std::fill(g.owner.begin(), g.owner.end(), -1);
    for (const auto& it : m.cm.items) {
      if (it.removed || it.net != P.net || &it == &pin || !(it.layers & model::layer_bit(layer))) continue;
      if (it.kind == drc::ItemKind::Pad && it.index >= 0 && m.b.pads[static_cast<std::size_t>(it.index)].footprint == P.footprint &&
          m.b.pads[static_cast<std::size_t>(it.index)].number == P.number)
        continue;  // the same pin's other copper (a padstack split): not another connection point
      for (const auto& s : it.shapes) mark(g, s, half_width, -slack, 0);  // touching it connects
    }
    return g;
  };

  // Does a track of `width` get out of the pin on `layer`? (reached cells kept for the via test)
  auto try_layer = [&](int layer, Coord width, Grid& g, Impl::Flood& f) {
    g = m.grid_around(P.pos, pin);
    const auto ws = m.walls(P.net, layer, width, false, m.box_of(g), soft, &pin);
    for (std::size_t w = 0; w < ws.size(); ++w) mark(g, *ws[w].shape, ws[w].keep, slack, static_cast<int>(w));
    const Grid goal = goal_grid(g, layer, width / 2);
    f = m.flood(g, pin, goal, width / 2);
    return std::make_pair(f.out, ws);
  };

  r.clearance = nc.clearance;  // reported; each wall carries the clearance the rules give for it

  // Is the pin walled in for tracks of width `w` and vias of diameter `via_d` (hole `drill`)? A via counts when it fits in the space
  // the track reaches (in the pin's own pad only with --via-in-pad).
  struct Walled {
    bool any_layer = false, out = false, via = false, via_in_pad = false;
    int out_layer = -1;
    std::vector<int> layers;
    std::map<std::string, std::pair<LD, bool>> walls;  // what -> (distance from the pin, a soft wall)
  };
  auto walled = [&](Coord w, Coord via_d, Coord drill, bool want_walls) {
    Walled res;
    Grid via_ok;
    bool via_grid_built = false;
    for (int L = 0; L < nl; ++L) {
      if (!(pin.layers & model::layer_bit(L))) continue;
      res.any_layer = true;
      Grid g;
      Impl::Flood f;
      const auto [out, ws] = try_layer(L, w, g, f);
      if (out) {
        res.out = true;
        res.out_layer = L;
        return res;
      }
      res.layers.push_back(L);
      if (want_walls)  // what blocks the cells next to the reached space
        for (int j = 1; j + 1 < g.n; ++j)
          for (int i = 1; i + 1 < g.n; ++i) {
            if (!f.reached[static_cast<std::size_t>(g.at(i, j))]) continue;
            for (const int k : {g.at(i + 1, j), g.at(i - 1, j), g.at(i, j + 1), g.at(i, j - 1)}) {
              const int o = g.owner[static_cast<std::size_t>(k)];
              if (o < 0) continue;
              const Wall& wl = ws[static_cast<std::size_t>(o)];
              const LD d = std::hypot(LD(wl.pos.x - P.pos.x), LD(wl.pos.y - P.pos.y));
              auto it = res.walls.find(wl.what);
              if (it == res.walls.end() || d < it->second.first) res.walls[wl.what] = {d, wl.zone};
            }
          }
      if (!via_grid_built) {  // cells where the via keeps its clearances on every copper layer
        via_ok = m.grid_around(P.pos, pin);
        for (int V = 0; V < nl; ++V) {
          const auto vs = m.walls(P.net, V, via_d, true, m.box_of(via_ok), soft, &pin, drill);
          for (std::size_t v = 0; v < vs.size(); ++v) mark(via_ok, *vs[v].shape, vs[v].keep, slack, static_cast<int>(v));
        }
        via_grid_built = true;
      }
      for (int j = 0; j < g.n && !res.via; ++j)
        for (int i = 0; i < g.n; ++i) {
          const int k = g.at(i, j);
          if (!f.reached[static_cast<std::size_t>(k)] || via_ok.owner[static_cast<std::size_t>(k)] >= 0) continue;
          const Point c = g.centre(i, j);
          bool in_pad = false;
          for (const auto& sh : pin.shapes)
            if (sh.closed && sh.pts.size() > 2 && geom::point_in_polygon(c, sh.pts)) in_pad = true;
          if (in_pad && !m.opt.via_in_pad) {
            res.via_in_pad = true;
            continue;
          }
          res.via = true;
          break;
        }
      if (res.via) return res;
    }
    return res;
  };

  const std::string who = pad_label(m.b, pad) + " (" + net_name + ")";
  const Walled at_class = walled(nc.track_width, nc.via_diameter, nc.via_drill, true);
  if (!at_class.any_layer) {
    r.analysed = false;
    return r;
  }
  if (at_class.out || at_class.via) {
    r.text = who + " can get out past the board's fixed copper" +
             (at_class.out ? " on " + m.b.copper_name(at_class.out_layer) : std::string(" through a via")) +
             ": what keeps it open is the routed copper of other nets (congestion), not the placement.";
    return r;
  }
  r.layers_enclosed = at_class.layers;
  std::vector<std::pair<LD, std::string>> walls;
  for (const auto& [what, d] : at_class.walls) {
    walls.push_back({d.first, what});
    r.soft_wall |= d.second;
  }
  std::sort(walls.begin(), walls.end());
  for (std::size_t k = 0; k < walls.size() && k < 6; ++k) r.walls.push_back(walls[k].second);

  // A class's track width and via size are defaults, not limits: the proof holds only for the board's minimum track and via
  // (KiCad's DRC checks those; 10 um when the board sets none).
  const Coord min_w = std::min(nc.track_width, std::max<Coord>(m.rules.minimums.track_width, 10'000));
  const Coord min_drill = std::min(nc.via_drill, std::max<Coord>(m.rules.minimums.through_hole_diameter, 10'000));
  const Coord min_via = std::min(nc.via_diameter, std::max({m.rules.minimums.via_diameter, min_drill + 2 * m.rules.minimums.via_annular_width, min_drill}));
  const Walled at_min = min_w < nc.track_width || min_via < nc.via_diameter ? walled(min_w, min_via, min_drill, false) : at_class;
  std::string layers;
  for (const int L : r.layers_enclosed) layers += (layers.empty() ? "" : ", ") + m.b.copper_name(L);
  std::string wall_names;
  for (const auto& w : r.walls) wall_names += (wall_names.empty() ? "" : ", ") + w;
  if (at_min.out || at_min.via) {
    if (at_min.out) {  // the widest track that gets out: lo gets out, hi does not
      Coord lo = min_w, hi = nc.track_width;
      while (hi - lo > 5'000) {
        const Coord mid = (lo + hi) / 2;
        (walled(mid, nc.via_diameter, nc.via_drill, false).out ? lo : hi) = mid;
      }
      r.neck_width = lo;
    }
    r.small_via_fits = !at_min.out;
    r.text = who + " is walled in on " + layers + " for its class's " + mm(nc.track_width) + " track and " + mm(nc.via_diameter) + " via" +
             (wall_names.empty() ? "" : " (" + wall_names + ")") + ", but " +
             (at_min.out ? "a track of " + mm(r.neck_width) + " gets out (a neck-down)." : "a via of " + mm(min_via) + " fits beside it.");
    return r;
  }
  r.enclosed = true;
  r.via_in_pad_fits = at_min.via_in_pad;
  r.text = who + " cannot be connected as placed: on " + layers + " the fixed copper" + (wall_names.empty() ? "" : " (" + wall_names + ")") +
           " leaves no way out even for a track of the board's minimum width (" + mm(min_w) + ") with its clearances, and no via fits beside it.";
  if (r.via_in_pad_fits) r.text += " A via in the pad would fit (--via-in-pad).";
  else r.text += " Moving a part or changing the rules is needed.";
  return r;
}

PinEscape EscapeAnalyser::run(int pad) const {
  PinEscape r = impl_->analyse(pad, impl_->opt.soft);
  if (!r.enclosed || impl_->opt.soft) return r;
  // Walled in with zone fills, or net-less copper graphics on the pin, among the walls: the router's view, not the placement's.
  // Ask again without them.
  const PinEscape bare = impl_->analyse(pad, true);
  if (bare.enclosed) return r;
  const auto& pin = impl_->cm.items[static_cast<std::size_t>(impl_->item_of_pad[static_cast<std::size_t>(pad)])];
  bool zones = false, graphics = false;
  for (const auto& w : r.walls) zones |= w.find("zone fill") != std::string::npos;
  for (const auto& it : impl_->cm.items) graphics |= it.kind == drc::ItemKind::Graphic && it.net == 0 && Impl::touches(pin, it);
  zones |= !graphics;
  r.enclosed = false;
  r.soft_only = true;
  r.neck_width = 0;
  r.via_in_pad_fits = false;
  r.text = pad_label(impl_->b, pad) + " is walled in only by " +
           (zones && graphics ? std::string("zone fills and net-less copper graphics on the pin")
            : zones           ? std::string("zone fills of other nets")
                              : std::string("net-less copper graphics that cover the pin (a designer's pour)")) +
           ":" + (zones ? " --soft-zones routes through fills and refills the zones afterwards;" : "") +
           (graphics ? " --soft-copper-graphics gives such graphics the net of the pads they touch;" : "");
  r.text.back() = '.';
  return r;
}

}  // namespace tmk::analysis

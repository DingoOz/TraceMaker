#include "place/legality.hpp"

#include <algorithm>
#include <cmath>

namespace tmk::place {

namespace {
inline Box shift(const Box& b, Point d) { return b.empty() ? b : Box{b.x0 + d.x, b.y0 + d.y, b.x1 + d.x, b.y1 + d.y}; }

template <class F>
bool any_edge_t(const Shape& s, Point d, F&& f) {
  const std::size_t n = s.pts.size();
  if (n == 1) return f(s.pts[0] + d, s.pts[0] + d);
  for (std::size_t i = 0; i + 1 < n; ++i)
    if (f(s.pts[i] + d, s.pts[i + 1] + d)) return true;
  if (s.closed && n >= 3) return f(s.pts[n - 1] + d, s.pts[0] + d);
  return false;
}
}  // namespace

Placement Placement::initial(const Problem& p) {
  Placement pl;
  for (const auto& pt : p.parts) {
    pl.pos.push_back(pt.pos0);
    pl.rot.push_back(0);
  }
  return pl;
}

bool closer(const Shape& a, Point da, const Shape& b, Point db, Coord clearance) {
  if (a.pts.empty() || b.pts.empty()) return false;
  const Coord t = clearance + a.r + b.r;
  if (!shift(a.box, da).inflated(clearance).intersects(shift(b.box, db))) return false;
  if (t <= 0) return false;
  if (a.closed && geom::point_in_polygon(b.pts[0] + db - da, a.pts)) return true;
  if (b.closed && geom::point_in_polygon(a.pts[0] + da - db, b.pts)) return true;
  return any_edge_t(a, da, [&](Point p, Point q) {
    return any_edge_t(b, db, [&](Point u, Point v) { return geom::seg_seg_closer(p, q, u, v, t); });
  });
}

// ---------------------------------------------------------------------------------------------- Legality

Legality::Legality(const Problem& p) : p_(p) {
  const Box r = p.region.inflated(20'000'000);
  const double area = static_cast<double>(r.x1 - r.x0) * static_cast<double>(r.y1 - r.y0);
  cell_ = std::max<Coord>(2'000'000, static_cast<Coord>(std::sqrt(area / 16384.0)));
  ox_ = r.x0;
  oy_ = r.y0;
  nx_ = static_cast<int>((r.x1 - r.x0) / cell_) + 1;
  ny_ = static_cast<int>((r.y1 - r.y0) / cell_) + 1;
  cells_.assign(z(nx_ * ny_), {});
  pos_.assign(p.parts.size(), Point{});
  rot_.assign(p.parts.size(), 0);
  present_.assign(p.parts.size(), 0);
  stamp_.assign(std::max(p.parts.size(), std::size_t{1}), 0);
  for (const auto& e : p.edges)
    for (std::size_t k = 0; k + 1 < e.pts.size(); ++k) segs_.push_back(Shape::segment(e.pts[k], e.pts[k + 1], 0));
  seg_cells_.assign(z(nx_ * ny_), {});
  for (std::size_t i = 0; i < segs_.size(); ++i) {
    int x0, y0, x1, y1;
    cells_of(segs_[i].box, x0, y0, x1, y1);
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) seg_cells_[z(y * nx_ + x)].push_back(static_cast<int>(i));
  }
}

void Legality::cells_of(const Box& b, int& cx0, int& cy0, int& cx1, int& cy1) const {
  auto cx = [&](Coord v) { return static_cast<int>(std::clamp<Coord>((v - ox_) / cell_, 0, nx_ - 1)); };
  auto cy = [&](Coord v) { return static_cast<int>(std::clamp<Coord>((v - oy_) / cell_, 0, ny_ - 1)); };
  cx0 = cx(b.x0);
  cx1 = cx(b.x1);
  cy0 = cy(b.y0);
  cy1 = cy(b.y1);
}

bool Legality::inside_ok(int part, Point pos, int rot) const {
  const PartGeom& g = p_.parts[z(part)].geom[z(rot)];
  const Box eb = shift(g.edge_box, pos);
  const Box& rg = p_.region;
  if (eb.x0 < rg.x0 || eb.y0 < rg.y0 || eb.x1 > rg.x1 || eb.y1 > rg.y1) return false;
  auto near_edge = [&](const Shape& s, Coord c) {
    int x0, y0, x1, y1;
    cells_of(shift(s.box, pos).inflated(c), x0, y0, x1, y1);
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
        for (int si : seg_cells_[z(y * nx_ + x)])
          if (closer(segs_[z(si)], Point{}, s, pos, c)) return true;
    return false;
  };
  auto in_board = [&](Point q) {
    if (p_.outline.empty()) return true;
    if (!geom::point_in_polygon(q, p_.outline)) return false;
    for (const auto& c : p_.cutouts)
      if (geom::point_in_polygon(q, c)) return false;
    return true;
  };
  for (int s = 0; s < 2; ++s) {
    for (const Shape& cy : g.cy[z(s)]) {
      if (near_edge(cy, 1)) return false;
      if (!in_board(cy.pts[0] + pos)) return false;
      for (const auto& k : p_.keepouts)
        if (k.side[s] && closer(k.poly, Point{}, cy, pos, 1)) return false;
    }
  }
  const Coord ec = std::max<Coord>(p_.edge_clearance, 1);
  for (const auto& pd : g.pads) {
    if (near_edge(pd, ec)) return false;
    if (!in_board(pd.pts[0] + pos)) return false;
  }
  return true;
}

bool Legality::pair_conflict(int a, Point pa, int ra, int b, Point pb, int rb) const {
  const PartGeom& ga = p_.parts[z(a)].geom[z(ra)];
  const PartGeom& gb = p_.parts[z(b)].geom[z(rb)];
  const Coord reach = std::max({p_.clearance, kThroughMargin, kThroughThrough});
  if (!shift(ga.body, pa).inflated(reach).intersects(shift(gb.body, pb))) return false;
  const Coord c = std::max<Coord>(p_.clearance, 1);
  for (int s = 0; s < 2; ++s)
    for (const Shape& u : ga.cy[z(s)])
      for (const Shape& v : gb.cy[z(s)])
        if (closer(u, pa, v, pb, c)) return true;
  for (const auto& t : ga.through) {
    for (int s = 0; s < 2; ++s)
      for (const Shape& v : gb.cy[z(s)])
        if (closer(t, pa, v, pb, kThroughMargin)) return true;
    for (const auto& u : gb.through)
      if (closer(t, pa, u, pb, kThroughThrough)) return true;
  }
  for (const auto& t : gb.through)
    for (int s = 0; s < 2; ++s)
      for (const Shape& v : ga.cy[z(s)])
        if (closer(t, pb, v, pa, kThroughMargin)) return true;
  return false;
}

void Legality::clear() {
  for (auto& c : cells_) c.clear();
  std::fill(present_.begin(), present_.end(), 0);
}

void Legality::reset(const Placement& pl) {
  clear();
  for (std::size_t i = 0; i < p_.parts.size(); ++i) insert(static_cast<int>(i), pl.pos[i], pl.rot[i]);
}

void Legality::insert(int part, Point pos, int rot) {
  if (present_[z(part)]) remove(part);
  pos_[z(part)] = pos;
  rot_[z(part)] = static_cast<std::uint8_t>(rot);
  present_[z(part)] = 1;
  int x0, y0, x1, y1;
  cells_of(shift(p_.parts[z(part)].geom[z(rot)].body, pos), x0, y0, x1, y1);
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) cells_[z(y * nx_ + x)].push_back(part);
}

void Legality::remove(int part) {
  if (!present_[z(part)]) return;
  present_[z(part)] = 0;
  int x0, y0, x1, y1;
  cells_of(shift(p_.parts[z(part)].geom[rot_[z(part)]].body, pos_[z(part)]), x0, y0, x1, y1);
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      auto& c = cells_[z(y * nx_ + x)];
      c.erase(std::find(c.begin(), c.end(), part));
    }
}

void Legality::neighbours(const Box& box, std::vector<int>& out) const {
  if (++epoch_ == 0) {
    std::fill(stamp_.begin(), stamp_.end(), 0);
    epoch_ = 1;
  }
  int x0, y0, x1, y1;
  cells_of(box, x0, y0, x1, y1);
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x)
      for (int q : cells_[z(y * nx_ + x)]) {
        if (stamp_[z(q)] == epoch_) continue;
        stamp_[z(q)] = epoch_;
        out.push_back(q);
      }
}

int Legality::find_conflict(int part, Point pos, int rot, int skip1, int skip2) const {
  const Coord reach = std::max({p_.clearance, kThroughMargin, kThroughThrough});
  if (++epoch_ == 0) {
    std::fill(stamp_.begin(), stamp_.end(), 0);
    epoch_ = 1;
  }
  int x0, y0, x1, y1;
  cells_of(shift(p_.parts[z(part)].geom[z(rot)].body, pos).inflated(reach), x0, y0, x1, y1);
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x)
      for (int q : cells_[z(y * nx_ + x)]) {
        if (q == part || q == skip1 || q == skip2 || stamp_[z(q)] == epoch_) continue;
        stamp_[z(q)] = epoch_;
        if (pair_conflict(part, pos, rot, q, pos_[z(q)], rot_[z(q)])) return q;
      }
  return -1;
}

void Legality::conflicts(int part, Point pos, int rot, std::vector<int>& out) const {
  const Coord reach = std::max({p_.clearance, kThroughMargin, kThroughThrough});
  std::vector<int> nb;
  neighbours(shift(p_.parts[z(part)].geom[z(rot)].body, pos).inflated(reach), nb);
  for (int q : nb)
    if (q != part && pair_conflict(part, pos, rot, q, pos_[z(q)], rot_[z(q)])) out.push_back(q);
  std::sort(out.begin(), out.end());
}

// ------------------------------------------------------------------------------------------------ Raster

Raster::Raster(const Problem& p, Coord cell) : p_(p), h_(cell) {
  const Box r = p.region.inflated(1'000'000);
  ox_ = r.x0;
  oy_ = r.y0;
  nx_ = static_cast<int>((r.x1 - r.x0) / h_) + 1;
  ny_ = static_cast<int>((r.y1 - r.y0) / h_) + 1;
  const std::size_t n = z(nx_) * z(ny_);
  for (int s = 0; s < 2; ++s) {
    blocked_[s].assign(n, 1);
    occ_[s].assign(n, 0);
  }
  // Inside-board cells: centre inside (even-odd over outline and cut-outs) and no edge through the cell.
  std::vector<std::uint8_t> inside(n, 0);
  if (p.outline.empty()) {
    for (int y = 0; y < ny_; ++y)
      for (int x = 0; x < nx_; ++x) {
        const Coord cx0 = ox_ + x * h_, cy0 = oy_ + y * h_;
        if (cx0 >= p.region.x0 && cy0 >= p.region.y0 && cx0 + h_ <= p.region.x1 && cy0 + h_ <= p.region.y1) inside[z(y * nx_ + x)] = 1;
      }
  } else {
    std::vector<const std::vector<Point>*> loops{&p.outline};
    for (const auto& c : p.cutouts) loops.push_back(&c);
    std::vector<double> xs;
    for (int y = 0; y < ny_; ++y) {
      const double yc = static_cast<double>(oy_) + (y + 0.5) * static_cast<double>(h_);
      xs.clear();
      for (const auto* l : loops)
        for (std::size_t i = 0, j = l->size() - 1; i < l->size(); j = i++) {
          const Point a = (*l)[j], b = (*l)[i];
          const double ay = static_cast<double>(a.y), by = static_cast<double>(b.y);
          if ((ay > yc) != (by > yc))
            xs.push_back(static_cast<double>(a.x) + (yc - ay) * static_cast<double>(b.x - a.x) / (by - ay));
        }
      std::sort(xs.begin(), xs.end());
      for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
        const double x0 = (xs[k] - static_cast<double>(ox_)) / static_cast<double>(h_) - 0.5;
        const double x1 = (xs[k + 1] - static_cast<double>(ox_)) / static_cast<double>(h_) - 0.5;
        for (int x = std::max(0, static_cast<int>(std::ceil(x0))); x <= std::min(nx_ - 1, static_cast<int>(std::floor(x1))); ++x)
          inside[z(y * nx_ + x)] = 1;
      }
    }
  }
  // Cells an edge passes through (samples every h/2, 3x3 neighbourhood: conservative).
  auto mark_path = [&](const std::vector<Point>& pts, bool closed, std::vector<std::uint8_t>& m, std::uint8_t v) {
    const std::size_t k = pts.size();
    for (std::size_t i = 0; i + (closed ? 0 : 1) < k; ++i) {
      const Point a = pts[i], b = pts[(i + 1) % k];
      const double len = std::hypot(static_cast<double>(b.x - a.x), static_cast<double>(b.y - a.y));
      const int steps = std::max(1, static_cast<int>(std::ceil(len / (static_cast<double>(h_) / 2))));
      for (int t = 0; t <= steps; ++t) {
        const double f = static_cast<double>(t) / steps;
        const double px = static_cast<double>(a.x) + f * static_cast<double>(b.x - a.x);
        const double py = static_cast<double>(a.y) + f * static_cast<double>(b.y - a.y);
        const int cx = static_cast<int>(std::floor((px - static_cast<double>(ox_)) / static_cast<double>(h_)));
        const int cy = static_cast<int>(std::floor((py - static_cast<double>(oy_)) / static_cast<double>(h_)));
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            const int x = cx + dx, y = cy + dy;
            if (x >= 0 && y >= 0 && x < nx_ && y < ny_) m[z(y * nx_ + x)] = v;
          }
      }
    }
  };
  for (const auto& e : p.edges) mark_path(e.pts, false, inside, 0);
  for (int s = 0; s < 2; ++s)
    for (std::size_t i = 0; i < n; ++i) blocked_[s][i] = inside[i] ? 0 : 1;
  // Keepouts: cells whose centre is inside, or that an edge of the keepout crosses.
  for (const auto& k : p.keepouts) {
    std::vector<std::uint8_t> m(n, 0);
    const Box kb = k.poly.box;
    for (int y = 0; y < ny_; ++y)
      for (int x = 0; x < nx_; ++x) {
        const Point c{ox_ + x * h_ + h_ / 2, oy_ + y * h_ + h_ / 2};
        if (c.x < kb.x0 || c.x > kb.x1 || c.y < kb.y0 || c.y > kb.y1) continue;
        if (geom::point_in_polygon(c, k.poly.pts)) m[z(y * nx_ + x)] = 1;
      }
    mark_path(k.poly.pts, true, m, 1);
    for (int s = 0; s < 2; ++s)
      if (k.side[s])
        for (std::size_t i = 0; i < n; ++i) blocked_[s][i] = static_cast<std::uint16_t>(blocked_[s][i] | m[i]);
  }
}

bool Raster::range(const Box& b, int& x0, int& y0, int& x1, int& y1) const {
  if (b.x0 < ox_ || b.y0 < oy_) return false;
  x0 = static_cast<int>((b.x0 - ox_) / h_);
  y0 = static_cast<int>((b.y0 - oy_) / h_);
  x1 = static_cast<int>((b.x1 - ox_) / h_);
  y1 = static_cast<int>((b.y1 - oy_) / h_);
  return x1 < nx_ && y1 < ny_;
}

bool Raster::any(const std::vector<std::uint16_t>& g, const Box& b) const {
  int x0, y0, x1, y1;
  if (!range(b, x0, y0, x1, y1)) return true;
  for (int y = y0; y <= y1; ++y) {
    const std::uint16_t* row = g.data() + z(y * nx_);
    for (int x = x0; x <= x1; ++x)
      if (row[x]) return true;
  }
  return false;
}

void Raster::bump(std::vector<std::uint16_t>& g, const Box& b, int delta) {
  auto cl = [](Coord v, int n) { return static_cast<int>(std::clamp<Coord>(v, 0, n - 1)); };
  const int x0 = cl((b.x0 - ox_) / h_, nx_), x1 = cl((b.x1 - ox_) / h_, nx_);
  const int y0 = cl((b.y0 - oy_) / h_, ny_), y1 = cl((b.y1 - oy_) / h_, ny_);
  for (int y = y0; y <= y1; ++y)
    for (int x = x0; x <= x1; ++x) {
      auto& c = g[z(y * nx_ + x)];
      c = static_cast<std::uint16_t>(c + delta);
    }
}

void Raster::add(int part, Point pos, int rot, int delta) {
  const PartGeom& g = p_.parts[z(part)].geom[z(rot)];
  for (int s = 0; s < 2; ++s)
    for (const Shape& cy : g.cy[z(s)]) bump(occ_[s], shift(cy.box, pos), delta);
  for (const auto& t : g.through)
    for (int s = 0; s < 2; ++s) bump(occ_[s], shift(t.box, pos), delta);
}

bool Raster::free(int part, Point pos, int rot) const {
  const PartGeom& g = p_.parts[z(part)].geom[z(rot)];
  const Coord infl = std::max(p_.clearance, kThroughMargin) + 1;
  const Coord tinfl = std::max(kThroughThrough, kThroughMargin) + 1;
  for (int s = 0; s < 2; ++s) {
    if (g.cy[z(s)].empty()) continue;
    if (any(blocked_[s], shift(g.edge_box, pos))) return false;
    for (const Shape& cy : g.cy[z(s)])
      if (any(occ_[s], shift(cy.box, pos).inflated(infl))) return false;
  }
  for (const auto& t : g.through)
    for (int s = 0; s < 2; ++s)
      if (any(occ_[s], shift(t.box, pos).inflated(tinfl))) return false;
  return true;
}

double Raster::inside_fraction(int side) const {
  std::size_t k = 0;
  for (auto v : blocked_[side]) k += v == 0 ? 1 : 0;
  return blocked_[side].empty() ? 0.0 : static_cast<double>(k) / static_cast<double>(blocked_[side].size());
}

// ------------------------------------------------------------------------------------------- check_all

Violations check_all(const Problem& p, const Placement& pl) {
  Violations v;
  Legality L(p);
  L.reset(pl);
  std::vector<int> nb;
  const Coord reach = std::max({p.clearance, kThroughMargin, kThroughThrough});
  for (std::size_t a = 0; a < p.parts.size(); ++a) {
    const int ia = static_cast<int>(a);
    if (!L.inside_ok(ia, pl.pos[a], pl.rot[a])) {
      if (p.parts[a].movable) {
        ++v.outside;
        v.outside_parts.push_back(ia);
      } else {
        ++v.fixed_outside;
      }
    }
    nb.clear();
    L.neighbours(shift(p.parts[a].geom[pl.rot[a]].body, pl.pos[a]).inflated(reach), nb);
    std::sort(nb.begin(), nb.end());
    for (int b : nb) {
      if (b <= ia) continue;
      if (!L.pair_conflict(ia, pl.pos[a], pl.rot[a], b, pl.pos[z(b)], pl.rot[z(b)])) continue;
      if (p.parts[a].movable || p.parts[z(b)].movable) {
        ++v.overlaps;
        v.pairs.emplace_back(ia, b);
      } else {
        ++v.fixed_overlaps;
      }
    }
  }
  return v;
}

}  // namespace tmk::place

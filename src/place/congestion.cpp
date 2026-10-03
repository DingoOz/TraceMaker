#include "place/congestion.hpp"

#include <climits>
#include <cmath>

namespace tmk::place {

int CongestionMap::bin(Point q) const {
  const int x = static_cast<int>(std::clamp<Coord>((q.x - ox) / cell, 0, nx - 1));
  const int y = static_cast<int>(std::clamp<Coord>((q.y - oy) / cell, 0, ny - 1));
  return y * nx + x;
}

CongestionMap make_congestion_map(const Problem& p, const CongestionOptions& o) {
  CongestionMap m;
  const Box r = p.region.empty() ? Box{0, 0, 10'000'000, 10'000'000} : p.region;
  const Coord w = std::max<Coord>(r.x1 - r.x0, 1), h = std::max<Coord>(r.y1 - r.y0, 1);
  m.cell = std::max<Coord>(1'000'000, (std::max(w, h) + o.max_bins - 1) / std::max(1, o.max_bins));
  m.ox = r.x0;
  m.oy = r.y0;
  m.nx = static_cast<int>((w + m.cell - 1) / m.cell);
  m.ny = static_cast<int>((h + m.cell - 1) / m.cell);
  m.nx = std::max(1, m.nx);
  m.ny = std::max(1, m.ny);
  const double cellmm = static_cast<double>(m.cell) / 1e6;
  // Ideal track length per bin and layer: (cell / pitch) tracks of length cell.
  const double per_layer_mm = cellmm * cellmm / std::max(0.05, o.pitch_mm);
  const double full_nm = per_layer_mm * 1e6 * std::max(1, p.copper_layers) * o.usable;
  m.pin_demand = static_cast<Coord>(std::llround(o.pin_tracks * cellmm * 1e6));
  m.cap.assign(static_cast<std::size_t>(m.nx * m.ny), 0);
  // Fraction of the bin inside the outline (4 × 4 samples; the whole bin when there is no outline).
  constexpr int kS = 4;
  for (int y = 0; y < m.ny; ++y)
    for (int x = 0; x < m.nx; ++x) {
      int in = 0;
      for (int sy = 0; sy < kS; ++sy)
        for (int sx = 0; sx < kS; ++sx) {
          const Point q{m.ox + x * m.cell + (2 * sx + 1) * m.cell / (2 * kS), m.oy + y * m.cell + (2 * sy + 1) * m.cell / (2 * kS)};
          bool inside = p.outline.size() < 3 || geom::point_in_polygon(q, p.outline);
          for (const auto& c : p.cutouts)
            if (inside && c.size() >= 3 && geom::point_in_polygon(q, c)) inside = false;
          in += inside ? 1 : 0;
        }
      m.cap[static_cast<std::size_t>(y * m.nx + x)] = static_cast<std::int64_t>(std::llround(full_nm * in / (kS * kS)));
    }
  return m;
}

bool rudy_net(const Problem& p, int net) {
  const PNet& n = p.nets[z(net)];
  return n.signal && n.pins.size() >= 2;
}

Box net_box(const Problem& p, const Placement& pl, int net) {
  Box b;
  for (int pi : p.nets[z(net)].pins) b.add(pl.pin(p, pi));
  return b;
}

std::vector<std::int64_t> rudy_demand(const Problem& p, const Placement& pl, const CongestionMap& m) {
  std::vector<std::int64_t> d(m.size(), 0);
  auto none = [](std::size_t) {};
  for (std::size_t n = 0; n < p.nets.size(); ++n)
    if (rudy_net(p, static_cast<int>(n))) add_box_demand(m, net_box(p, pl, static_cast<int>(n)), +1, d, none);
  for (const Pin& q : p.pins)
    if (!p.nets[z(q.net)].affinity) d[z(m.bin(pl.pos[z(q.part)] + q.off[pl.rot[z(q.part)]]))] += m.pin_demand;
  return d;
}

std::int64_t total_overflow(const CongestionMap& m, const std::vector<std::int64_t>& demand) {
  std::int64_t s = 0;
  for (std::size_t k = 0; k < m.size(); ++k) s += bin_overflow(demand[k], m.cap[k]);
  return s;
}

std::int64_t rudy_overflow(const Problem& p, const Placement& pl, const CongestionMap& m) {
  return total_overflow(m, rudy_demand(p, pl, m));
}

void scale_bins(CongestionMap& m, const std::vector<Point>& pts, Coord radius, double factor) {
  std::vector<std::uint8_t> hit(m.size(), 0);
  for (const Point& q : pts) {
    const int x0 = static_cast<int>(std::clamp<Coord>((q.x - radius - m.ox) / m.cell, 0, m.nx - 1));
    const int x1 = static_cast<int>(std::clamp<Coord>((q.x + radius - m.ox) / m.cell, 0, m.nx - 1));
    const int y0 = static_cast<int>(std::clamp<Coord>((q.y - radius - m.oy) / m.cell, 0, m.ny - 1));
    const int y1 = static_cast<int>(std::clamp<Coord>((q.y + radius - m.oy) / m.cell, 0, m.ny - 1));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) hit[static_cast<std::size_t>(y * m.nx + x)] = 1;
  }
  for (std::size_t k = 0; k < m.size(); ++k)
    if (hit[k]) m.cap[k] = static_cast<std::int64_t>(std::llround(static_cast<double>(m.cap[k]) * factor));
}

}  // namespace tmk::place

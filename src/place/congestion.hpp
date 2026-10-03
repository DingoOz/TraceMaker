#pragma once
// Routability estimate for placement (design doc 04 §3 B `R(x)`, E `β·RUDY overflow`, G).
//
// RUDY (Rectangular Uniform wire DensitY; Spindler & Johannes, "Fast and accurate routing demand estimation
// for efficient routability-driven placement", DATE 2007): each signal net's half-perimeter wirelength is spread
// uniformly over its pin bounding box, so the demand of a bin is Σ_nets HPWL · area(box ∩ bin) / area(box).
// Every pin also consumes a fixed amount of track length in its bin (pin density). Capacity per bin is the
// track length the copper layers offer there (layers × bin² / pitch × usable fraction × fraction inside the
// outline). The cost is the total overflow Σ max(0, demand − capacity), all in integer nm so incremental and
// from-scratch evaluation agree exactly (CLAUDE.md rules 2 and 3).
//
// Power nets (PNet::signal false) are left out (they are expected on pours or planes; the same reason they get a low HPWL
// weight). The router-in-the-loop (G) lowers the capacity of bins where the router failed (`scale_bins`), which
// is the PCB analogue of RePlAce's cell inflation in overflowed tiles.
#include <algorithm>
#include <cstdint>
#include <vector>

#include "place/legality.hpp"

namespace tmk::place {

struct CongestionOptions {
  double pitch_mm = 0.5;    // track pitch (width + clearance) used for the capacity
  double usable = 0.6;      // fraction of the ideal track length that is routable (pads, vias, detours)
  double pin_tracks = 0.5;  // a pin consumes this many tracks across its bin
  int max_bins = 32;        // bins along the longer side of the board
};

struct CongestionMap {
  Coord ox = 0, oy = 0, cell = 1'000'000;
  int nx = 1, ny = 1;
  std::vector<std::int64_t> cap;  // track-length capacity per bin, nm
  Coord pin_demand = 0;           // track length consumed by one pin, nm
  int bin(Point q) const;         // bin containing q (clamped to the grid)
  std::size_t size() const { return cap.size(); }
};

CongestionMap make_congestion_map(const Problem& p, const CongestionOptions& o = {});

// True if the net contributes RUDY demand (signal nets with at least two pins).
bool rudy_net(const Problem& p, int net);

// Pin bounding box of one net.
Box net_box(const Problem& p, const Placement& pl, int net);

// Adds `sign` × the RUDY demand of a net with pin bounding box `b` to `demand`. `touch(bin)` is called before a
// bin is changed (for incremental bookkeeping); pass a no-op lambda when not needed.
template <class Touch>
void add_box_demand(const CongestionMap& m, const Box& b, int sign, std::vector<std::int64_t>& demand, Touch&& touch);

// From-scratch demand of a placement (reference path for the annealer's incremental bookkeeping).
std::vector<std::int64_t> rudy_demand(const Problem& p, const Placement& pl, const CongestionMap& m);
inline std::int64_t bin_overflow(std::int64_t demand, std::int64_t cap) { return demand > cap ? demand - cap : 0; }
std::int64_t total_overflow(const CongestionMap& m, const std::vector<std::int64_t>& demand);
std::int64_t rudy_overflow(const Problem& p, const Placement& pl, const CongestionMap& m);

// Multiplies the capacity of the bins within `radius` of each point by `factor` (once per bin per call).
void scale_bins(CongestionMap& m, const std::vector<Point>& pts, Coord radius, double factor);

// ---- implementation of the template -------------------------------------------------------------------------

namespace detail {
// RUDY box: the pin box widened to at least one bin per axis, so aligned pins do not give infinite density.
inline Box rudy_box(const CongestionMap& m, const Box& b) {
  Box r = b;
  if (r.x1 - r.x0 < m.cell) {
    const Coord c = (r.x0 + r.x1) / 2;
    r.x0 = c - m.cell / 2;
    r.x1 = r.x0 + m.cell;
  }
  if (r.y1 - r.y0 < m.cell) {
    const Coord c = (r.y0 + r.y1) / 2;
    r.y0 = c - m.cell / 2;
    r.y1 = r.y0 + m.cell;
  }
  return r;
}
}  // namespace detail

template <class Touch>
void add_box_demand(const CongestionMap& m, const Box& b, int sign, std::vector<std::int64_t>& demand, Touch&& touch) {
  if (b.empty()) return;
  const Coord hp = (b.x1 - b.x0) + (b.y1 - b.y0);
  if (hp <= 0) return;
  const Box r = detail::rudy_box(m, b);
  const geom::i128 area = static_cast<geom::i128>(r.x1 - r.x0) * (r.y1 - r.y0);
  auto cx = [&](Coord v) { return static_cast<int>(std::clamp<Coord>((v - m.ox) / m.cell, 0, m.nx - 1)); };
  auto cy = [&](Coord v) { return static_cast<int>(std::clamp<Coord>((v - m.oy) / m.cell, 0, m.ny - 1)); };
  const int x0 = cx(r.x0), x1 = cx(r.x1 - 1), y0 = cy(r.y0), y1 = cy(r.y1 - 1);
  for (int y = y0; y <= y1; ++y) {
    // Bins at the grid border absorb everything beyond it.
    const Coord by0 = y == 0 ? r.y0 : std::max(r.y0, m.oy + y * m.cell);
    const Coord by1 = y == m.ny - 1 ? r.y1 : std::min(r.y1, m.oy + (y + 1) * m.cell);
    if (by1 <= by0) continue;
    for (int x = x0; x <= x1; ++x) {
      const Coord bx0 = x == 0 ? r.x0 : std::max(r.x0, m.ox + x * m.cell);
      const Coord bx1 = x == m.nx - 1 ? r.x1 : std::min(r.x1, m.ox + (x + 1) * m.cell);
      if (bx1 <= bx0) continue;
      const geom::i128 ov = static_cast<geom::i128>(bx1 - bx0) * (by1 - by0);
      const auto d = static_cast<std::int64_t>(static_cast<geom::i128>(hp) * ov / area);
      if (d == 0) continue;
      const auto k = static_cast<std::size_t>(y * m.nx + x);
      touch(k);
      demand[k] += sign * d;
    }
  }
}

}  // namespace tmk::place

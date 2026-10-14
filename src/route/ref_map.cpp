// SPDX-License-Identifier: GPL-3.0-or-later
// Reference-plane voids on the routing lattice (issue #24, doc 05 §44): see ref_map.hpp.
#include "route/ref_map.hpp"

#include <algorithm>
#include <cmath>

namespace tmk::route {

namespace {
using geom::Rings;
}

// Scanline fill (even-odd over properly nested rings equals the non-zero rule of a united region): for each lattice row the x of every edge crossing
// it, sorted, and the cells between the 1st and 2nd, the 3rd and 4th ... are inside. An edge crosses the row y when it has one end above and one at
// or below it (half-open, so a vertex on the row is counted once).
std::vector<std::uint8_t> rasterise(const Rings& region, const geom::Box& lat, Coord pitch, int nx, int ny) {
  std::vector<std::uint8_t> out(static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny), 0);
  if (nx <= 0 || ny <= 0 || pitch <= 0) return out;
  std::vector<std::vector<double>> hits(static_cast<std::size_t>(ny));
  for (const auto& ring : region) {
    const std::size_t n = ring.size();
    for (std::size_t i = 0; i < n; ++i) {
      const geom::Point a = ring[i], b = ring[(i + 1) % n];
      if (a.y == b.y) continue;
      const geom::Point lo = a.y < b.y ? a : b, hi = a.y < b.y ? b : a;
      // Rows whose y satisfies lo.y <= y < hi.y.
      const double r0 = std::ceil(static_cast<double>(lo.y - lat.y0) / static_cast<double>(pitch));
      const double r1 = std::ceil(static_cast<double>(hi.y - lat.y0) / static_cast<double>(pitch)) - 1;
      const int y0 = static_cast<int>(std::max(0.0, r0)), y1 = static_cast<int>(std::min(static_cast<double>(ny - 1), r1));
      for (int y = y0; y <= y1; ++y) {
        const Coord yc = lat.y0 + static_cast<Coord>(y) * pitch;
        const double t = static_cast<double>(yc - lo.y) / static_cast<double>(hi.y - lo.y);
        hits[static_cast<std::size_t>(y)].push_back(static_cast<double>(lo.x) + t * static_cast<double>(hi.x - lo.x));
      }
    }
  }
  for (int y = 0; y < ny; ++y) {
    auto& xs = hits[static_cast<std::size_t>(y)];
    std::sort(xs.begin(), xs.end());
    std::uint8_t* row = &out[static_cast<std::size_t>(y) * static_cast<std::size_t>(nx)];
    for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
      // Cells x with xs[k] <= lat.x0 + x * pitch < xs[k + 1].
      const double c0 = std::ceil((xs[k] - static_cast<double>(lat.x0)) / static_cast<double>(pitch));
      const double c1 = std::ceil((xs[k + 1] - static_cast<double>(lat.x0)) / static_cast<double>(pitch)) - 1;
      const int x0 = static_cast<int>(std::max(0.0, c0)), x1 = static_cast<int>(std::min(static_cast<double>(nx - 1), c1));
      if (x0 <= x1) std::fill(row + x0, row + x1 + 1, static_cast<std::uint8_t>(1));
    }
  }
  return out;
}

ReferenceMap build_reference_map(const model::Board& b, const geom::Box& lat, Coord pitch, int nx, int ny, double min_coverage) {
  ReferenceMap m;
  const int nl = b.copper_count();
  m.reference_layer.assign(static_cast<std::size_t>(std::max(nl, 0)), -1);
  m.coverage.assign(static_cast<std::size_t>(std::max(nl, 0)), 0.0);
  if (nl < 2) return m;
  std::vector<Rings> per(static_cast<std::size_t>(nl));
  for (const auto& z : b.zones) {
    if (z.rule_area) continue;
    for (const auto& [layer, pts] : z.fills)
      if (layer >= 0 && layer < nl && pts.size() >= 3) per[static_cast<std::size_t>(layer)].push_back(pts);
  }
  const geom::Box bb = lat;
  const double area = static_cast<double>(bb.x1 - bb.x0) * static_cast<double>(bb.y1 - bb.y0);
  std::vector<Rings> united(static_cast<std::size_t>(nl));
  for (int l = 0; l < nl; ++l) {
    if (per[static_cast<std::size_t>(l)].empty()) continue;
    united[static_cast<std::size_t>(l)] = geom::unite(per[static_cast<std::size_t>(l)]);
    m.coverage[static_cast<std::size_t>(l)] = area > 0 ? geom::area(united[static_cast<std::size_t>(l)]) / area : 0.0;
  }
  // The reference of a layer: the adjacent layer with the larger coverage, if it is a plane at all (ties go to the lower index).
  std::vector<std::vector<std::uint8_t>> covered(static_cast<std::size_t>(nl));  // rasterised planes, made on demand
  auto plane = [&](int l) -> const std::vector<std::uint8_t>& {
    auto& c = covered[static_cast<std::size_t>(l)];
    if (c.empty()) c = rasterise(united[static_cast<std::size_t>(l)], lat, pitch, nx, ny);
    return c;
  };
  const std::size_t layer_cells = static_cast<std::size_t>(nx) * static_cast<std::size_t>(ny);
  for (int l = 0; l < nl; ++l) {
    int best = -1;
    for (const int r : {l - 1, l + 1}) {
      if (r < 0 || r >= nl || m.coverage[static_cast<std::size_t>(r)] < min_coverage) continue;
      if (best < 0 || m.coverage[static_cast<std::size_t>(r)] > m.coverage[static_cast<std::size_t>(best)]) best = r;
    }
    if (best < 0) continue;
    m.reference_layer[static_cast<std::size_t>(l)] = best;
    if (m.over_void.empty()) m.over_void.assign(static_cast<std::size_t>(nl) * layer_cells, 0);
    const auto& copper = plane(best);
    std::uint8_t* dst = &m.over_void[static_cast<std::size_t>(l) * layer_cells];
    for (std::size_t i = 0; i < layer_cells; ++i) dst[i] = copper[i] ? 0 : 1;
  }
  // A plane layer with no plane next to it (the outer plane of a two-layer board): a track in one of its own gaps has no copper beside it, nor under
  // it, so its gaps count as voids too. Without this a net would hop onto the plane layer to cross a split on the cheap.
  for (int l = 0; l < nl; ++l) {
    if (m.reference_layer[static_cast<std::size_t>(l)] >= 0 || m.coverage[static_cast<std::size_t>(l)] < min_coverage) continue;
    if (m.over_void.empty()) m.over_void.assign(static_cast<std::size_t>(nl) * layer_cells, 0);
    const auto& copper = plane(l);
    std::uint8_t* dst = &m.over_void[static_cast<std::size_t>(l) * layer_cells];
    for (std::size_t i = 0; i < layer_cells; ++i) dst[i] = copper[i] ? 0 : 1;
  }
  return m;
}

}  // namespace tmk::route

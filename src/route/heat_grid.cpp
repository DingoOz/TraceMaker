// SPDX-License-Identifier: GPL-3.0-or-later
#include "route/heat_grid.hpp"

#include <algorithm>
#include <cmath>

namespace tmk::route {

void HeatGrid::init(const geom::Box& area, Coord min_cell) {
  if (area.empty()) {
    w_ = h_ = 0;
    v_.clear();
    return;
  }
  const Coord span = std::max(area.x1 - area.x0, area.y1 - area.y0) + 1;
  Coord c = std::max<Coord>({min_cell, 1, (span + kMaxCells - 1) / kMaxCells});
  c = (c + 999) / 1000 * 1000;  // whole micrometres: readable in the viewer and in recordings
  cell_ = c;
  x0_ = area.x0;
  y0_ = area.y0;
  w_ = static_cast<int>((area.x1 - area.x0) / c) + 1;
  h_ = static_cast<int>((area.y1 - area.y0) / c) + 1;
  v_.assign(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), 0);
}

void HeatGrid::clear() { std::fill(v_.begin(), v_.end(), 0); }

int HeatGrid::index(geom::Point p) const {
  if (w_ == 0 || p.x < x0_ || p.y < y0_) return -1;
  const Coord ix = (p.x - x0_) / cell_, iy = (p.y - y0_) / cell_;
  if (ix >= w_ || iy >= h_) return -1;
  return static_cast<int>(iy * w_ + ix);
}

void HeatGrid::add(geom::Point p, std::uint64_t w) {
  if (const int i = index(p); i >= 0) v_[static_cast<std::size_t>(i)] += w;
}

void HeatGrid::raise(geom::Point p, std::uint64_t v) {
  if (const int i = index(p); i >= 0) v_[static_cast<std::size_t>(i)] = std::max(v_[static_cast<std::size_t>(i)], v);
}

std::uint64_t HeatGrid::max() const { return v_.empty() ? 0 : *std::max_element(v_.begin(), v_.end()); }

std::vector<std::uint8_t> HeatGrid::bytes() const {
  std::vector<std::uint8_t> out(v_.size(), 0);
  const std::uint64_t m = max();
  if (m == 0) return out;
  const double inv = 1.0 / static_cast<double>(m);
  for (std::size_t i = 0; i < v_.size(); ++i) {
    if (v_[i] == 0) continue;
    const long q = std::lround(255.0 * std::sqrt(static_cast<double>(v_[i]) * inv));
    out[i] = static_cast<std::uint8_t>(std::clamp<long>(q, 1, 255));
  }
  return out;
}

std::string HeatGrid::message(std::string_view name, int layer) const {
  // Built by hand like the router's other messages (tm::route does not link the JSON library).
  std::string m = "{\"type\":\"heatmap\",\"name\":\"";
  m += name;
  m += "\",\"x0\":" + std::to_string(x0_) + ",\"y0\":" + std::to_string(y0_) + ",\"cell\":" + std::to_string(cell_) +
       ",\"w\":" + std::to_string(w_) + ",\"h\":" + std::to_string(h_) + ",\"layer\":" + std::to_string(layer) +
       ",\"max\":" + std::to_string(max()) + ",\"scale\":\"sqrt\",\"data\":[";
  const auto b = bytes();
  m.reserve(m.size() + b.size() * 4 + 2);
  for (std::size_t i = 0; i < b.size(); ++i) {
    if (i) m += ',';
    m += std::to_string(b[i]);
  }
  return m + "]}";
}

}  // namespace tmk::route

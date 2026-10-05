// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Coarse accumulation grid behind the viewer's heatmap overlays (design doc 09 §3 "Heatmap overlays", protocol
// doc 13 `heatmap`). Visualisation only: the router writes into it while a sink is attached and never reads it
// back, so routing results cannot depend on it (rule 2; checked by tests/integration/record_replay.py, which
// routes with and without recording and compares the boards byte for byte).
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "geom/point.hpp"

namespace tmk::route {

class HeatGrid {
 public:
  // At most this many cells along the longer side: a 128 x 128 overlay is ~16k values per message, small
  // enough to resend every few hundred thousand expansions without slowing the router or bloating recordings.
  static constexpr int kMaxCells = 128;

  // Covers `area` with square cells of at least `min_cell` nm (cell size rounded up to whole micrometres).
  void init(const geom::Box& area, Coord min_cell);
  bool ready() const { return w_ > 0; }
  void clear();
  void add(geom::Point p, std::uint64_t w);    // accumulate (search effort)
  void raise(geom::Point p, std::uint64_t v);  // keep the maximum (history cost): independent of visiting order
  std::uint64_t max() const;
  int width() const { return w_; }
  int height() const { return h_; }
  Coord cell() const { return cell_; }
  // One byte per cell, row-major: round(255 * sqrt(v / max)), at least 1 for any non-zero cell so faint activity
  // stays visible. The square root keeps heavy-tailed counts (a few hot spots) from washing out everything else.
  std::vector<std::uint8_t> bytes() const;
  // The `heatmap` protocol message (layer -1 = all layers) with "scale":"sqrt" so a legend can invert it.
  std::string message(std::string_view name, int layer) const;

 private:
  int index(geom::Point p) const;
  Coord x0_ = 0, y0_ = 0, cell_ = 1;
  int w_ = 0, h_ = 0;
  std::vector<std::uint64_t> v_;
};

}  // namespace tmk::route

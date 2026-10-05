// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Uniform spatial hash grid over a bounding box (design doc 03 §2): O(1) insert, box queries return each
// id at most once per query via a generation stamp.
#include <algorithm>
#include <cstdint>
#include <vector>

#include "geom/point.hpp"

namespace tmk::index {

class UniformGrid {
 public:
  UniformGrid(geom::Box bounds, Coord cell, std::size_t max_ids)
      : b_(bounds), cell_(std::max<Coord>(cell, 1)), stamp_(max_ids, 0) {
    nx_ = static_cast<int>((b_.x1 - b_.x0) / cell_) + 1;
    ny_ = static_cast<int>((b_.y1 - b_.y0) / cell_) + 1;
    cells_.resize(static_cast<std::size_t>(nx_) * static_cast<std::size_t>(ny_));
  }

  void insert(int id, const geom::Box& box) {
    if (static_cast<std::size_t>(id) >= stamp_.size()) stamp_.resize(static_cast<std::size_t>(id) * 2 + 16, 0);
    int x0, y0, x1, y1;
    range(box, x0, y0, x1, y1);
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) cells_[idx(x, y)].push_back(id);
  }

  // Calls f(id) once for every id whose inserted box shares a cell with `box`.
  // Removes `id` from the cells of `box` (the box it was inserted with).
  void erase(int id, const geom::Box& box) {
    int x0, y0, x1, y1;
    range(box, x0, y0, x1, y1);
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) std::erase(cells_[idx(x, y)], id);
  }

  template <class F>
  void query(const geom::Box& box, F&& f) {
    if (++gen_ == 0) {
      std::fill(stamp_.begin(), stamp_.end(), 0u);
      gen_ = 1;
    }
    int x0, y0, x1, y1;
    range(box, x0, y0, x1, y1);
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x)
        for (int id : cells_[idx(x, y)]) {
          auto& s = stamp_[static_cast<std::size_t>(id)];
          if (s == gen_) continue;
          s = gen_;
          f(id);
        }
  }

 private:
  std::size_t idx(int x, int y) const { return static_cast<std::size_t>(y) * static_cast<std::size_t>(nx_) + static_cast<std::size_t>(x); }
  void range(const geom::Box& box, int& x0, int& y0, int& x1, int& y1) const {
    auto cx = [&](Coord v) { return std::clamp(static_cast<int>((v - b_.x0) / cell_), 0, nx_ - 1); };
    auto cy = [&](Coord v) { return std::clamp(static_cast<int>((v - b_.y0) / cell_), 0, ny_ - 1); };
    x0 = cx(box.x0); x1 = cx(box.x1); y0 = cy(box.y0); y1 = cy(box.y1);
  }

  geom::Box b_;
  Coord cell_;
  int nx_ = 1, ny_ = 1;
  std::vector<std::vector<int>> cells_;
  std::vector<std::uint32_t> stamp_;
  std::uint32_t gen_ = 0;
};

}  // namespace tmk::index

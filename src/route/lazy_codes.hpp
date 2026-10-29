// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Lattice-indexed obstacle codes that start "not computed" (INT32_MIN) and are allocated in chunks on first write.
// The router's per-class caches cover the whole board lattice on every layer, but a run touches only the cells its
// windows reach: a full array per class was 1.6 GB per router on KiCad-Library_Teensy_test_layout, and filling it
// with INT32_MIN 3-5 % of the run. Reading an unallocated chunk gives INT32_MIN, as the full array did.
#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tmk::route {

class LazyCodes {
 public:
  static constexpr std::int32_t kUnknown = INT32_MIN;

  // n entries, all unknown; drops any earlier contents.
  void init(std::size_t n) {
    n_ = n;
    chunks_.clear();
    chunks_.resize((n + kChunk - 1) / kChunk);
  }
  bool empty() const { return n_ == 0; }
  std::size_t size() const { return n_; }

  // The entry, allocating its chunk (all unknown) if needed.
  std::int32_t& operator[](std::size_t i) {
    auto& c = chunks_[i >> kShift];
    if (!c) {
      c = std::make_unique<std::int32_t[]>(kChunk);
      for (std::size_t k = 0; k < kChunk; ++k) c[k] = kUnknown;
    }
    return c[i & (kChunk - 1)];
  }
  // The entry without allocating: unknown where nothing was written.
  std::int32_t get(std::size_t i) const {
    const auto& c = chunks_[i >> kShift];
    return c ? c[i & (kChunk - 1)] : kUnknown;
  }
  // Bytes held by allocated chunks (diagnostics, test).
  std::size_t bytes() const {
    std::size_t b = 0;
    for (const auto& c : chunks_) b += c ? kChunk * sizeof(std::int32_t) : 0;
    return b;
  }

 private:
  static constexpr std::size_t kShift = 12;  // 4,096 entries (16 KB): a few lattice rows of a window
  static constexpr std::size_t kChunk = std::size_t{1} << kShift;
  std::size_t n_ = 0;
  std::vector<std::unique_ptr<std::int32_t[]>> chunks_;
};

}  // namespace tmk::route

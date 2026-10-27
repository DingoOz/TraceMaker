// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Back-off for a GPU that refused work (design doc 07 §4).
//
// With the shared GPUs full of other jobs, every cost-to-go field asked the driver for free memory and the threads queued
// on its lock for seconds before falling back to the CPU: interf_u took 2,900 s instead of 20 s, with byte-identical output
// (soak 2026-10-25, phase 6b). A device that refused is therefore not asked again for a while, the delay doubling while it
// stays full (30 s, 1, 2, 4, 8 min, then 15 min); the CPU reference meanwhile computes the same results. Wall time decides
// only which of two identical paths runs, so outputs cannot change (rule 2).
#include <algorithm>
#include <atomic>
#include <cstdint>

namespace tmk::gpu {

class DeviceBackOff {
 public:
  static constexpr std::int64_t kFirstMs = 30'000, kMaxMs = 900'000;

  // True while a refusal is recent enough that the device should not be asked.
  bool waiting(std::int64_t now_ms) const { return now_ms < until_ms_.load(std::memory_order_relaxed); }

  // The device refused (not enough free memory, or allocation failed): wait, twice as long as last time.
  void refused(std::int64_t now_ms) {
    const int n = refusals_.fetch_add(1, std::memory_order_relaxed);
    const std::int64_t delay = std::min(kFirstMs << std::min(n, 5), kMaxMs);
    until_ms_.store(now_ms + delay, std::memory_order_relaxed);
  }

  // The device took work again: the next refusal starts from the first delay.
  void accepted() { refusals_.store(0, std::memory_order_relaxed); }

  // Delay a refusal at this point would set (for tests and logs).
  std::int64_t next_delay_ms() const { return std::min(kFirstMs << std::min(refusals_.load(std::memory_order_relaxed), 5), kMaxMs); }

 private:
  std::atomic<std::int64_t> until_ms_{0};
  std::atomic<int> refusals_{0};
};

}  // namespace tmk::gpu

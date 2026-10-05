// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Deterministic, counter-based random numbers (design doc 02 §6.2).
//
// Every random decision is a pure function of (job seed, stage, item, counter), so results never depend
// on thread timing, and CPU reference code and CUDA kernels produce identical streams.
//
// Philox4x32-10: Salmon, Moraes, Dror, Shaw, "Parallel random numbers: as easy as 1, 2, 3", SC 2011.
// SplitMix64:    Steele, Lea, Flood, "Fast splittable pseudorandom number generators", OOPSLA 2014.
#include <cstdint>

#include "core/hd.hpp"

namespace tmk {

struct U32x4 {
  std::uint32_t v[4];
  TM_HD friend constexpr bool operator==(const U32x4& a, const U32x4& b) {
    return a.v[0] == b.v[0] && a.v[1] == b.v[1] && a.v[2] == b.v[2] && a.v[3] == b.v[3];
  }
};

struct U32x2 {
  std::uint32_t v[2];
};

namespace detail {
TM_HD_INLINE constexpr void mul_hi_lo(std::uint32_t a, std::uint32_t b, std::uint32_t& hi, std::uint32_t& lo) {
  const std::uint64_t p = static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b);
  hi = static_cast<std::uint32_t>(p >> 32);
  lo = static_cast<std::uint32_t>(p);
}
}  // namespace detail

// One Philox4x32-10 block: 128 random bits for a 128-bit counter under a 64-bit key.
TM_HD_INLINE constexpr U32x4 philox4x32_10(U32x4 ctr, U32x2 key) {
  constexpr std::uint32_t kM0 = 0xD2511F53u, kM1 = 0xCD9E8D57u;
  constexpr std::uint32_t kW0 = 0x9E3779B9u, kW1 = 0xBB67AE85u;
  for (int round = 0; round < 10; ++round) {
    if (round > 0) {
      key.v[0] += kW0;
      key.v[1] += kW1;
    }
    std::uint32_t hi0 = 0, lo0 = 0, hi1 = 0, lo1 = 0;
    detail::mul_hi_lo(kM0, ctr.v[0], hi0, lo0);
    detail::mul_hi_lo(kM1, ctr.v[2], hi1, lo1);
    ctr = U32x4{{hi1 ^ ctr.v[1] ^ key.v[0], lo1, hi0 ^ ctr.v[3] ^ key.v[1], lo0}};
  }
  return ctr;
}

// SplitMix64 finaliser: a fast, well-mixed 64-bit hash, used to derive keys and for hashing ids.
TM_HD_INLINE constexpr std::uint64_t splitmix64(std::uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// A random stream identified by (seed, stage, item). Draw number `n` is a pure function of the stream
// and `n`, so any thread or GPU lane can jump to any draw without coordination.
class RngStream {
 public:
  TM_HD constexpr RngStream(std::uint64_t seed, std::uint32_t stage, std::uint64_t item)
      : key_{static_cast<std::uint32_t>(splitmix64(seed ^ (static_cast<std::uint64_t>(stage) << 32))),
             static_cast<std::uint32_t>(splitmix64(seed + stage) >> 32)},
        item_(item) {}

  // Four 32-bit values for block `n` of this stream.
  TM_HD constexpr U32x4 block(std::uint64_t n) const {
    const U32x4 ctr{{static_cast<std::uint32_t>(n), static_cast<std::uint32_t>(n >> 32),
                     static_cast<std::uint32_t>(item_), static_cast<std::uint32_t>(item_ >> 32)}};
    return philox4x32_10(ctr, U32x2{{key_[0], key_[1]}});
  }

  // Draw `n` as a 64-bit value.
  TM_HD constexpr std::uint64_t u64(std::uint64_t n) const {
    const U32x4 b = block(n);
    return (static_cast<std::uint64_t>(b.v[0]) << 32) | b.v[1];
  }

  // Draw `n` as a double in [0, 1) with 53 random bits.
  TM_HD constexpr double uniform(std::uint64_t n) const {
    return static_cast<double>(u64(n) >> 11) * 0x1.0p-53;
  }

 private:
  std::uint32_t key_[2];
  std::uint64_t item_;
};

}  // namespace tmk

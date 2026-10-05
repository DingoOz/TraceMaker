// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Fills an array with draws 0..n-1 of the RNG stream (seed, stage, item).
//
// The first GPU kernel and its CPU reference. It proves the toolchain end to end and establishes the
// equivalence-test pattern every later kernel follows: same inputs, bit-identical outputs.
#include <cstdint>
#include <string>
#include <vector>

namespace tmk::gpu {

std::vector<std::uint64_t> philox_fill_cpu(std::uint64_t seed, std::uint32_t stage, std::uint64_t item,
                                           std::size_t n);

struct GpuStatus {
  bool ok = false;
  std::string error;  // why the GPU path was not used; the caller falls back to the CPU path
};

// GPU version. On failure (no CUDA, not enough free memory, launch error) returns !ok and leaves `out` empty.
GpuStatus philox_fill_cuda(int cuda_index, std::uint64_t seed, std::uint32_t stage, std::uint64_t item, std::size_t n,
                           std::vector<std::uint64_t>& out);

}  // namespace tmk::gpu

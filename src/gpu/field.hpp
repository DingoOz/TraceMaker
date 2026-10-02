#pragma once
// Cost-to-go fields for the router (design doc 05 §5.2, doc 07 §2): exact shortest-path distances from a set
// of target lattice points over a window, on an octilinear multi-layer lattice with through-via moves.
//
// Computed by alternating line sweeps (row, column and both diagonals, each in both directions) plus via
// relaxation, repeated to a fixpoint (GAMER, Lin et al., TCAD 2023). The fixpoint is the unique shortest-path
// distance, so the CPU reference and the CUDA kernel produce identical results whatever the sweep order.
#include <cstdint>
#include <limits>
#include <vector>

#include "gpu/philox_fill.hpp"  // GpuStatus

namespace tmk::gpu {

inline constexpr std::int32_t kFieldInf = std::numeric_limits<std::int32_t>::max() / 4;

struct FieldProblem {
  int w = 0, h = 0, layers = 0;
  std::int32_t step = 0, diag = 0, via = 0;   // move costs (nm-equivalent)
  const std::uint8_t* pass = nullptr;          // [layers][h][w]: 1 = may be entered
  const std::uint8_t* via_pass = nullptr;      // [h][w]: 1 = a via may be placed (all layers)
  const std::uint8_t* target = nullptr;        // [layers][h][w]: 1 = distance 0
};

// CPU reference. `out` is resized to layers*h*w. Returns the number of sweep rounds.
int field_cpu(const FieldProblem& p, std::vector<std::int32_t>& out);

// CUDA version on device `cuda_index`; on failure (no CUDA, no memory) returns !ok and leaves `out` empty.
GpuStatus field_cuda(int cuda_index, const FieldProblem& p, std::vector<std::int32_t>& out);

}  // namespace tmk::gpu

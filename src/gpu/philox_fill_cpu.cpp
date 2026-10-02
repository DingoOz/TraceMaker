#include "core/rng.hpp"
#include "gpu/philox_fill.hpp"

namespace tmk::gpu {

std::vector<std::uint64_t> philox_fill_cpu(std::uint64_t seed, std::uint32_t stage, std::uint64_t item,
                                           std::size_t n) {
  const RngStream s(seed, stage, item);
  std::vector<std::uint64_t> out(n);
  for (std::size_t i = 0; i < n; ++i) out[i] = s.u64(i);
  return out;
}

#if !TM_HAVE_CUDA
GpuStatus philox_fill_cuda(int, std::uint64_t, std::uint32_t, std::uint64_t, std::size_t,
                           std::vector<std::uint64_t>& out) {
  out.clear();
  return {false, "built without CUDA"};
}
#endif

}  // namespace tmk::gpu

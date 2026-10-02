// CPU-vs-GPU equivalence for the Philox fill kernel, on every visible device (design doc 07 §3).
// A device without enough free memory is skipped, not failed: the GPUs are shared with other jobs.
#include <catch2/catch_test_macros.hpp>

#include "gpu/device.hpp"
#include "gpu/philox_fill.hpp"

TEST_CASE("CPU reference fill is deterministic", "[gpu][cpu-reference]") {
  CHECK(tmk::gpu::philox_fill_cpu(1, 2, 3, 1000) == tmk::gpu::philox_fill_cpu(1, 2, 3, 1000));
}

TEST_CASE("GPU Philox fill is bit-identical to the CPU reference on every device", "[gpu][cuda]") {
  if (!tmk::gpu::cuda_compiled()) SKIP("built without CUDA");
  const auto devices = tmk::gpu::list_devices();
  if (devices.empty()) SKIP("no CUDA devices visible");

  constexpr std::size_t n = 1'000'003;  // not a multiple of the block size
  const auto expected = tmk::gpu::philox_fill_cpu(0xC0FFEE, 5, 99, n);
  int ran = 0;
  for (const auto& d : devices) {
    std::vector<std::uint64_t> got;
    const auto status = tmk::gpu::philox_fill_cuda(d.cuda_index, 0xC0FFEE, 5, 99, n, got);
    if (!status.ok) {
      WARN(d.name << " skipped: " << status.error);
      continue;
    }
    INFO(d.name << " (" << d.uuid << ")");
    REQUIRE(got == expected);
    ++ran;
  }
  if (ran == 0) SKIP("no device had enough free memory");
}

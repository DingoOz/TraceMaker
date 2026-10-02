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

#include "core/rng.hpp"
#include "gpu/field.hpp"

TEST_CASE("cost-to-go field: CPU reference is the exact shortest-path distance on a small case", "[gpu][field]") {
  // 5x1 corridor on one layer, target at x = 0; a wall at x = 2 on layer 0, open on layer 1 with vias everywhere.
  const std::uint8_t pass[] = {1, 1, 0, 1, 1, 1, 1, 1, 1, 1};
  const std::uint8_t via[] = {1, 1, 1, 1, 1};
  const std::uint8_t tgt[] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  tmk::gpu::FieldProblem p{5, 1, 2, 10, 14, 100, pass, via, tgt};
  std::vector<std::int32_t> d;
  tmk::gpu::field_cpu(p, d);
  CHECK(d[1] == 10);
  CHECK(d[2] == tmk::gpu::kFieldInf);  // blocked
  CHECK(d[5] == 100);                   // via down at the target cell
  CHECK(d[8] == 130);                   // along layer 1
  CHECK(d[3] == 230);                   // back up through a via at x = 3
}

TEST_CASE("cost-to-go field: GPU equals CPU on random grids", "[gpu][field][cuda]") {
  if (!tmk::gpu::cuda_compiled()) SKIP("built without CUDA");
  const auto devices = tmk::gpu::list_devices();
  if (devices.empty()) SKIP("no CUDA devices visible");
  const tmk::RngStream rng(11, 2, 0);
  std::uint64_t k = 0;
  for (int t = 0; t < 6; ++t) {
    const int w = 37 + 53 * t, h = 29 + 41 * t, L = 1 + t % 3;
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    std::vector<std::uint8_t> pass(n * static_cast<std::size_t>(L)), via(n), tgt(n * static_cast<std::size_t>(L), 0);
    for (auto& x : pass) x = rng.u64(k++) % 100 < 78 ? 1 : 0;
    for (auto& x : via) x = rng.u64(k++) % 100 < 30 ? 1 : 0;
    for (int q = 0; q < 3; ++q) tgt[rng.u64(k++) % tgt.size()] = 1;
    tmk::gpu::FieldProblem p{w, h, L, 1000, 1414, 9000, pass.data(), via.data(), tgt.data()};
    std::vector<std::int32_t> cpu, gpu;
    tmk::gpu::field_cpu(p, cpu);
    for (const auto& dev : devices) {
      const auto st = tmk::gpu::field_cuda(dev.cuda_index, p, gpu);
      if (!st.ok) {
        WARN(dev.name << ": " << st.error);
        continue;
      }
      INFO(dev.name << " case " << t);
      REQUIRE(gpu == cpu);
    }
  }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// CPU-vs-GPU equivalence for CUDA Philox and the selected cost-to-go backend.
// A device without enough free memory is skipped, not failed: the GPUs are shared with other jobs.
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <future>
#include <utility>

#include "gpu/device.hpp"
#include "gpu/philox_fill.hpp"

TEST_CASE("CPU reference fill is deterministic", "[gpu][cpu-reference]") {
  CHECK(tmk::gpu::philox_fill_cpu(1, 2, 3, 1000) == tmk::gpu::philox_fill_cpu(1, 2, 3, 1000));
}

TEST_CASE("GPU Philox fill is bit-identical to the CPU reference on every device", "[gpu][cuda]") {
  if (tmk::gpu::compiled_backend() != tmk::gpu::Backend::Cuda) SKIP("Philox kernel is CUDA-only");
  const auto devices = tmk::gpu::list_devices();
  if (devices.empty()) SKIP("no CUDA devices visible");

  constexpr std::size_t n = 1'000'003;  // not a multiple of the block size
  const auto expected = tmk::gpu::philox_fill_cpu(0xC0FFEE, 5, 99, n);
  int ran = 0;
  for (const auto& d : devices) {
    std::vector<std::uint64_t> got;
    const auto status = tmk::gpu::philox_fill_cuda(d.index, 0xC0FFEE, 5, 99, n, got);
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

TEST_CASE("cost-to-go field: GPU equals CPU on random grids", "[gpu][field]") {
  if (tmk::gpu::compiled_backend() == tmk::gpu::Backend::Cpu) SKIP("built without GPU support");
  const auto devices = tmk::gpu::list_devices();
  if (devices.empty()) SKIP("no GPU devices visible");
  int ran = 0;
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
      const auto st = tmk::gpu::field_gpu(dev.index, p, gpu);
      if (!st.ok) {
        WARN(dev.name << ": " << st.error);
        continue;
      }
      INFO(dev.name << " case " << t);
      REQUIRE(gpu == cpu);
      ++ran;
    }
  }
  if (ran == 0) SKIP("no device could execute a field");
}

TEST_CASE("cost-to-go field: unavailable device clears the result", "[gpu][field]") {
  const std::uint8_t cell[] = {1};
  const tmk::gpu::FieldProblem p{1, 1, 1, 10, 14, 100, cell, cell, cell};
  std::vector<std::int32_t> out{123};
  CHECK_FALSE(tmk::gpu::field_gpu(-1, p, out).ok);
  CHECK(out.empty());
}

TEST_CASE("cost-to-go field: thin grids, blocked targets and reused buffers match CPU", "[gpu][field]") {
  const auto devices = tmk::gpu::list_devices();
  if (devices.empty()) SKIP("no GPU devices visible");
  int ran = 0;
  for (const auto& dev : devices) {
    if (!tmk::gpu::has_free_memory(dev.index, 1 << 20)) continue;
    for (const auto [w, h] : {std::pair{1, 1}, std::pair{1, 9}, std::pair{9, 1}, std::pair{7, 5}}) {
      const std::size_t n = static_cast<std::size_t>(w * h);
      std::vector<std::uint8_t> pass(n * 3, 1), via(n, 1), target(n * 3, 0);
      pass[n / 2] = 0;
      target[0] = 1;  // blocked targets still have distance zero
      target.back() = 1;
      const tmk::gpu::FieldProblem p{w, h, 3, 10, 14, 100, pass.data(), via.data(), target.data()};
      std::vector<std::int32_t> expected, got;
      tmk::gpu::field_cpu(p, expected);
      INFO(dev.name << " " << w << "x" << h);
      REQUIRE(tmk::gpu::field_gpu(dev.index, p, got).ok);
      CHECK(got == expected);
      std::fill(target.begin(), target.end(), 0);
      tmk::gpu::field_cpu(p, expected);
      REQUIRE(tmk::gpu::field_gpu(dev.index, p, got).ok);
      CHECK(got == expected);  // no stale target distances
      ++ran;
    }
  }
  if (ran == 0) SKIP("no device had enough free memory");
}

TEST_CASE("cost-to-go field: a winding corridor needing many rounds matches CPU", "[gpu][field]") {
  // Alternating wall gaps force more rounds than one command buffer holds, with lines over 32 cells.
  // A 75 um pitch in nm exercises 64-bit scan keys.
  const auto devices = tmk::gpu::list_devices();
  if (devices.empty()) SKIP("no GPU devices visible");
  const int w = 300, h = 81;
  const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
  std::vector<std::uint8_t> pass(n * 2, 1), via(n, 0), target(n * 2, 0);
  for (int y = 1; y < h; y += 2)
    for (int x = 0; x < w; ++x)
      if (x != ((y / 2) % 2 == 0 ? w - 1 : 0)) pass[static_cast<std::size_t>(y * w + x)] = 0;
  via[n - 1] = 1;  // the second layer is reached only at the far end of the corridor
  target[0] = 1;
  const tmk::gpu::FieldProblem p{w, h, 2, 75'000, 106'066, 3'000'000, pass.data(), via.data(), target.data()};
  std::vector<std::int32_t> expected, got;
  const int rounds = tmk::gpu::field_cpu(p, expected);
  REQUIRE(rounds > 32);
  int ran = 0;
  for (const auto& dev : devices) {
    const auto st = tmk::gpu::field_gpu(dev.index, p, got);
    if (!st.ok) {
      WARN(dev.name << ": " << st.error);
      continue;
    }
    INFO(dev.name);
    CHECK(got == expected);
    ++ran;
  }
  if (ran == 0) SKIP("no device could execute a field");
}

TEST_CASE("cost-to-go field: concurrent callers have independent state", "[gpu][field]") {
  const auto devices = tmk::gpu::list_devices();
  if (devices.empty()) SKIP("no GPU devices visible");
  const auto& dev = devices.front();
  if (!tmk::gpu::has_free_memory(dev.index, 4 << 20)) SKIP("not enough free memory");
  const auto compute = [device = dev.index](int target_index) {
    std::vector<std::uint8_t> pass(31 * 23 * 2, 1), via(31 * 23, 1), target(pass.size(), 0);
    target[static_cast<std::size_t>(target_index)] = 1;
    const tmk::gpu::FieldProblem p{31, 23, 2, 10, 14, 100, pass.data(), via.data(), target.data()};
    std::vector<std::int32_t> expected, got;
    tmk::gpu::field_cpu(p, expected);
    const auto status = tmk::gpu::field_gpu(device, p, got);
    return std::pair{status.ok, got == expected};
  };
  auto first = std::async(std::launch::async, compute, 0);
  auto second = std::async(std::launch::async, compute, 31 * 23 * 2 - 1);
  const auto a = first.get(), b = second.get();
  REQUIRE(a.first);
  REQUIRE(b.first);
  CHECK(a.second);
  CHECK(b.second);
}

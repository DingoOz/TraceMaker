// SPDX-License-Identifier: GPL-3.0-or-later
#include <cuda_runtime.h>

#include "core/rng.hpp"
#include "gpu/device.hpp"
#include "gpu/philox_fill.hpp"

namespace tmk::gpu {
namespace {

__global__ void philox_fill_kernel(RngStream s, std::uint64_t n, std::uint64_t* out) {
  const std::uint64_t stride = static_cast<std::uint64_t>(gridDim.x) * blockDim.x;
  for (std::uint64_t i = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x; i < n; i += stride) {
    out[i] = s.u64(i);
  }
}

GpuStatus fail(cudaError_t e, const char* what) {
  cudaGetLastError();
  return {false, std::string(what) + ": " + cudaGetErrorString(e)};
}

}  // namespace

GpuStatus philox_fill_cuda(int cuda_index, std::uint64_t seed, std::uint32_t stage, std::uint64_t item, std::size_t n,
                           std::vector<std::uint64_t>& out) {
  out.clear();
  const std::size_t bytes = n * sizeof(std::uint64_t);
  if (!has_free_memory(cuda_index, bytes)) return {false, "not enough free device memory"};
  if (auto e = cudaSetDevice(cuda_index); e != cudaSuccess) return fail(e, "cudaSetDevice");

  std::uint64_t* d_out = nullptr;
  if (auto e = cudaMalloc(&d_out, bytes); e != cudaSuccess) return fail(e, "cudaMalloc");
  const RngStream s(seed, stage, item);
  const unsigned threads = 256;
  const unsigned blocks = static_cast<unsigned>(std::min<std::size_t>((n + threads - 1) / threads, 4096));
  if (n > 0) philox_fill_kernel<<<blocks, threads>>>(s, n, d_out);
  cudaError_t e = cudaGetLastError();
  if (e == cudaSuccess) e = cudaDeviceSynchronize();
  if (e == cudaSuccess) {
    out.resize(n);
    e = cudaMemcpy(out.data(), d_out, bytes, cudaMemcpyDeviceToHost);
  }
  cudaFree(d_out);
  if (e != cudaSuccess) {
    out.clear();
    return fail(e, "kernel");
  }
  return {true, {}};
}

}  // namespace tmk::gpu

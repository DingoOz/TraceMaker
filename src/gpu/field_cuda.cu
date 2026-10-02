#include <cuda_runtime.h>

#include "gpu/device.hpp"
#include "gpu/field.hpp"

namespace tmk::gpu {
namespace {

// One thread per line. Each thread walks its line in sweep order (sequential within the line, parallel across
// lines): identical arithmetic to the CPU reference, and the fixpoint is unique, so results match exactly.
__global__ void sweep_kernel(const std::uint8_t* pass, std::int32_t* d, int w, int h, int dx, int dy, std::int32_t cost, int* changed) {
  const int t = blockIdx.x * blockDim.x + threadIdx.x;
  int x, y;
  if (dy == 0) {
    if (t >= h) return;
    x = dx > 0 ? 0 : w - 1;
    y = t;
  } else if (dx == 0) {
    if (t >= w) return;
    x = t;
    y = dy > 0 ? 0 : h - 1;
  } else {
    const int sx = dx > 0 ? 0 : w - 1, sy = dy > 0 ? 0 : h - 1;
    if (t < w) {
      x = t;
      y = sy;
    } else {
      const int k = t - w;  // rows other than sy
      if (k >= h - 1) return;
      y = k < sy ? k : k + 1;
      x = sx;
    }
  }
  std::int32_t prev = kFieldInf;
  int local = 0;
  while (x >= 0 && y >= 0 && x < w && y < h) {
    const int i = y * w + x;
    if (!pass[i]) {
      prev = kFieldInf;
    } else {
      std::int32_t v = d[i];
      if (prev < kFieldInf && prev + cost < v) {
        v = prev + cost;
        d[i] = v;
        local = 1;
      }
      prev = v;
    }
    x += dx;
    y += dy;
  }
  if (local) *changed = 1;
}

__global__ void via_kernel(const std::uint8_t* pass, const std::uint8_t* via_pass, std::int32_t* d, int n, int layers, std::int32_t via, int* changed) {
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i >= n || !via_pass[i]) return;
  std::int32_t best = kFieldInf;
  for (int l = 0; l < layers; ++l) best = min(best, d[l * n + i]);
  if (best >= kFieldInf) return;
  for (int l = 0; l < layers; ++l)
    if (pass[l * n + i] && best + via < d[l * n + i]) {
      d[l * n + i] = best + via;
      *changed = 1;
    }
}

__global__ void init_kernel(const std::uint8_t* target, std::int32_t* d, int total) {
  const int i = blockIdx.x * blockDim.x + threadIdx.x;
  if (i < total) d[i] = target[i] ? 0 : kFieldInf;
}

}  // namespace

GpuStatus field_cuda(int cuda_index, const FieldProblem& p, std::vector<std::int32_t>& out) {
  out.clear();
  const std::size_t n = static_cast<std::size_t>(p.w) * static_cast<std::size_t>(p.h);
  const std::size_t total = n * static_cast<std::size_t>(p.layers);
  const std::size_t bytes = total * (sizeof(std::int32_t) + 2) + n + sizeof(int);
  if (!has_free_memory(cuda_index, bytes)) return {false, "not enough free device memory"};
  if (cudaSetDevice(cuda_index) != cudaSuccess) return {false, "cudaSetDevice failed"};
  cudaStream_t s = cudaStreamPerThread;
  std::uint8_t *dpass = nullptr, *dvia = nullptr, *dtgt = nullptr;
  std::int32_t* dd = nullptr;
  int* dchanged = nullptr;
  auto cleanup = [&] {
    cudaFreeAsync(dpass, s);
    cudaFreeAsync(dvia, s);
    cudaFreeAsync(dtgt, s);
    cudaFreeAsync(dd, s);
    cudaFreeAsync(dchanged, s);
    cudaStreamSynchronize(s);
  };
  if (cudaMallocAsync(&dpass, total, s) != cudaSuccess || cudaMallocAsync(&dvia, n, s) != cudaSuccess ||
      cudaMallocAsync(&dtgt, total, s) != cudaSuccess || cudaMallocAsync(&dd, total * sizeof(std::int32_t), s) != cudaSuccess ||
      cudaMallocAsync(&dchanged, sizeof(int), s) != cudaSuccess) {
    cudaGetLastError();
    cleanup();
    return {false, "device allocation failed"};
  }
  cudaMemcpyAsync(dpass, p.pass, total, cudaMemcpyHostToDevice, s);
  cudaMemcpyAsync(dvia, p.via_pass, n, cudaMemcpyHostToDevice, s);
  cudaMemcpyAsync(dtgt, p.target, total, cudaMemcpyHostToDevice, s);
  const int threads = 128;
  init_kernel<<<static_cast<unsigned>((total + threads - 1) / threads), threads, 0, s>>>(dtgt, dd, static_cast<int>(total));
  static const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};
  for (int round = 0; round < 100000; ++round) {
    cudaMemsetAsync(dchanged, 0, sizeof(int), s);
    for (int l = 0; l < p.layers; ++l)
      for (const auto& dd2 : dirs) {
        const int lines = dd2[1] == 0 ? p.h : dd2[0] == 0 ? p.w : p.w + p.h - 1;
        sweep_kernel<<<static_cast<unsigned>((lines + threads - 1) / threads), threads, 0, s>>>(
            dpass + static_cast<std::size_t>(l) * n, dd + static_cast<std::size_t>(l) * n, p.w, p.h, dd2[0], dd2[1],
            (dd2[0] && dd2[1]) ? p.diag : p.step, dchanged);
      }
    if (p.layers > 1)
      via_kernel<<<static_cast<unsigned>((n + threads - 1) / threads), threads, 0, s>>>(dpass, dvia, dd, static_cast<int>(n), p.layers, p.via, dchanged);
    int changed = 0;
    cudaMemcpyAsync(&changed, dchanged, sizeof(int), cudaMemcpyDeviceToHost, s);
    if (cudaStreamSynchronize(s) != cudaSuccess) {
      cudaGetLastError();
      cleanup();
      return {false, "kernel failed"};
    }
    if (!changed) break;
  }
  out.resize(total);
  cudaMemcpyAsync(out.data(), dd, total * sizeof(std::int32_t), cudaMemcpyDeviceToHost, s);
  const cudaError_t e = cudaStreamSynchronize(s);
  cleanup();
  if (e != cudaSuccess) {
    out.clear();
    return {false, "copy failed"};
  }
  return {true, {}};
}

}  // namespace tmk::gpu

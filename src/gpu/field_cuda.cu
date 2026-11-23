// SPDX-License-Identifier: GPL-3.0-or-later
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
    const std::int64_t i = static_cast<std::int64_t>(y) * w + x;  // w*h alone can overflow int
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

__global__ void via_kernel(const std::uint8_t* pass, const std::uint8_t* via_pass, std::int32_t* d, std::int64_t n, int layers, std::int32_t via, int* changed) {
  const std::int64_t i = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
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

__global__ void init_kernel(const std::uint8_t* target, std::int32_t* d, std::int64_t total) {
  const std::int64_t i = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (i < total) d[i] = target[i] ? 0 : kFieldInf;
}

// Step 1 of the CUDA field port (design note tracemaker-design-1.md): the device buffers a field needs
// live in a per-thread workspace and are reused across calls, growing only when a field larger than any
// that thread has run arrives. This removes the per-call cudaMallocAsync/cudaFreeAsync cycle and the
// synchronising cleanup it needed; the kernels and their arithmetic are unchanged, so results stay
// bit-identical to the CPU reference. Mirrors the Metal backend's per-thread Workspace (field_metal.mm),
// with one buffer per array instead of one packed slab: CUDA allocations are already 256-byte aligned,
// and per-array growth keeps each array at the largest size its own shape has needed. The `changed`
// buffer is the flags allocation of the port plan; Step 2 widens it to one flag per round.
struct Workspace {
  cudaStream_t stream = nullptr;
  std::uint8_t* pass = nullptr;
  std::uint8_t* via = nullptr;
  std::uint8_t* target = nullptr;
  std::int32_t* distance = nullptr;
  int* changed = nullptr;
  std::size_t pass_cap = 0, via_cap = 0, target_cap = 0, distance_cap = 0, changed_cap = 0;  // elements
};

Workspace& workspace(int index) {
  static thread_local std::vector<Workspace> workspaces;
  if (static_cast<std::size_t>(index) >= workspaces.size()) workspaces.resize(static_cast<std::size_t>(index) + 1);
  return workspaces[static_cast<std::size_t>(index)];
}

// Grows one buffer to `need` elements if its capacity is smaller. The old buffer is freed before
// allocating so admission never counts both (field_metal.mm's prepare_buffer does the same). On failure
// the buffer is left empty (capacity 0) for the next call to retry; buffers already grown keep their
// new size, so the workspace is always in a consistent state.
template <typename T>
bool grow(T*& ptr, std::size_t& cap, std::size_t need) {
  if (cap >= need) return true;
  cudaFree(ptr);
  ptr = nullptr;
  cap = 0;
  if (cudaMalloc(&ptr, need * sizeof(T)) != cudaSuccess) return false;
  cap = need;
  return true;
}

// Device memory the workspace does not already hold for a field of n cells per layer and total cells
// over all layers. Admission is charged on this growth delta only (the retained buffers are already
// allocated), mirroring Metal's reuse accounting in prepare_buffer.
std::size_t growth_bytes(const Workspace& work, std::size_t n, std::size_t total) {
  std::size_t delta = 0;
  if (total > work.pass_cap) delta += (total - work.pass_cap) * sizeof(std::uint8_t);
  if (n > work.via_cap) delta += (n - work.via_cap) * sizeof(std::uint8_t);
  if (total > work.target_cap) delta += (total - work.target_cap) * sizeof(std::uint8_t);
  if (total > work.distance_cap) delta += (total - work.distance_cap) * sizeof(std::int32_t);
  if (work.changed_cap < 1) delta += sizeof(int);
  return delta;
}

}  // namespace

GpuStatus field_gpu(int cuda_index, const FieldProblem& p, std::vector<std::int32_t>& out) {
  out.clear();
  if (cuda_index < 0) return {false, "invalid CUDA device index"};
  // Host-side validation, ported from the Metal backend (field_metal.mm): reject problems the kernels
  // cannot represent before any device work, instead of corrupting silently or crashing on bad input.
  if (p.w < 0 || p.h < 0 || p.layers < 0) return {false, "invalid CUDA field dimensions"};
  if (p.w == 0 || p.h == 0 || p.layers == 0) return {true, {}};
  if (!p.pass || !p.target || (p.layers > 1 && !p.via_pass)) return {false, "missing CUDA field input"};
  constexpr std::int32_t max_cost = std::numeric_limits<std::int32_t>::max() - kFieldInf;
  if (p.step < 0 || p.diag < 0 || p.via < 0 || p.step > max_cost || p.diag > max_cost || p.via > max_cost)
    return {false, "CUDA field costs exceed safe int32 arithmetic"};
  const std::size_t n = static_cast<std::size_t>(p.w) * static_cast<std::size_t>(p.h);
  if (static_cast<std::size_t>(p.layers) > std::numeric_limits<std::size_t>::max() / n)
    return {false, "CUDA field exceeds addressable size"};
  const std::size_t total = n * static_cast<std::size_t>(p.layers);
  if (total > (std::numeric_limits<std::size_t>::max() - n - sizeof(int)) / (sizeof(std::int32_t) + 2))
    return {false, "CUDA field exceeds addressable size"};
  Workspace& work = workspace(cuda_index);
  // Admission is charged on the growth delta only: buffers the workspace already holds are not
  // re-charged. With no growth the call still passes the free-memory margin check, as Metal's reuse
  // path requires headroom >= the margin before reusing its buffer.
  if (!has_free_memory(cuda_index, growth_bytes(work, n, total))) return {false, "not enough free device memory"};
  if (cudaSetDevice(cuda_index) != cudaSuccess) return {false, "cudaSetDevice failed"};
  if (!work.stream && cudaStreamCreateWithFlags(&work.stream, cudaStreamNonBlocking) != cudaSuccess) {
    cudaGetLastError();
    return {false, "stream creation failed"};
  }
  const cudaStream_t s = work.stream;
  if (!grow(work.pass, work.pass_cap, total) || !grow(work.via, work.via_cap, n) ||
      !grow(work.target, work.target_cap, total) || !grow(work.distance, work.distance_cap, total) ||
      !grow(work.changed, work.changed_cap, 1)) {
    cudaGetLastError();
    return {false, "device allocation failed"};
  }
  std::uint8_t* dpass = work.pass;
  std::uint8_t* dvia = work.via;
  std::uint8_t* dtgt = work.target;
  std::int32_t* dd = work.distance;
  int* dchanged = work.changed;
  cudaMemcpyAsync(dpass, p.pass, total, cudaMemcpyHostToDevice, s);
  cudaMemcpyAsync(dvia, p.via_pass, n, cudaMemcpyHostToDevice, s);
  cudaMemcpyAsync(dtgt, p.target, total, cudaMemcpyHostToDevice, s);
  const int threads = 128;
  init_kernel<<<static_cast<unsigned>((total + threads - 1) / threads), threads, 0, s>>>(dtgt, dd, static_cast<std::int64_t>(total));
  static const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};
  int changed = 1;  // until a round changes nothing, the field is not converged
  for (int round = 0; round < p.max_rounds; ++round) {
    cudaMemsetAsync(dchanged, 0, sizeof(int), s);
    for (int l = 0; l < p.layers; ++l)
      for (const auto& dd2 : dirs) {
        const int lines = dd2[1] == 0 ? p.h : dd2[0] == 0 ? p.w : p.w + p.h - 1;
        sweep_kernel<<<static_cast<unsigned>((lines + threads - 1) / threads), threads, 0, s>>>(
            dpass + static_cast<std::size_t>(l) * n, dd + static_cast<std::size_t>(l) * n, p.w, p.h, dd2[0], dd2[1],
            (dd2[0] && dd2[1]) ? p.diag : p.step, dchanged);
      }
    if (p.layers > 1)
      via_kernel<<<static_cast<unsigned>((n + threads - 1) / threads), threads, 0, s>>>(dpass, dvia, dd, static_cast<std::int64_t>(n), p.layers, p.via, dchanged);
    changed = 0;
    cudaMemcpyAsync(&changed, dchanged, sizeof(int), cudaMemcpyDeviceToHost, s);
    if (cudaStreamSynchronize(s) != cudaSuccess) {
      cudaGetLastError();
      return {false, "kernel failed"};
    }
    if (!changed) break;
  }
  if (changed) {  // the round bound was reached while the field was still changing: not a result
    return {false, "CUDA field did not converge"};
  }
  out.resize(total);
  cudaMemcpyAsync(out.data(), dd, total * sizeof(std::int32_t), cudaMemcpyDeviceToHost, s);
  if (cudaStreamSynchronize(s) != cudaSuccess) {
    out.clear();
    return {false, "copy failed"};
  }
  return {true, {}};
}

}  // namespace tmk::gpu

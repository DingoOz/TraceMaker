// SPDX-License-Identifier: GPL-3.0-or-later
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "gpu/device.hpp"
#include "gpu/field.hpp"
#include "gpu/metal_private.hpp"

namespace tmk::gpu {
namespace {

// GAMER (Lin et al., TCAD 2023): within an open run, d'[i] = c*i + min_{j<=i}(d[j] - c*j).
// One 32-lane SIMD-group scans each line, carrying the segmented minimum between blocks.
// Embedded source keeps the executable independent of its installation path.
constexpr char kFieldSource[] = R"metal(
#include <metal_stdlib>
using namespace metal;
constant int field_inf = 536870911;
constant long key_inf = 0x7fffffffffffffff;
constant uint kLinesPerGroup = 4;  // SIMD-groups (lines) per threadgroup
struct Params {
  int w, h, layers, n, total;
  int dx, dy, cost, lines, round;
};
inline bool converged_before(device const atomic_uint* flags, int round) {
  return round > 0 && atomic_load_explicit(&flags[round - 1], memory_order_relaxed) == 0u;
}
inline long up(long v, ushort off) { return as_type<long>(simd_shuffle_up(as_type<uint2>(v), off)); }
inline long down(long v, ushort off) { return as_type<long>(simd_shuffle_down(as_type<uint2>(v), off)); }
inline long lane_value(long v, ushort lane) { return as_type<long>(simd_broadcast(as_type<uint2>(v), lane)); }
kernel void field_init(device const uchar* target [[buffer(2)]],
                       device int* d [[buffer(3)]],
                       constant Params& p [[buffer(5)]],
                       uint t [[thread_position_in_grid]]) {
  if (t < uint(p.total)) d[t] = target[t] ? 0 : field_inf;
}
// Disjoint lines can sweep forward/backward together without changing field_cpu's per-round state.
kernel void field_sweep(device const uchar* pass [[buffer(0)]],
                        device int* d [[buffer(3)]],
                        device atomic_uint* flags [[buffer(4)]],
                        constant Params& p [[buffer(5)]],
                        uint group [[threadgroup_position_in_grid]],
                        ushort sg [[simdgroup_index_in_threadgroup]],
                        ushort lane [[thread_index_in_simdgroup]]) {
  if (converged_before(flags, p.round)) return;  // uniform for the whole grid
  const int line = int(group * kLinesPerGroup + sg);
  if (line >= p.lines * p.layers) return;       // uniform for the SIMD-group
  const int layer = line / p.lines;
  const int t = line % p.lines;
  int x, y;
  if (p.dy == 0) {
    x = p.dx > 0 ? 0 : p.w - 1;
    y = t;
  } else if (p.dx == 0) {
    x = t;
    y = p.dy > 0 ? 0 : p.h - 1;
  } else {
    const int sx = p.dx > 0 ? 0 : p.w - 1;
    const int sy = p.dy > 0 ? 0 : p.h - 1;
    if (t < p.w) {
      x = t;
      y = sy;
    } else {
      const int k = t - p.w;
      y = k < sy ? k : k + 1;
      x = sx;
    }
  }
  int len = p.w + p.h;
  if (p.dx > 0) len = min(len, p.w - x);
  if (p.dx < 0) len = min(len, x + 1);
  if (p.dy > 0) len = min(len, p.h - y);
  if (p.dy < 0) len = min(len, y + 1);
  const int start = layer * p.n + y * p.w + x;
  const int stride = p.dy * p.w + p.dx;
  const long cost = p.cost;
  const int blocks = (len + 31) / 32;
  bool local = false;
  // Forward: inclusive segmented min of d[j] - c*j. Lanes past the end act as blocked cells.
  long carry = key_inf;
  for (int b = 0; b < blocks; ++b) {
    const int i = b * 32 + int(lane);
    const int idx = start + i * stride;
    const bool open = i < len && pass[idx];
    const int v = open ? d[idx] : field_inf;
    const long at = cost * long(i);
    long k = open && v < field_inf ? long(v) - at : key_inf;
    ushort reset = open ? 0 : 1;  // simd shuffles do not take bool
    for (ushort off = 1; off < 32; off <<= 1) {
      const long other = up(k, off);
      const ushort other_reset = simd_shuffle_up(reset, off);
      if (lane >= off) {
        if (!reset) k = min(k, other);
        reset |= other_reset;
      }
    }
    const long run = reset ? k : min(carry, k);
    if (open && run != key_inf && run + at < long(v)) {
      d[idx] = int(run + at);
      local = true;
    }
    carry = lane_value(run, 31);
  }
  // Backward: suffix segmented min of d[j] + c*j over the same blocks, last block first.
  carry = key_inf;
  for (int b = blocks - 1; b >= 0; --b) {
    const int i = b * 32 + int(lane);
    const int idx = start + i * stride;
    const bool open = i < len && pass[idx];
    const int v = open ? d[idx] : field_inf;
    const long at = cost * long(i);
    long k = open && v < field_inf ? long(v) + at : key_inf;
    ushort reset = open ? 0 : 1;
    for (ushort off = 1; off < 32; off <<= 1) {
      const long other = down(k, off);
      const ushort other_reset = simd_shuffle_down(reset, off);
      if (lane + off < 32) {
        if (!reset) k = min(k, other);
        reset |= other_reset;
      }
    }
    const long run = reset ? k : min(carry, k);
    if (open && run != key_inf && run - at < long(v)) {
      d[idx] = int(run - at);
      local = true;
    }
    carry = lane_value(run, 0);
  }
  if (simd_any(local) && lane == 0) atomic_store_explicit(&flags[p.round], 1u, memory_order_relaxed);
}
kernel void field_via(device const uchar* pass [[buffer(0)]],
                      device const uchar* via_pass [[buffer(1)]],
                      device int* d [[buffer(3)]],
                      device atomic_uint* flags [[buffer(4)]],
                      constant Params& p [[buffer(5)]],
                      uint tid [[thread_position_in_grid]]) {
  if (tid >= uint(p.n) || !via_pass[tid] || converged_before(flags, p.round)) return;
  const int i = int(tid);
  int best = field_inf;
  for (int l = 0; l < p.layers; ++l) best = min(best, d[l * p.n + i]);
  if (best >= field_inf) return;
  bool local = false;
  for (int l = 0; l < p.layers; ++l) {
    const int j = l * p.n + i;
    if (pass[j] && best + p.cost < d[j]) {
      d[j] = best + p.cost;
      local = true;
    }
  }
  if (local) atomic_store_explicit(&flags[p.round], 1u, memory_order_relaxed);
}
)metal";
static_assert(kFieldInf == 536870911);

constexpr NSUInteger kLinesPerGroup = 4;  // 32-lane SIMD-groups per threadgroup, as in the shader
constexpr int kFirstRounds = 16;          // rounds in the first command buffer
constexpr int kMoreRounds = 8;            // rounds per later command buffer
constexpr int kMaxRounds = 1024;          // round flags; beyond this the CPU reference takes over

struct Params {
  std::int32_t w, h, layers, n, total;
  std::int32_t dx, dy, cost, lines, round;
};
static_assert(sizeof(Params) == 40);

std::string error_message(NSError* error, const char* fallback) {
  const char* message = error.localizedDescription.UTF8String;
  return message ? message : fallback;
}

struct Context {
  id<MTLDevice> device = nil;
  id<MTLComputePipelineState> init = nil;
  id<MTLComputePipelineState> sweep = nil;
  id<MTLComputePipelineState> via = nil;
  std::string error;

  explicit Context(id<MTLDevice> selected) : device(selected) {
    NSError* failure = nil;
    NSString* source = [NSString stringWithUTF8String:kFieldSource];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&failure];
    if (!library) {
      error = error_message(failure, "Metal field library compilation failed");
      return;
    }
    auto pipeline = [&](NSString* name) -> id<MTLComputePipelineState> {
      id<MTLFunction> function = [library newFunctionWithName:name];
      if (!function) {
        error = "Metal field function missing";
        return nil;
      }
      id<MTLComputePipelineState> state = [device newComputePipelineStateWithFunction:function error:&failure];
      if (!state) error = error_message(failure, "Metal field pipeline creation failed");
      return state;
    };
    init = pipeline(@"field_init");
    if (!init) return;
    sweep = pipeline(@"field_sweep");
    if (!sweep) return;
    via = pipeline(@"field_via");
  }
};

Context& context(int index) {
  static std::mutex mutex;
  static std::vector<std::unique_ptr<Context>> contexts(metal::device_count());
  const std::lock_guard lock(mutex);
  auto& selected = contexts[static_cast<std::size_t>(index)];
  if (!selected) selected = std::make_unique<Context>(metal::device_at_index(index));
  return *selected;
}

// One aligned shared allocation avoids staging allocations and gives exact admission accounting.
struct Layout {
  std::size_t pass = 0, via, target, distance, flags, bytes;
  Layout(std::size_t n, std::size_t total) {
    auto aligned = [](std::size_t size) { return (size + 255) & ~std::size_t{255}; };
    via = aligned(total);
    target = via + aligned(n);
    distance = target + aligned(total);
    flags = distance + aligned(total * sizeof(std::int32_t));
    bytes = flags + kMaxRounds * sizeof(std::uint32_t);
  }
};

// Per-thread queues let portfolio fields run concurrently; each queue executes its commands in order.
struct Workspace {
  id<MTLBuffer> buffer = nil;
  id<MTLCommandQueue> queue = nil;
};

Workspace& workspace(int index) {
  static thread_local std::vector<Workspace> workspaces(metal::device_count());
  return workspaces[static_cast<std::size_t>(index)];
}

bool prepare_buffer(Context& ctx, Workspace& work, std::size_t bytes) {
  if (bytes > ctx.device.maxBufferLength) return false;
  const std::lock_guard lock(metal::allocation_mutex());
  if (work.buffer && work.buffer.length >= bytes)
    return metal::working_set_headroom(ctx.device) >= kDeviceMemoryMargin;
  // Release the old buffer so admission does not count both allocations.
  work.buffer = nil;
  const std::size_t headroom = metal::working_set_headroom(ctx.device);
  if (headroom < kDeviceMemoryMargin || bytes > headroom - kDeviceMemoryMargin) return false;
  work.buffer = [ctx.device newBufferWithLength:bytes
                                      options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeTracked];
  return work.buffer && work.buffer.contents;
}

void dispatch_cells(id<MTLComputeCommandEncoder> encoder, id<MTLComputePipelineState> pipeline, const Params& params,
                    std::size_t count) {
  [encoder setComputePipelineState:pipeline];
  [encoder setBytes:&params length:sizeof params atIndex:5];
  const NSUInteger threads = std::min<NSUInteger>(256, pipeline.maxTotalThreadsPerThreadgroup);
  [encoder dispatchThreadgroups:MTLSizeMake((count + threads - 1) / threads, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
  // Dependent dispatches need a grid-wide buffer barrier, not a shader threadgroup_barrier.
  [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
}

void dispatch_lines(id<MTLComputeCommandEncoder> encoder, id<MTLComputePipelineState> pipeline, const Params& params) {
  [encoder setComputePipelineState:pipeline];
  [encoder setBytes:&params length:sizeof params atIndex:5];
  const NSUInteger lines = static_cast<NSUInteger>(params.lines) * static_cast<NSUInteger>(params.layers);
  [encoder dispatchThreadgroups:MTLSizeMake((lines + kLinesPerGroup - 1) / kLinesPerGroup, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(kLinesPerGroup * 32, 1, 1)];
  [encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
}

GpuStatus compute_field(int index, const FieldProblem& p, std::vector<std::int32_t>& out) {
  if (!metal::device_at_index(index)) return {false, "Metal device unavailable"};
  if (p.w < 0 || p.h < 0 || p.layers < 0) return {false, "invalid Metal field dimensions"};
  if (p.w == 0 || p.h == 0 || p.layers == 0) return {true, {}};
  const std::size_t n = static_cast<std::size_t>(p.w) * static_cast<std::size_t>(p.h);
  const std::size_t limit = static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
  if (n > limit || static_cast<std::size_t>(p.layers) > limit / n)
    return {false, "Metal field exceeds int32 indexing"};
  const std::size_t total = n * static_cast<std::size_t>(p.layers);
  if (!p.pass || !p.target || (p.layers > 1 && !p.via_pass)) return {false, "missing Metal field input"};
  constexpr std::int32_t max_cost = std::numeric_limits<std::int32_t>::max() - kFieldInf;
  if (p.step < 0 || p.diag < 0 || p.via < 0 || p.step > max_cost || p.diag > max_cost || p.via > max_cost)
    return {false, "Metal field costs exceed safe int32 arithmetic"};

  Context& ctx = context(index);
  if (!ctx.error.empty()) return {false, ctx.error};
  if (ctx.sweep.threadExecutionWidth != 32 || ctx.sweep.maxTotalThreadsPerThreadgroup < kLinesPerGroup * 32)
    return {false, "Metal field sweep needs 32-lane SIMD-groups"};
  Workspace& work = workspace(index);
  if (!work.queue) work.queue = [ctx.device newCommandQueue];
  if (!work.queue) return {false, "Metal command queue allocation failed"};
  const Layout layout(n, total);
  if (!prepare_buffer(ctx, work, layout.bytes)) return {false, "Metal working-set budget or buffer allocation failed"};
  auto* contents = static_cast<std::uint8_t*>(work.buffer.contents);
  std::memcpy(contents + layout.pass, p.pass, total);
  if (p.layers > 1) std::memcpy(contents + layout.via, p.via_pass, n);
  std::memcpy(contents + layout.target, p.target, total);
  auto* flags = reinterpret_cast<std::uint32_t*>(contents + layout.flags);
  std::memset(flags, 0, kMaxRounds * sizeof(std::uint32_t));
  Params params{p.w, p.h, p.layers, static_cast<std::int32_t>(n), static_cast<std::int32_t>(total), 0, 0, 0, 0, 0};
  // Keep field_cpu's direction order; layers within a sweep are independent.
  static constexpr int pairs[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
  // Batch rounds to avoid command-buffer round trips; shaders skip rounds after convergence.
  for (int first = 0, batch = kFirstRounds; first < kMaxRounds; first += batch, batch = kMoreRounds) {
    const int last = std::min(kMaxRounds, first + batch);
    // Drain command buffers/encoders every batch, including long fields.
    @autoreleasepool {
      id<MTLCommandBuffer> command = [work.queue commandBuffer];
      if (!command) return {false, "Metal command buffer allocation failed"};
      id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
      if (!encoder) return {false, "Metal compute encoder allocation failed"};
      [encoder setBuffer:work.buffer offset:layout.pass atIndex:0];
      [encoder setBuffer:work.buffer offset:layout.via atIndex:1];
      [encoder setBuffer:work.buffer offset:layout.target atIndex:2];
      [encoder setBuffer:work.buffer offset:layout.distance atIndex:3];
      [encoder setBuffer:work.buffer offset:layout.flags atIndex:4];
      if (first == 0) dispatch_cells(encoder, ctx.init, params, total);
      for (int round = first; round < last; ++round) {
        params.round = round;
        for (const auto& dir : pairs) {
          params.dx = dir[0];
          params.dy = dir[1];
          params.cost = (dir[0] && dir[1]) ? p.diag : p.step;
          params.lines = dir[1] == 0 ? p.h : dir[0] == 0 ? p.w : p.w - 1 + p.h;
          dispatch_lines(encoder, ctx.sweep, params);
        }
        if (p.layers > 1) {
          params.cost = p.via;
          dispatch_cells(encoder, ctx.via, params, n);
        }
      }
      [encoder endEncoding];
      [command commit];
      [command waitUntilCompleted];
      if (command.status != MTLCommandBufferStatusCompleted) {
        work.buffer = nil;
        return {false, error_message(command.error, "Metal field dispatch failed")};
      }
      // Shared storage is coherent after completion; never read while a GPU dispatch is running.
      for (int round = first; round < last; ++round)
        if (!flags[round]) {
          out.resize(total);
          std::memcpy(out.data(), contents + layout.distance, total * sizeof(std::int32_t));
          return {true, {}};
        }
    }
  }
  return {false, "Metal field did not converge"};
}
}  // namespace

GpuStatus field_gpu(int device_index, const FieldProblem& p, std::vector<std::int32_t>& out) {
  out.clear();
  @autoreleasepool {
    try {
      return compute_field(device_index, p, out);
    } catch (const std::bad_alloc&) {
      out.clear();
      return {false, "Metal field host allocation failed"};
    }
  }
}

}  // namespace tmk::gpu

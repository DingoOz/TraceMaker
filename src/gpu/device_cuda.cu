// SPDX-License-Identifier: GPL-3.0-or-later
#include <cuda_runtime.h>

#include <cstdio>

#include "gpu/device.hpp"

namespace tmk::gpu {
namespace {

std::string format_uuid(const cudaUUID_t& id) {
  const auto* b = reinterpret_cast<const unsigned char*>(id.bytes);
  char buf[64];
  std::snprintf(buf, sizeof buf, "GPU-%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return buf;
}

}  // namespace

Backend compiled_backend() { return Backend::Cuda; }

std::vector<DeviceInfo> list_devices() {
  std::vector<DeviceInfo> out;
  int count = 0;
  if (cudaGetDeviceCount(&count) != cudaSuccess) {
    cudaGetLastError();  // clear the sticky error so later calls are unaffected
    return out;
  }
  int previous = 0;
  cudaGetDevice(&previous);
  for (int i = 0; i < count; ++i) {
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, i) != cudaSuccess) continue;
    DeviceInfo info;
    info.index = i;
    info.name = prop.name;
    info.uuid = format_uuid(prop.uuid);
    info.cc_major = prop.major;
    info.cc_minor = prop.minor;
    info.total_bytes = prop.totalGlobalMem;
    if (cudaSetDevice(i) == cudaSuccess) {
      std::size_t free_b = 0, total_b = 0;
      if (cudaMemGetInfo(&free_b, &total_b) == cudaSuccess) info.free_bytes = free_b;
      else cudaGetLastError();
    }
    out.push_back(std::move(info));
  }
  cudaSetDevice(previous);
  return out;
}

std::optional<DeviceInfo> find_device(std::string_view uuid_or_name) {
  for (auto& d : list_devices()) {
    if (d.uuid == uuid_or_name || d.name.find(uuid_or_name) != std::string::npos) return d;
  }
  return std::nullopt;
}

bool has_free_memory(int cuda_index, std::size_t bytes) {
  int previous = 0;
  cudaGetDevice(&previous);
  bool ok = false;
  if (cudaSetDevice(cuda_index) == cudaSuccess) {
    std::size_t free_b = 0, total_b = 0;
    if (cudaMemGetInfo(&free_b, &total_b) == cudaSuccess) ok = free_b >= bytes + kDeviceMemoryMargin;
    else cudaGetLastError();
  }
  cudaSetDevice(previous);
  return ok;
}

}  // namespace tmk::gpu

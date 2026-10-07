// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// GPU discovery and memory admission control (design doc 07 §4).
//
// The GPUs on the development machine are shared with other jobs, so free memory is scarce and changes
// over time. Callers ask `has_free_memory` before large allocations and fall back to the CPU reference
// path when it fails; results are identical either way.
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tmk::gpu {

enum class Backend { Cpu, Cuda, Metal };

struct DeviceInfo {
  int index = -1;           // backend runtime index; select by uuid/name for stability
  std::string name;
  std::string uuid;         // CUDA UUID or Metal registry ID
  int cc_major = 0, cc_minor = 0;
  std::size_t total_bytes = 0;
  std::size_t free_bytes = 0;  // CUDA free memory or Metal working-set headroom
};

// Backend compiled into this build. CPU references are always available.
Backend compiled_backend();

// All visible devices with current memory budget. Empty without an available GPU.
std::vector<DeviceInfo> list_devices();

// Finds a device by exact UUID or by a case-sensitive substring of its name ("V100", "P100").
std::optional<DeviceInfo> find_device(std::string_view uuid_or_name);

// True when `bytes` plus a safety margin fit in the device's free memory right now.
bool has_free_memory(int device_index, std::size_t bytes);

// Safety margin kept free on every device for other processes and runtime overheads.
inline constexpr std::size_t kDeviceMemoryMargin = std::size_t{256} << 20;

}  // namespace tmk::gpu

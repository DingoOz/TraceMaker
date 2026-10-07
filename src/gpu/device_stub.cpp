// SPDX-License-Identifier: GPL-3.0-or-later
// Device API for CPU-only builds: no devices, never enough memory.
#include "gpu/device.hpp"

namespace tmk::gpu {
Backend compiled_backend() { return Backend::Cpu; }
std::vector<DeviceInfo> list_devices() { return {}; }
std::optional<DeviceInfo> find_device(std::string_view) { return std::nullopt; }
bool has_free_memory(int, std::size_t) { return false; }
}  // namespace tmk::gpu

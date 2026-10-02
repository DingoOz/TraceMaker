// Device API for builds without CUDA: no devices, never enough memory.
#include "gpu/device.hpp"

namespace tmk::gpu {
bool cuda_compiled() { return false; }
std::vector<DeviceInfo> list_devices() { return {}; }
std::optional<DeviceInfo> find_device(std::string_view) { return std::nullopt; }
bool has_free_memory(int, std::size_t) { return false; }
}  // namespace tmk::gpu

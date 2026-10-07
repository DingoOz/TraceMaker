// SPDX-License-Identifier: GPL-3.0-or-later
#import <Foundation/Foundation.h>

#include <cstdio>
#include <utility>

#include "gpu/device.hpp"
#include "gpu/metal_private.hpp"

namespace tmk::gpu {
namespace metal {
namespace {

NSArray<id<MTLDevice>>* devices() {
  static NSArray<id<MTLDevice>>* const snapshot = []() -> NSArray<id<MTLDevice>>* {
    @autoreleasepool {
      NSArray<id<MTLDevice>>* all = MTLCopyAllDevices();
      if (!all) return @[];
      return [all sortedArrayUsingComparator:^NSComparisonResult(id<MTLDevice> a, id<MTLDevice> b) {
        if (a.registryID < b.registryID) return NSOrderedAscending;
        if (a.registryID > b.registryID) return NSOrderedDescending;
        return NSOrderedSame;
      }];
    }
  }();
  return snapshot;
}

}  // namespace

std::size_t device_count() { return devices().count; }

id<MTLDevice> device_at_index(int index) {
  if (index < 0 || static_cast<std::size_t>(index) >= device_count()) return nil;
  return devices()[static_cast<NSUInteger>(index)];
}

std::size_t working_set_headroom(id<MTLDevice> device) {
  if (!device) return 0;
  const std::size_t budget = device.recommendedMaxWorkingSetSize;
  const std::size_t allocated = device.currentAllocatedSize;
  return allocated < budget ? budget - allocated : 0;
}

std::mutex& allocation_mutex() {
  static std::mutex mutex;
  return mutex;
}

}  // namespace metal

Backend compiled_backend() { return Backend::Metal; }

std::vector<DeviceInfo> list_devices() {
  @autoreleasepool {
    std::vector<DeviceInfo> out;
    out.reserve(metal::device_count());
    for (std::size_t i = 0; i < metal::device_count(); ++i) {
      id<MTLDevice> device = metal::device_at_index(static_cast<int>(i));
      DeviceInfo info;
      info.index = static_cast<int>(i);
      const char* name = device.name.UTF8String;
      info.name = name ? name : "Metal device";
      char uuid[32];
      std::snprintf(uuid, sizeof uuid, "METAL-%016llx", static_cast<unsigned long long>(device.registryID));
      info.uuid = uuid;
      // CUDA compute capability has no Metal equivalent; leave both fields zero.
      info.total_bytes = device.recommendedMaxWorkingSetSize;
      info.free_bytes = metal::working_set_headroom(device);
      out.push_back(std::move(info));
    }
    return out;
  }
}

std::optional<DeviceInfo> find_device(std::string_view uuid_or_name) {
  for (auto& device : list_devices()) {
    if (device.uuid == uuid_or_name || device.name.find(uuid_or_name) != std::string::npos) return device;
  }
  return std::nullopt;
}

bool has_free_memory(int index, std::size_t bytes) {
  @autoreleasepool {
    const std::lock_guard lock(metal::allocation_mutex());
    const std::size_t headroom = metal::working_set_headroom(metal::device_at_index(index));
    return headroom >= kDeviceMemoryMargin && bytes <= headroom - kDeviceMemoryMargin;
  }
}

}  // namespace tmk::gpu

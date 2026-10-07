// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#import <Metal/Metal.h>

#include <cstddef>
#include <mutex>

namespace tmk::gpu::metal {

// One stable, registry-ID-ordered snapshot shared by discovery and field dispatch.
std::size_t device_count();
id<MTLDevice> device_at_index(int index);

// Process working-set budget, not free VRAM: currentAllocatedSize excludes other processes.
std::size_t working_set_headroom(id<MTLDevice> device);

// Serialize admission and allocation so callers cannot admit against the same headroom.
std::mutex& allocation_mutex();

}  // namespace tmk::gpu::metal

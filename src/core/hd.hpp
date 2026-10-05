// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// TM_HD marks functions that must compile identically for host and device.
// Code shared by a CPU reference path and a CUDA kernel uses it, so both paths run the same arithmetic.
#if defined(__CUDACC__)
#define TM_HD __host__ __device__
#define TM_HD_INLINE __host__ __device__ __forceinline__
#else
#define TM_HD
#define TM_HD_INLINE inline
#endif

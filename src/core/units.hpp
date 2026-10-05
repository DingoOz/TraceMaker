// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <cstdint>

#include "core/hd.hpp"

namespace tmk {

// Engine coordinate: signed 64-bit nanometres. KiCad's internal unit is also 1 nm, so file
// round-trips are exact. Millimetres appear only at I/O and UI boundaries.
using Coord = std::int64_t;

inline constexpr Coord kNmPerMm = 1'000'000;
inline constexpr Coord kNmPerMil = 25'400;

// Converts millimetres to nanometres, rounding half away from zero (KiCad's parser behaviour).
inline Coord mm_to_nm(double mm) { return static_cast<Coord>(std::llround(mm * static_cast<double>(kNmPerMm))); }

TM_HD_INLINE double nm_to_mm(Coord nm) { return static_cast<double>(nm) / static_cast<double>(kNmPerMm); }

}  // namespace tmk

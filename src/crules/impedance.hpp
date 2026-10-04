#pragma once
// Closed-form transmission-line impedance for PCB traces (design doc 15 §5.3) and the IPC-2221 width for
// current (§5.4). Quasi-static (no dispersion: the targets are 45-100 ohm lines below a few GHz), lossless,
// no solder mask (it lowers outer-layer impedance by roughly 1-3 ohm; KiCad's calculator also leaves it off
// by default). Geometry in integer nanometres; impedances in ohm.
//
// Models (each cited at its definition in impedance.cpp):
//   microstrip          Hammerstad & Jensen 1980, with their finite-thickness correction
//   stripline           Cohn 1954 (wide strip, thick) / Wheeler 1978 (narrow strip), asymmetric by the image
//                       split of Wadell 1991 §3.5.3 (two virtual centred striplines in parallel)
//   coupled microstrip  Kirschning & Jansen 1984 static even/odd mode, Jansen 1978 thickness correction,
//                       Bahl & Garg 1977 thickness filling-factor term
//   coupled stripline   Cohn 1955 zero-thickness even/odd, Cohn 1960 thickness correction, asymmetric by the
//                       same image split
//   grounded coplanar   Ghione & Naldi 1983/1984 conformal mapping, Gupta et al. 1996 thickness correction
// Solving for a width or gap is a bisection on integer nanometres: deterministic and exact to 1 nm.
#include <functional>
#include <string>

#include "core/units.hpp"

namespace tmk::crules::imp {

inline constexpr double kEta0 = 376.730313668;   // free-space wave impedance, ohm
inline constexpr double kC0MmPerPs = 0.299792458;  // speed of light, mm/ps

struct Line {                // single-ended line
  double z0 = 0;             // characteristic impedance, ohm
  double eps_eff = 0;        // effective relative permittivity (sets the propagation delay)
};

struct Pair {                // symmetric edge-coupled pair
  double z_even = 0, z_odd = 0;
  double eps_eff_even = 0, eps_eff_odd = 0;
  double zdiff() const { return 2.0 * z_odd; }  // differential = 2 x odd mode
};

// Microstrip: strip of width w and thickness t on a dielectric of height h (strip bottom to plane), εr er.
Line microstrip(Coord w, Coord h, Coord t, double er);
// Stripline between two planes: h1 dielectric from the upper plane to the strip, h2 from the strip to the lower
// plane (the plane spacing is h1 + t + h2); symmetric when h1 == h2. Homogeneous dielectric: eps_eff = er.
Line stripline(Coord w, Coord h1, Coord h2, Coord t, double er);
// Edge-coupled microstrip: two strips of width w, edge-to-edge gap s.
Pair coupled_microstrip(Coord w, Coord s, Coord h, Coord t, double er);
// Edge-coupled stripline (broadside layout not modelled).
Pair coupled_stripline(Coord w, Coord s, Coord h1, Coord h2, Coord t, double er);
// Grounded (conductor-backed) coplanar waveguide: strip w, gap g to the coplanar ground on both sides, plane at h.
Line gcpw(Coord w, Coord g, Coord h, Coord t, double er);

// IPC-2141A's simple forms (the industry rule of thumb, ±5-10 %), used only as an independent cross-check.
double ipc2141_microstrip(Coord w, Coord h, Coord t, double er);
double ipc2141_stripline(Coord w, Coord b, Coord t, double er);  // b = plane spacing, strip centred

// Propagation delay in ps/mm for an effective permittivity: sqrt(eps_eff) / c.
double prop_delay_ps_per_mm(double eps_eff);

// Complete elliptic integral ratio K(k) / K(k'), k' = sqrt(1 - k^2), by the arithmetic-geometric mean
// (K(k) = pi / (2 AGM(1, k'))). Exposed for tests.
double k_ratio(double k);

// ---- solving -----------------------------------------------------------------------------------------------

struct Solved {
  bool ok = false;
  Coord value = 0;           // the solved width or gap, nm
  double z = 0;              // impedance at `value`
  std::string why;           // set when !ok: target below/above what [lo, hi] can reach
};

// Bisection on integer nm for a function that decreases (decreasing = true, e.g. Z(width)) or increases
// (Z(gap)) monotonically on [lo, hi]. Returns the integer closest to the crossing (ties: the smaller value).
Solved solve_monotone(const std::function<double(Coord)>& z_of, double target, Coord lo, Coord hi, bool decreasing);

// ---- stated formula accuracy -------------------------------------------------------------------------------

enum class Structure : std::uint8_t { Microstrip, Stripline, Gcpw };
const char* structure_name(Structure s);
// Formula error stated in the report, percent (doc 15 §5.3 point 3; dev/assumptions-m13.md).
double formula_error_pct(Structure s, bool differential, bool asymmetric);

// ---- IPC-2221 width for current (doc 15 §5.4) ----------------------------------------------------------------

// IPC-2221B §6.2 (from the IPC-D-275 charts): I = k * dT^0.44 * A^0.725, A cross-section in mil^2,
// k = 0.048 external / 0.024 internal layers. Width for `amps` at temperature rise `delta_t_c` with copper
// thickness `copper` (nm), rounded up to the next nm.
Coord width_for_current(double amps, double delta_t_c, Coord copper, bool external);
double current_for_width(Coord width, double delta_t_c, Coord copper, bool external);

}  // namespace tmk::crules::imp

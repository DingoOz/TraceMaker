// SPDX-License-Identifier: GPL-3.0-or-later
// Closed-form PCB transmission-line models (doc 15 §5.3) and IPC-2221 width for current (§5.4).
// All formulas are quasi-static; lengths are normalised to the dielectric height before use, so the units cancel.
#include "crules/impedance.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace tmk::crules::imp {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kE = std::numbers::e;

double d(Coord v) { return static_cast<double>(v); }

// ---- Hammerstad & Jensen 1980 -------------------------------------------------------------------------------
// E. Hammerstad, O. Jensen, "Accurate Models for Microstrip Computer-Aided Design", IEEE MTT-S International
// Microwave Symposium Digest, 1980, pp. 407-409. Static accuracy better than 0.01 % (Z01) and 0.2 % (eps_eff)
// for 0.01 <= u <= 100, er <= 128.

// Impedance of a zero-thickness microstrip of normalised width u = w/h in air (eq. 1-2).
double hj_z01(double u) {
  const double f = 6.0 + (2.0 * kPi - 6.0) * std::exp(-std::pow(30.666 / u, 0.7528));
  return kEta0 / (2.0 * kPi) * std::log(f / u + std::sqrt(1.0 + 4.0 / (u * u)));
}

// Static effective permittivity of a zero-thickness microstrip (eq. 3-5).
double hj_eps_eff(double u, double er) {
  const double u4 = u * u * u * u;
  const double a = 1.0 + std::log((u4 + (u / 52.0) * (u / 52.0)) / (u4 + 0.432)) / 49.0 + std::log(1.0 + std::pow(u / 18.1, 3.0)) / 18.7;
  const double b = 0.564 * std::pow((er - 0.9) / (er + 3.0), 0.053);
  return (er + 1.0) / 2.0 + (er - 1.0) / 2.0 * std::pow(1.0 + 10.0 / u, -a * b);
}

// Width increase from strip thickness tn = t/h: in air (er = 1) and on the substrate (eq. 6-7).
double hj_du_air(double u, double tn) {
  if (tn <= 0) return 0;
  const double th = std::tanh(std::sqrt(6.517 * u));
  return tn / kPi * std::log(1.0 + 4.0 * kE * th * th / tn);
}
double hj_du_sub(double du_air, double er) { return 0.5 * du_air * (1.0 + 1.0 / std::cosh(std::sqrt(er - 1.0))); }

// ---- Cohn 1954 / Wheeler 1978: single stripline between planes spaced b, strip centred -----------------------
// Wide strip (w / (b - t) >= 0.35): S. B. Cohn, "Characteristic Impedance of the Shielded-Strip Transmission Line",
// IRE Trans. MTT-2, July 1954, pp. 52-57: parallel-plate capacitance plus Cohn's thick-strip fringing capacitance
// (his eq. 2). Narrow strip: H. A. Wheeler, "Transmission-Line Properties of a Strip Line Between Parallel
// Planes", IEEE Trans. MTT-26, Nov. 1978, pp. 866-876, via the equivalent round-conductor diameter. Both as
// collected in B. C. Wadell, "Transmission Line Design Handbook", Artech House 1991, §3.5.1.

// Cohn's fringing capacitance per edge, normalised to the permittivity, for strip thickness t in spacing b.
double cohn_fringe(double t, double b) {
  if (t <= 0) return 2.0 * std::log(2.0) / kPi;
  const double x = t / b;
  const double y = 1.0 / (1.0 - x);
  return ((2.0 * y) * std::log(y + 1.0) - (y - 1.0) * std::log(y * y - 1.0)) / kPi;
}

double stripline_centred(double w, double b, double t, double er) {
  const double hmt = b - t;
  if (w / hmt >= 0.35) {
    // Z0 = eta0 (b - t) / (4 sqrt(er) (w + 2 Cf' (b - t) / 2)) with Cf' the per-edge fringe (Cohn 1954 eq. 2).
    const double cf = cohn_fringe(t, b) * hmt;  // both edges together: (b - t) * C_f / eps, as Cohn's eq. writes it
    return kEta0 * hmt / (4.0 * std::sqrt(er) * (w + cf));
  }
  double de;  // equivalent diameter (Wheeler)
  if (t <= 0) {
    de = w / 2.0;
  } else {
    double x = t / w;
    if (x > 1.0) x = 1.0 / x;
    de = 1.0 + x / kPi * (1.0 + std::log(4.0 * kPi / x)) + 0.236 * std::pow(x, 1.65);
    de *= (t < w ? w : t) / 2.0;
  }
  return kEta0 / (2.0 * kPi * std::sqrt(er)) * std::log(4.0 * b / (kPi * de));
}

// ---- Cohn 1955: coupled stripline, centred ---------------------------------------------------------------------
// S. B. Cohn, "Shielded Coupled-Strip Transmission Line", IRE Trans. MTT-3, Oct. 1955, pp. 29-38: exact
// zero-thickness even/odd impedances by conformal mapping (eq. 2-7) and the thick-strip corrections (eq. 18, 20,
// 22), which add the change of the single strip's capacitance scaled by the fringing-capacitance ratio.
Pair coupled_stripline_centred(double w, double s, double b, double t, double er) {
  const double a1 = kPi * w / (2.0 * b), a2 = kPi * (w + s) / (2.0 * b);
  const double ke = std::tanh(a1) * std::tanh(a2);
  const double ko = std::tanh(a1) / std::tanh(a2);
  const double pre = kEta0 / (4.0 * std::sqrt(er));
  const double ze0 = pre / k_ratio(ke);  // K(k')/K(k)
  const double zo0 = pre / k_ratio(ko);
  Pair p;
  p.eps_eff_even = p.eps_eff_odd = er;
  if (t <= 0) {
    p.z_even = ze0;
    p.z_odd = zo0;
    return p;
  }
  const double z_t = stripline_centred(w, b, t, er);
  const double z_0 = pre * k_ratio(1.0 / std::cosh(a1));  // zero-thickness single strip: K(sech)/K(tanh)
  const double cf_ratio = cohn_fringe(t, b) / cohn_fringe(0, b);
  p.z_even = 1.0 / (1.0 / z_t - cf_ratio * (1.0 / z_0 - 1.0 / ze0));                        // eq. 18
  if (s / t >= 5.0) {
    p.z_odd = 1.0 / (1.0 / z_t + cf_ratio * (1.0 / zo0 - 1.0 / z_0));                       // eq. 20
  } else {                                                                                  // eq. 22: thin gaps
    // Cohn writes the capacitive terms with the medium's wave impedance eta = eta0 / sqrt(er).
    const double eta = kEta0 / std::sqrt(er);
    const double dcf = cohn_fringe(t, b) - cohn_fringe(0, b);
    p.z_odd = 1.0 / (1.0 / zo0 + (1.0 / z_t - 1.0 / z_0) - 2.0 / eta * dcf + 2.0 * t / (eta * s));
  }
  return p;
}

// Wadell 1991 §3.5.3 (eq. 3.5.3.x / 3.6.3.22 for pairs): an off-centre strip at h1 below the upper plane and h2
// above the lower one behaves as two centred striplines of spacing 2 h1 + t and 2 h2 + t in parallel, each
// contributing half its admittance.
double image_split(double z1, double z2) { return 2.0 / (1.0 / z1 + 1.0 / z2); }

// ---- Kirschning & Jansen 1984: coupled microstrip --------------------------------------------------------------
// M. Kirschning, R. H. Jansen, "Accurate Wide-Range Design Equations for the Frequency-Dependent Characteristic
// of Parallel Coupled Microstrip Lines", IEEE Trans. MTT-32, Jan. 1984, pp. 83-90 (errata MTT-33, Mar. 1985,
// p. 288): static even/odd effective permittivity (eq. 3-4) and impedances (eq. 8-9, Q1-Q10), stated accuracy
// 0.7 % for 0.1 <= u <= 10, 0.1 <= g <= 10, 1 <= er <= 18. Finite thickness: R. H. Jansen, "High-Speed
// Computation of Single and Coupled Microstrip Parameters Including Dispersion, High-Order Modes, Loss and Finite
// Strip Thickness", IEEE Trans. MTT-26, Feb. 1978, pp. 75-82 (mode-dependent width increase), and the filling
// factor reduction of I. J. Bahl, R. Garg, "Simple and Accurate Formulas for a Microstrip with Finite Strip
// Thickness", Proc. IEEE 65, Nov. 1977, pp. 1611-1612.
double kj_ae(double v) {
  const double v3 = v * v * v, v4 = v3 * v;
  return 1.0 + std::log((v4 + v * v / 2704.0) / (v4 + 0.432)) / 49.0 + std::log(1.0 + v3 / 5929.741) / 18.7;
}

// Bahl & Garg thickness term on the filling factor, tn = t/h.
double bg_qt(double u, double tn) { return tn > 0 ? 2.0 * std::log(2.0) / kPi * tn / std::sqrt(u) : 0.0; }

}  // namespace

double k_ratio(double k) {
  // K(k)/K(k') = AGM(1, k) / AGM(1, k'): K(k) = pi / (2 AGM(1, k')), K(k') = pi / (2 AGM(1, k)).
  const double kp = std::sqrt(std::max(0.0, 1.0 - k * k));
  auto agm = [](double a, double b) {
    for (int i = 0; i < 64 && std::fabs(a - b) > 1e-15 * a; ++i) {
      const double an = 0.5 * (a + b);
      b = std::sqrt(a * b);
      a = an;
    }
    return a;
  };
  return agm(1.0, k) / agm(1.0, kp);
}

Line microstrip(Coord w_nm, Coord h_nm, Coord t_nm, double er) {
  const double u = d(w_nm) / d(h_nm), tn = d(t_nm) / d(h_nm);
  const double du1 = hj_du_air(u, tn);
  const double u1 = u + du1, ur = u + hj_du_sub(du1, er);
  const double z01_r = hj_z01(ur), z01_1 = hj_z01(u1);
  const double ee_r = hj_eps_eff(ur, er);
  Line l;
  l.z0 = z01_r / std::sqrt(ee_r);                      // H&J eq. 8 with the substrate-corrected width
  l.eps_eff = ee_r * (z01_1 / z01_r) * (z01_1 / z01_r);  // H&J eq. 9
  return l;
}

Line stripline(Coord w, Coord h1, Coord h2, Coord t, double er) {
  Line l;
  l.eps_eff = er;
  if (h1 == h2) {
    l.z0 = stripline_centred(d(w), d(h1 + h2 + t), d(t), er);
  } else {
    l.z0 = image_split(stripline_centred(d(w), d(2 * h1 + t), d(t), er), stripline_centred(d(w), d(2 * h2 + t), d(t), er));
  }
  return l;
}

Pair coupled_stripline(Coord w, Coord s, Coord h1, Coord h2, Coord t, double er) {
  if (h1 == h2) return coupled_stripline_centred(d(w), d(s), d(h1 + h2 + t), d(t), er);
  const Pair a = coupled_stripline_centred(d(w), d(s), d(2 * h1 + t), d(t), er);
  const Pair b = coupled_stripline_centred(d(w), d(s), d(2 * h2 + t), d(t), er);
  Pair p;
  p.z_even = image_split(a.z_even, b.z_even);
  p.z_odd = image_split(a.z_odd, b.z_odd);
  p.eps_eff_even = p.eps_eff_odd = er;
  return p;
}

Pair coupled_microstrip(Coord w_nm, Coord s_nm, Coord h_nm, Coord t_nm, double er) {
  const double u = d(w_nm) / d(h_nm), g = d(s_nm) / d(h_nm), tn = d(t_nm) / d(h_nm);
  // Zero-thickness single line of the same width (K&J build the pair on it).
  const double ee0 = hj_eps_eff(u, er);
  const double zl0 = hj_z01(u) / std::sqrt(ee0);
  // Jansen 1978: thickness widens the even mode less than the odd mode (the gap edge carries odd-mode charge).
  double ue = u, uo = u;
  if (tn > 0) {
    const double du = 1.25 * tn / kPi * (1.0 + std::log((2.0 + (4.0 * kPi * u - 2.0) / (1.0 + std::exp(-100.0 * (u - 1.0 / (2.0 * kPi))))) / tn));
    const double dt = tn / (g * er);
    const double due = du * (1.0 - 0.5 * std::exp(-0.69 * du / dt));
    ue = u + due;
    uo = u + due + dt;
  }
  Pair p;
  // Even-mode effective permittivity (K&J eq. 3).
  {
    const double v = ue * (20.0 + g * g) / (10.0 + g * g) + g * std::exp(-g);
    const double be = 0.564 * std::pow((er - 0.9) / (er + 3.0), 0.053);
    const double q = std::pow(1.0 + 10.0 / v, -kj_ae(v) * be) - bg_qt(ue, tn);
    p.eps_eff_even = 0.5 * (er + 1.0) + 0.5 * (er - 1.0) * q;
  }
  // Odd-mode effective permittivity (K&J eq. 4).
  {
    const double bo = 0.747 * er / (0.15 + er);
    const double co = bo - (bo - 0.207) * std::exp(-0.414 * uo);
    const double dO = 0.593 + 0.694 * std::exp(-0.562 * uo);
    const double ao = 0.7287 * (ee0 - 0.5 * (er + 1.0)) * (1.0 - std::exp(-0.179 * uo));
    const double q = std::exp(-co * std::pow(g, dO)) - bg_qt(uo, tn);
    p.eps_eff_odd = (0.5 * (er + 1.0) + ao - ee0) * q + ee0;
  }
  // Impedances (K&J eq. 8-9).
  const double q1 = 0.8695 * std::pow(ue, 0.194);
  const double q2 = 1.0 + 0.7519 * g + 0.189 * std::pow(g, 2.31);
  const double q3 = 0.1975 + std::pow(16.6 + std::pow(8.4 / g, 6.0), -0.387) + std::log(std::pow(g, 10.0) / (1.0 + std::pow(g / 3.4, 10.0))) / 241.0;
  const double q4 = 2.0 * q1 / (q2 * (std::exp(-g) * std::pow(ue, q3) + (2.0 - std::exp(-g)) * std::pow(ue, -q3)));
  p.z_even = zl0 * std::sqrt(ee0 / p.eps_eff_even) / (1.0 - std::sqrt(ee0) * q4 * zl0 / kEta0);
  const double q5 = 1.794 + 1.14 * std::log(1.0 + 0.638 / (g + 0.517 * std::pow(g, 2.43)));
  const double q6 = 0.2305 + std::log(std::pow(g, 10.0) / (1.0 + std::pow(g / 5.8, 10.0))) / 281.3 + std::log(1.0 + 0.598 * std::pow(g, 1.154)) / 5.1;
  const double q7 = (10.0 + 190.0 * g * g) / (1.0 + 82.3 * g * g * g);
  const double q8 = std::exp(-6.5 - 0.95 * std::log(g) - std::pow(g / 0.15, 5.0));
  const double q9 = std::log(q7) * (q8 + 1.0 / 16.5);
  const double q10 = (q2 * q4 - q5 * std::exp(std::log(uo) * q6 * std::pow(uo, -q9))) / q2;
  p.z_odd = zl0 * std::sqrt(ee0 / p.eps_eff_odd) / (1.0 - std::sqrt(ee0) * q10 * zl0 / kEta0);
  return p;
}

Line gcpw(Coord w_nm, Coord g_nm, Coord h_nm, Coord t_nm, double er) {
  // Conformal mapping: G. Ghione, C. Naldi, "Parameters of Coplanar Waveguides with Lower Common Planes",
  // Electronics Letters 19(18), 1983, pp. 734-735 (back-metal k3) and "Analytical Formulas for Coplanar Lines in
  // Hybrid and Monolithic MICs", Electronics Letters 20(4), 1984, pp. 179-181; also Wadell 1991 §3.4.3.
  // Thickness: K. C. Gupta, R. Garg, I. Bahl, P. Bhartia, "Microstrip Lines and Slotlines", 2nd ed., Artech House
  // 1996, eq. 7.98-7.100 (centre strip widened and slots narrowed by d; eps_eff reduced by the slot fill).
  const double w = d(w_nm), s = d(g_nm), h = d(h_nm), t = d(t_nm);
  const double q1 = k_ratio(w / (w + 2.0 * s));
  const double k3 = std::tanh(kPi * w / (4.0 * h)) / std::tanh(kPi * (w + 2.0 * s) / (4.0 * h));
  const double q3 = k_ratio(k3);
  double qe = q1;
  if (t > 0) {
    const double dd = 1.25 * t / kPi * (1.0 + std::log(4.0 * kPi * w / t));
    const double we = w + dd, se = s - dd;
    if (se > 0) qe = k_ratio(we / (we + 2.0 * se));
  }
  const double qz = 1.0 / (qe + q3);
  double ee = 1.0 + q3 * qz * (er - 1.0);
  if (t > 0) ee -= 0.7 * (ee - 1.0) * t / s / (q1 + 0.7 * t / s);
  Line l;
  l.eps_eff = ee;
  l.z0 = kEta0 / 2.0 * qz / std::sqrt(ee);
  return l;
}

double ipc2141_microstrip(Coord w, Coord h, Coord t, double er) {
  // IPC-2141A (2004) §4.2.1: Z0 = 87 / sqrt(er + 1.41) ln(5.98 h / (0.8 w + t)); 0.1 < w/h < 2.0, 1 < er < 15.
  return 87.0 / std::sqrt(er + 1.41) * std::log(5.98 * d(h) / (0.8 * d(w) + d(t)));
}

double ipc2141_stripline(Coord w, Coord b, Coord t, double er) {
  // IPC-2141A §4.2.3: Z0 = 60 / sqrt(er) ln(4 b / (0.67 pi (0.8 w + t))); w/(b - t) < 0.35, t/b < 0.25.
  return 60.0 / std::sqrt(er) * std::log(4.0 * d(b) / (0.67 * kPi * (0.8 * d(w) + d(t))));
}

double prop_delay_ps_per_mm(double eps_eff) { return std::sqrt(eps_eff) / kC0MmPerPs; }

Solved solve_monotone(const std::function<double(Coord)>& z_of, double target, Coord lo, Coord hi, bool decreasing) {
  Solved r;
  // Orient so that f increases with x: f(x) = z(x) - target for an increasing z, target - z(x) for a decreasing one.
  auto f = [&](Coord x) { const double z = z_of(x); return decreasing ? target - z : z - target; };
  const double flo = f(lo), fhi = f(hi);
  if (!std::isfinite(flo) || !std::isfinite(fhi)) {
    r.why = "formula not finite at the search bounds";
    return r;
  }
  if (flo > 0) {
    r.why = "target needs less than " + std::to_string(nm_to_mm(lo)).substr(0, 5) + " mm";
    r.value = lo;
    r.z = z_of(lo);
    return r;
  }
  if (fhi < 0) {
    r.why = "target needs more than " + std::to_string(nm_to_mm(hi)).substr(0, 5) + " mm";
    r.value = hi;
    r.z = z_of(hi);
    return r;
  }
  Coord a = lo, b = hi;  // invariant: f(a) <= 0 <= f(b)
  while (b - a > 1) {
    const Coord m = a + (b - a) / 2;
    if (f(m) <= 0) a = m;
    else b = m;
  }
  const double za = z_of(a), zb = z_of(b);
  const bool pick_a = std::fabs(za - target) <= std::fabs(zb - target);
  r.ok = true;
  r.value = pick_a ? a : b;
  r.z = pick_a ? za : zb;
  return r;
}

const char* structure_name(Structure s) {
  switch (s) {
    case Structure::Microstrip: return "microstrip";
    case Structure::Stripline: return "stripline";
    case Structure::Gcpw: return "gcpw";
  }
  return "?";
}

double formula_error_pct(Structure s, bool differential, bool asymmetric) {
  // Stated model error (not fab tolerance): the papers' own accuracy plus what the model leaves out (solder mask
  // on outer layers, dispersion, the image split for off-centre strips). See dev/assumptions-m13.md (P4).
  switch (s) {
    case Structure::Microstrip: return differential ? 5.0 : 4.0;
    case Structure::Stripline: return asymmetric ? (differential ? 8.0 : 5.0) : (differential ? 4.0 : 2.0);
    case Structure::Gcpw: return 7.0;
  }
  return 10.0;
}

Coord width_for_current(double amps, double delta_t_c, Coord copper, bool external) {
  if (amps <= 0 || delta_t_c <= 0 || copper <= 0) return 0;
  const double k = external ? 0.048 : 0.024;
  const double area_mil2 = std::pow(amps / (k * std::pow(delta_t_c, 0.44)), 1.0 / 0.725);
  const double t_mil = d(copper) / d(kNmPerMil);
  return static_cast<Coord>(std::ceil(area_mil2 / t_mil * d(kNmPerMil)));
}

double current_for_width(Coord width, double delta_t_c, Coord copper, bool external) {
  if (width <= 0 || delta_t_c <= 0 || copper <= 0) return 0;
  const double k = external ? 0.048 : 0.024;
  const double area_mil2 = (d(width) / d(kNmPerMil)) * (d(copper) / d(kNmPerMil));
  return k * std::pow(delta_t_c, 0.44) * std::pow(area_mil2, 0.725);
}

}  // namespace tmk::crules::imp

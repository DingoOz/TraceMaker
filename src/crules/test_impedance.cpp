// Impedance solver and IPC-2221 width tests (design doc 15 §5.3-5.4, §9.1 L0). Every reference value names its
// source and tolerance. Three kinds of reference:
//   textbook  - worked examples in D. M. Pozar, "Microwave Engineering", 4th ed., Wiley 2012 (Ex. 3.5, 3.7);
//               their formulas are close fits of the same physics, so a few tenths of a percent apart;
//   exact     - zero-thickness conformal-mapping results (Cohn 1955 coupled stripline, Ghione & Naldi CPW)
//               evaluated independently with scipy.special.ellipk (2026-10-04);
//   KiCad     - KiCad's PCB calculator code (common/transline_calculations, master of 2026-10-04) compiled outside
//               the tree and run at 1 MHz (quasi-static). It implements the same papers, with known differences:
//               its single microstrip adds the Bahl & Garg thickness term to Hammerstad & Jensen (2-3 % lower
//               eps_eff on thin dielectrics), its coupled stripline uses Cohn's thin-gap equation without the
//               sqrt(er) of the medium impedance, and its elliptic integral gives a few % different zero-thickness
//               values. Tolerances below are set from those differences, not widened to make tests pass.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "crules/impedance.hpp"

using namespace tmk;
using namespace tmk::crules;
using Catch::Approx;

namespace {

constexpr Coord um(double v) { return static_cast<Coord>(v * 1000.0 + (v >= 0 ? 0.5 : -0.5)); }
Coord mm(double v) { return mm_to_nm(v); }
double rel(double a, double b) { return std::fabs(a - b) / std::fabs(b); }

}  // namespace

TEST_CASE("elliptic integral ratio K(k)/K(k')", "[impedance]") {
  CHECK(imp::k_ratio(std::sqrt(0.5)) == Approx(1.0).epsilon(1e-12));  // k = k' : ratio 1
  // scipy.special.ellipk(m = k^2) / ellipk(1 - k^2)
  CHECK(imp::k_ratio(0.5) == Approx(0.7817009613480559).epsilon(1e-10));
  CHECK(imp::k_ratio(0.1) == Approx(0.42610933023021025).epsilon(1e-10));
  CHECK(imp::k_ratio(0.9) * imp::k_ratio(std::sqrt(1 - 0.81)) == Approx(1.0).epsilon(1e-10));  // K(k)/K(k') x K(k')/K(k)
}

TEST_CASE("microstrip: Hammerstad & Jensen against textbook, KiCad and IPC-2141A", "[impedance]") {
  // Pozar Ex. 3.7: 50 ohm on 1.27 mm, er 2.20 -> W = 3.911 mm, eps_eff 1.87 (zero thickness). Tolerance 0.5 %.
  const auto p = imp::microstrip(mm(3.911), mm(1.27), 0, 2.2);
  CHECK(rel(p.z0, 50.0) < 0.005);
  CHECK(rel(p.eps_eff, 1.87) < 0.01);
  // KiCad (1 MHz): identical at zero thickness (same H&J equations); with 35 um copper KiCad is 0.2-3 % higher
  // because of its extra thickness term. Tolerance 0.1 % at t = 0, 3 % with copper.
  CHECK(rel(imp::microstrip(mm(3.911), mm(1.27), 0, 2.2).z0, 50.0355) < 0.001);
  CHECK(rel(imp::microstrip(mm(2.9), mm(1.51), um(35), 4.5).z0, 49.0330) < 0.03);
  CHECK(rel(imp::microstrip(mm(0.35), mm(0.2), um(35), 4.3).z0, 51.4430) < 0.03);
  CHECK(rel(imp::microstrip(mm(0.18), mm(0.1), um(35), 4.4).z0, 49.2874) < 0.03);
  // IPC-2141A rule of thumb (valid 0.1 < w/h < 2): within 10 %.
  for (double w : {0.1, 0.2, 0.3, 0.38}) {
    INFO("w " << w);
    CHECK(rel(imp::microstrip(mm(w), mm(0.2), um(35), 4.3).z0, imp::ipc2141_microstrip(mm(w), mm(0.2), um(35), 4.3)) < 0.10);
  }
}

TEST_CASE("microstrip: 50 ohm on 1.6 mm FR-4 is about 3 mm wide (doc 15 §5.3)", "[impedance]") {
  // Doc 15 §5.3 / KiCad calculator: 50 ohm microstrip on 1.6 mm FR-4 (er 4.5, 35 um) is 2.9-3.0 mm wide.
  const auto sv = imp::solve_monotone([](Coord w) { return imp::microstrip(w, mm(1.6), um(35), 4.5).z0; }, 50.0, mm(0.1), mm(10), true);
  REQUIRE(sv.ok);
  CHECK(sv.value > mm(2.9));
  CHECK(sv.value < mm(3.0));
}

TEST_CASE("stripline: Cohn/Wheeler against textbook and KiCad", "[impedance]") {
  // Pozar Ex. 3.5: b = 3.2 mm, er 2.20, 50 ohm -> W = 2.66 mm (t = 0). Tolerance 0.5 %.
  CHECK(rel(imp::stripline(mm(2.66), mm(1.6), mm(1.6), 0, 2.2).z0, 50.0) < 0.005);
  // KiCad (same Cohn/Wheeler equations and image split): 0.1 %.
  // b = 20 mil, t = 0.7 mil, w = 8 mil, centred (a = 9.65 mil).
  CHECK(rel(imp::stripline(mm(0.2032), mm(0.24511), mm(0.508 - 0.24511 - 0.01778), mm(0.01778), 4.3).z0, 49.7075) < 0.001);
  // Off-centre: 0.1 mm above, 0.365 mm below, narrow and wide strips.
  CHECK(rel(imp::stripline(mm(0.15), mm(0.1), mm(0.365), um(35), 4.3).z0, 43.4971) < 0.001);
  CHECK(rel(imp::stripline(mm(0.3), mm(0.1), mm(0.365), um(35), 4.3).z0, 29.6889) < 0.001);
  // IPC-2141A stripline (w/(b-t) < 0.35): within 10 %.
  CHECK(rel(imp::stripline(mm(0.12), mm(0.2325), mm(0.2325), um(35), 4.3).z0, imp::ipc2141_stripline(mm(0.12), mm(0.5), um(35), 4.3)) < 0.10);
  CHECK(imp::stripline(mm(0.2), mm(0.2), mm(0.2), um(35), 4.3).eps_eff == 4.3);
}

TEST_CASE("coupled microstrip: Kirschning & Jansen against KiCad", "[impedance]") {
  // KiCad runs the same K&J / Jansen / Bahl-Garg chain: agreement to 0.01 % (rounding only); tolerance 0.1 %.
  struct C { double er, h, t, w, s, zdiff; };
  for (const C c : {C{4.3, 0.2, 0.035, 0.15, 0.2, 133.3032}, C{4.4, 0.1, 0.035, 0.12, 0.15, 113.2457}, C{4.4, 0.2, 0.035, 0.3, 0.15, 92.7780},
                    C{4.5, 1.51, 0.035, 1.5, 0.2, 79.1502}}) {
    INFO("w " << c.w << " s " << c.s << " h " << c.h);
    const auto p = imp::coupled_microstrip(mm(c.w), mm(c.s), mm(c.h), mm(c.t), c.er);
    CHECK(rel(p.zdiff(), c.zdiff) < 0.001);
    CHECK(p.z_even > p.z_odd);
    CHECK(p.eps_eff_odd < p.eps_eff_even);  // the odd mode has more field in air
  }
  // At the edge of K&J's range (g = s/h = 10, zero thickness) the pair is nearly decoupled: both modes within 2 % of
  // the single H&J line.
  const auto far = imp::coupled_microstrip(mm(0.3), mm(2), mm(0.2), 0, 4.4);
  const double single = imp::microstrip(mm(0.3), mm(0.2), 0, 4.4).z0;
  CHECK(rel(far.z_odd, single) < 0.02);
  CHECK(rel(far.z_even, single) < 0.02);
}

TEST_CASE("coupled stripline: Cohn 1955 exact and against KiCad", "[impedance]") {
  // Exact zero-thickness conformal mapping (scipy): w = s = 0.1 mm, b = 0.5 mm, er 4.3. Tolerance 1e-6.
  const auto p0 = imp::coupled_stripline(mm(0.1), mm(0.1), mm(0.25), mm(0.25), 0, 4.3);
  CHECK(p0.z_even == Approx(91.20899122497444).epsilon(1e-6));
  CHECK(p0.z_odd == Approx(55.089940669175135).epsilon(1e-6));
  // KiCad QA fixture (test_coupled_stripline_offset.cpp, centred): b = 20 mil, w = s = 8 mil, t = 0.7 mil, er 4.3:
  // Z0e 55.34, Z0o 43.59, Zdiff 87.18 (KiCad pins them at 5 %). Ours differs by ~1.2 %; tolerance 2 %.
  const auto p = imp::coupled_stripline(mm(0.2032), mm(0.2032), mm(0.24511), mm(0.24511), mm(0.01778), 4.3);
  CHECK(rel(p.z_even, 55.34) < 0.02);
  CHECK(rel(p.z_odd, 43.59) < 0.02);
  CHECK(rel(p.zdiff(), 87.18) < 0.02);
  // Thin gaps (s < 5t) switch to Cohn's eq. 22; it must join eq. 20 within 2 % at s = 5t.
  const Coord t = um(35);
  const double below = imp::coupled_stripline(mm(0.2), 5 * t - 1, mm(0.2), mm(0.2), t, 4.3).z_odd;
  const double at = imp::coupled_stripline(mm(0.2), 5 * t, mm(0.2), mm(0.2), t, 4.3).z_odd;
  CHECK(rel(below, at) < 0.02);
}

TEST_CASE("grounded coplanar waveguide: exact and against KiCad", "[impedance]") {
  // Exact zero-thickness conformal mapping (scipy): w 0.5, gap 0.3, h 0.8 mm, er 4.4 -> 70.4831 ohm, eps_eff 2.8166.
  const auto l0 = imp::gcpw(mm(0.5), mm(0.3), mm(0.8), 0, 4.4);
  CHECK(l0.z0 == Approx(70.48314701248627).epsilon(1e-6));
  CHECK(l0.eps_eff == Approx(2.8166012444662734).epsilon(1e-6));
  // KiCad with 35 um copper (same Gupta et al. thickness correction): 70.8609; tolerance 3 %.
  CHECK(rel(imp::gcpw(mm(0.5), mm(0.3), mm(0.8), um(35), 4.4).z0, 70.8609) < 0.03);
  // A coplanar ground lowers the impedance below the plain microstrip of the same width.
  CHECK(imp::gcpw(mm(1.0), mm(0.2), mm(1.51), um(35), 4.5).z0 < imp::microstrip(mm(1.0), mm(1.51), um(35), 4.5).z0);
}

TEST_CASE("impedance is monotone where the solver relies on it", "[impedance]") {
  double prev = 1e9;
  for (Coord w = mm(0.05); w <= mm(5); w += mm(0.05)) {
    const double z = imp::microstrip(w, mm(0.2), um(35), 4.4).z0;
    CHECK(z < prev);
    prev = z;
  }
  prev = 1e9;
  for (Coord w = mm(0.05); w <= mm(3); w += mm(0.05)) {
    const double z = imp::coupled_microstrip(w, mm(0.15), mm(0.2), um(35), 4.4).zdiff();
    CHECK(z < prev);
    prev = z;
  }
  prev = 0;
  for (Coord s = mm(0.05); s <= mm(3); s += mm(0.05)) {
    const double z = imp::coupled_microstrip(mm(0.2), s, mm(0.2), um(35), 4.4).zdiff();
    CHECK(z > prev);
    prev = z;
  }
  prev = 0;
  for (Coord s = mm(0.05); s <= mm(2); s += mm(0.05)) {
    const double z = imp::coupled_stripline(mm(0.1), s, mm(0.2), mm(0.2), um(35), 4.4).zdiff();
    CHECK(z > prev);
    prev = z;
  }
  prev = 1e9;
  for (Coord w = mm(0.05); w <= mm(3); w += mm(0.05)) {
    const double z = imp::gcpw(w, mm(0.2), mm(1.51), um(35), 4.5).z0;
    CHECK(z < prev);
    prev = z;
  }
}

TEST_CASE("solving: integer-nm bisection round trips and is deterministic", "[impedance]") {
  auto ms = [](Coord w) { return imp::microstrip(w, mm(0.2104), um(35), 4.4).z0; };
  const auto a = imp::solve_monotone(ms, 50.0, mm(0.1), mm(10), true);
  REQUIRE(a.ok);
  // 1 nm of width moves Z by ~1e-4 ohm here: the round trip is exact to 1e-3 ohm.
  CHECK(std::fabs(ms(a.value) - 50.0) < 1e-3);
  CHECK(std::fabs(ms(a.value - 1) - 50.0) >= std::fabs(ms(a.value) - 50.0));
  CHECK(std::fabs(ms(a.value + 1) - 50.0) >= std::fabs(ms(a.value) - 50.0));
  const auto b = imp::solve_monotone(ms, 50.0, mm(0.1), mm(10), true);
  CHECK(a.value == b.value);
  // Gap for a differential target at a fixed width (increasing in the gap).
  auto cms = [](Coord s) { return imp::coupled_microstrip(mm(0.15), s, mm(0.1), um(35), 4.4).zdiff(); };
  const auto g = imp::solve_monotone(cms, 100.0, mm(0.05), mm(5), false);
  REQUIRE(g.ok);
  CHECK(std::fabs(cms(g.value) - 100.0) < 1e-2);
  // Coupled stripline width.
  auto csl = [](Coord w) { return imp::coupled_stripline(w, mm(0.2), mm(0.2), mm(0.3), um(17.5), 4.2).zdiff(); };
  const auto c = imp::solve_monotone(csl, 90.0, mm(0.05), mm(5), true);
  REQUIRE(c.ok);
  CHECK(std::fabs(csl(c.value) - 90.0) < 1e-2);
  // Out of range: reported, never clamped silently.
  const auto bad = imp::solve_monotone(ms, 500.0, mm(0.1), mm(10), true);
  CHECK(!bad.ok);
  CHECK(!bad.why.empty());
}

TEST_CASE("propagation delay", "[impedance]") {
  CHECK(imp::prop_delay_ps_per_mm(1.0) == Approx(3.33564095).epsilon(1e-8));  // 1 / c
  CHECK(imp::prop_delay_ps_per_mm(4.0) == Approx(6.6712819).epsilon(1e-7));
}

TEST_CASE("IPC-2221 width for current", "[impedance][current]") {
  // IPC-2221B §6.2: I = k dT^0.44 A^0.725. 1 A, dT 10 C, 1 oz (35 um = 1.378 mil):
  //   outer: A = (1 / (0.048 * 10^0.44))^(1/0.725) = 16.30 mil^2 -> 11.83 mil = 0.300 mm
  //   inner: A = (1 / (0.024 * 10^0.44))^(1/0.725) = 42.38 mil^2 -> 30.75 mil = 0.781 mm
  // (hand-evaluated; the usual "12 mil per amp outer, 30 mil inner at 10 C, 1 oz" rule of thumb). Tolerance 0.5 %.
  CHECK(rel(nm_to_mm(imp::width_for_current(1.0, 10.0, um(35), true)), 0.3005) < 0.005);
  CHECK(rel(nm_to_mm(imp::width_for_current(1.0, 10.0, um(35), false)), 0.7811) < 0.005);
  // Doc 15 / USBC-09: 5 A outer, dT 10 C, 1 oz: A = 150.0 mil^2 -> 2.77 mm.
  CHECK(rel(nm_to_mm(imp::width_for_current(5.0, 10.0, um(35), true)), 2.765) < 0.005);
  // Round trip and monotonicity.
  const Coord w = imp::width_for_current(3.0, 20.0, um(70), true);
  CHECK(imp::current_for_width(w, 20.0, um(70), true) >= 3.0);
  CHECK(imp::current_for_width(w - 1, 20.0, um(70), true) < 3.0);
  CHECK(imp::width_for_current(2.0, 10.0, um(35), true) < imp::width_for_current(3.0, 10.0, um(35), true));
  CHECK(imp::width_for_current(0, 10.0, um(35), true) == 0);
}

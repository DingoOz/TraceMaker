#pragma once
// Global placement (design doc 04 §3 A–C).
//
// (A) Quadratic placement with the bound-to-bound (B2B) net model: Spindler, Schlichtmann, Johannes,
//     "Kraftwerk2 — A Fast Force-Directed Quadratic Placement Approach Using an Accurate Net Model",
//     IEEE TCAD 27(8), 2008. Each net connects every pin to the net's two extreme pins with weight
//     w·2/((p−1)·|x_i − x_j|), which reproduces HPWL at the linearisation point; the SPD systems are solved
//     with Jacobi-preconditioned conjugate gradient (Eigen) and re-linearised.
// (B) Spreading: SimPL (Kim, Lee, Markov, "SimPL: An Effective Placement Algorithm", ICCAD 2010 / TCAD
//     2012): alternate the quadratic solve (lower-bound placement) with a rough legalisation (upper-bound
//     placement) and pull the parts towards it with anchor pseudo-nets of growing weight.
// (C) Rotation: coordinate descent; each step is the exact HPWL optimum for one part with the others fixed.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "place/legality.hpp"

namespace tmk::place {

// (A) Solves the B2B quadratic program for the movable parts (rotations taken from `pl`), `relinearise`
// times. Optional anchors pull part i towards anchor[i] with weight anchor_w[i] × (the part's net weight in
// the current linearisation); 0 = no anchor.
void quadratic_place(const Problem& p, Placement& pl, int relinearise, const std::vector<Point>* anchor = nullptr,
                     const std::vector<double>* anchor_w = nullptr);

struct SpreadStats {
  int iterations = 0;
  double overflow_start = 0, overflow_end = 0;  // bin density overflow of the quadratic placement
  std::int64_t hpwl_lower = 0, hpwl_upper = 0;  // weighted HPWL of the last lower/upper placements
  std::vector<std::string> log;
};

// (B) SimPL loop starting from a quadratic placement `pl`; on return `pl` holds the spread (upper-bound)
// placement.
// `trace` (optional) receives the spread placement of every iteration.
SpreadStats spread(const Problem& p, Placement& pl, int max_iterations = 40, double target_overflow = 0.10,
                   std::vector<Placement>* trace = nullptr);

// Bin density overflow of a placement: Σ max(0, demand − capacity) / Σ demand over a bin grid per side.
double density_overflow(const Problem& p, const Placement& pl);

// Movable courtyard area (incl. clearance) / free board area, per side.
std::array<double, 2> utilisation(const Problem& p);

// (C) Rotation coordinate descent over the movable parts (HPWL of each part's nets). Returns the number of
// rotation changes; terminates because every change strictly lowers the weighted HPWL.
int optimise_rotations(const Problem& p, Placement& pl, int max_passes = 20);

// Nets touching each part (sorted, unique).
std::vector<std::vector<int>> part_nets(const Problem& p);

}  // namespace tmk::place

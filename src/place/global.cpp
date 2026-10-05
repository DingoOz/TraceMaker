// SPDX-License-Identifier: GPL-3.0-or-later
#include "place/global.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include <Eigen/IterativeLinearSolvers>
#include <Eigen/Sparse>

#include "place/wirelength.hpp"

namespace tmk::place {

namespace {

constexpr double kMm = 1e6;
constexpr double kEps = 0.05;  // mm: B2B distance floor

double coord_mm(Point q, int axis) { return static_cast<double>(axis == 0 ? q.x : q.y) / kMm; }

// Box centre offset of a part's body at a rotation.
Point body_centre(const Part& pt, int rot) {
  const Box& b = pt.geom[z(rot)].body;
  return Point{(b.x0 + b.x1) / 2, (b.y0 + b.y1) / 2};
}

// Even-odd fill of the board (outline and cut-outs) on a regular grid of sample points (scanline per row).
std::vector<std::uint8_t> inside_samples(const Problem& p, Coord ox, Coord oy, Coord step, int nx, int ny) {
  std::vector<std::uint8_t> m(z(nx) * z(ny), 0);
  if (p.outline.empty()) {
    for (int y = 0; y < ny; ++y)
      for (int x = 0; x < nx; ++x) {
        const Point c{ox + x * step + step / 2, oy + y * step + step / 2};
        m[z(y * nx + x)] = c.x >= p.region.x0 && c.x <= p.region.x1 && c.y >= p.region.y0 && c.y <= p.region.y1;
      }
    return m;
  }
  std::vector<const std::vector<Point>*> loops{&p.outline};
  for (const auto& c : p.cutouts) loops.push_back(&c);
  std::vector<double> xs;
  for (int y = 0; y < ny; ++y) {
    const double yc = static_cast<double>(oy + y * step + step / 2);
    xs.clear();
    for (const auto* l : loops)
      for (std::size_t i = 0, j = l->size() - 1; i < l->size(); j = i++) {
        const Point a = (*l)[j], b = (*l)[i];
        const double ay = static_cast<double>(a.y), by = static_cast<double>(b.y);
        if ((ay > yc) != (by > yc)) xs.push_back(static_cast<double>(a.x) + (yc - ay) * static_cast<double>(b.x - a.x) / (by - ay));
      }
    std::sort(xs.begin(), xs.end());
    for (std::size_t k = 0; k + 1 < xs.size(); k += 2) {
      const double x0 = (xs[k] - static_cast<double>(ox + step / 2)) / static_cast<double>(step);
      const double x1 = (xs[k + 1] - static_cast<double>(ox + step / 2)) / static_cast<double>(step);
      for (int x = std::max(0, static_cast<int>(std::ceil(x0))); x <= std::min(nx - 1, static_cast<int>(std::floor(x1))); ++x)
        m[z(y * nx + x)] = 1;
    }
  }
  return m;
}

// Bin grid with free capacity (nm² as double) per side: inside the board, outside keepouts and fixed parts.
struct Bins {
  Coord ox = 0, oy = 0, bs = 1'000'000;
  int nx = 1, ny = 1;
  std::vector<double> cap[2];

  double& at(int s, int x, int y) { return cap[s][z(y * nx + x)]; }
  double at(int s, int x, int y) const { return cap[s][z(y * nx + x)]; }
};

Bins make_bins(const Problem& p) {
  Bins b;
  double mean_area = 0;
  int nm = 0;
  for (const auto& pt : p.parts)
    if (pt.movable) {
      mean_area += static_cast<double>(pt.area);
      ++nm;
    }
  mean_area = nm ? mean_area / nm : 1e12;
  const Coord w = p.region.x1 - p.region.x0, h = p.region.y1 - p.region.y0;
  b.bs = std::max<Coord>({500'000, static_cast<Coord>(std::sqrt(mean_area)), std::max(w, h) / 96});
  b.ox = p.region.x0;
  b.oy = p.region.y0;
  b.nx = static_cast<int>(std::max<Coord>(1, (w + b.bs - 1) / b.bs));
  b.ny = static_cast<int>(std::max<Coord>(1, (h + b.bs - 1) / b.bs));
  constexpr int kSub = 6;
  const Coord step = std::max<Coord>(1, b.bs / kSub);
  const int sx = b.nx * kSub, sy = b.ny * kSub;
  const auto inside = inside_samples(p, b.ox, b.oy, step, sx, sy);
  const double sample_area = static_cast<double>(step) * static_cast<double>(step);
  for (int s = 0; s < 2; ++s) {
    std::vector<std::uint8_t> freem = inside;
    auto clear_box = [&](const Box& bx, auto&& pred) {
      const int x0 = std::max(0, static_cast<int>((bx.x0 - b.ox) / step)), x1 = std::min(sx - 1, static_cast<int>((bx.x1 - b.ox) / step));
      const int y0 = std::max(0, static_cast<int>((bx.y0 - b.oy) / step)), y1 = std::min(sy - 1, static_cast<int>((bx.y1 - b.oy) / step));
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          const Point c{b.ox + x * step + step / 2, b.oy + y * step + step / 2};
          if (pred(c)) freem[z(y * sx + x)] = 0;
        }
    };
    for (const auto& k : p.keepouts)
      if (k.side[s]) clear_box(k.poly.box, [&](Point c) { return geom::point_in_polygon(c, k.poly.pts); });
    for (const auto& pt : p.parts) {
      if (pt.movable) continue;
      const PartGeom& g = pt.geom[0];
      for (const Shape& cy : g.cy[z(s)]) {
        const Box bx{cy.box.x0 + pt.pos0.x, cy.box.y0 + pt.pos0.y, cy.box.x1 + pt.pos0.x, cy.box.y1 + pt.pos0.y};
        clear_box(bx, [&](Point c) { return geom::point_in_polygon(c - pt.pos0, cy.pts); });
      }
      for (const auto& t : g.through) {
        const Box bx{t.box.x0 + pt.pos0.x, t.box.y0 + pt.pos0.y, t.box.x1 + pt.pos0.x, t.box.y1 + pt.pos0.y};
        clear_box(bx, [](Point) { return true; });
      }
    }
    b.cap[s].assign(z(b.nx) * z(b.ny), 0.0);
    for (int y = 0; y < sy; ++y)
      for (int x = 0; x < sx; ++x)
        if (freem[z(y * sx + x)]) b.at(s, x / kSub, y / kSub) += sample_area;
  }
  return b;
}

// Demand of a part: its body box plus the clearance, as an area spread over the bins it covers.
Box demand_box(const Problem& p, const Part& pt, Point pos, int rot) {
  const Box& bb = pt.geom[z(rot)].body;
  const Coord h = p.clearance / 2;
  return Box{bb.x0 + pos.x - h, bb.y0 + pos.y - h, bb.x1 + pos.x + h, bb.y1 + pos.y + h};
}

double overflow_with(const Problem& p, const Bins& bins, const Placement& pl) {
  double over = 0, total = 0;
  for (int s = 0; s < 2; ++s) {
    std::vector<double> dem(bins.cap[s].size(), 0.0);
    for (std::size_t i = 0; i < p.parts.size(); ++i) {
      const Part& pt = p.parts[i];
      if (!pt.movable || pt.side != s) continue;
      const Box d = demand_box(p, pt, pl.pos[i], pl.rot[i]);
      total += static_cast<double>(d.x1 - d.x0) * static_cast<double>(d.y1 - d.y0);
      const int x0 = static_cast<int>(std::floor(static_cast<double>(d.x0 - bins.ox) / static_cast<double>(bins.bs)));
      const int x1 = static_cast<int>(std::floor(static_cast<double>(d.x1 - bins.ox) / static_cast<double>(bins.bs)));
      const int y0 = static_cast<int>(std::floor(static_cast<double>(d.y0 - bins.oy) / static_cast<double>(bins.bs)));
      const int y1 = static_cast<int>(std::floor(static_cast<double>(d.y1 - bins.oy) / static_cast<double>(bins.bs)));
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          const Coord bx0 = bins.ox + x * bins.bs, by0 = bins.oy + y * bins.bs;
          const double ox = static_cast<double>(std::min(d.x1, bx0 + bins.bs) - std::max(d.x0, bx0));
          const double oy = static_cast<double>(std::min(d.y1, by0 + bins.bs) - std::max(d.y0, by0));
          if (ox <= 0 || oy <= 0) continue;
          if (x < 0 || y < 0 || x >= bins.nx || y >= bins.ny) {
            over += ox * oy;  // outside the board region: all overflow
            continue;
          }
          dem[z(y * bins.nx + x)] += ox * oy;
        }
    }
    for (std::size_t k = 0; k < dem.size(); ++k) over += std::max(0.0, dem[k] - bins.cap[s][k]);
  }
  return total > 0 ? over / total : 0.0;
}

// SimPL-style rough legalisation by recursive bisection: split the parts at their area median along the
// longer side of the region, split the region where its free capacity is in the same proportion, recurse until
// one part per region, then clamp each part into its region. Relative order is preserved on every cut.
struct Spreader {
  const Problem& p;
  const Bins& bins;
  int side;
  Placement& pl;
  std::vector<Point> centre;  // body centre per part (nm)

  double demand(int i) const { return static_cast<double>(p.parts[z(i)].area); }

  void run() {
    std::vector<int> ids;
    for (std::size_t i = 0; i < p.parts.size(); ++i)
      if (p.parts[i].movable && p.parts[i].side == side) ids.push_back(static_cast<int>(i));
    centre.assign(p.parts.size(), Point{});
    for (int i : ids) centre[z(i)] = pl.pos[z(i)] + body_centre(p.parts[z(i)], pl.rot[z(i)]);
    split(0, 0, bins.nx, bins.ny, ids);
  }

  void place_leaf(int bx0, int by0, int bx1, int by1, const std::vector<int>& ids) {
    const Coord x0 = bins.ox + bx0 * bins.bs, x1 = bins.ox + bx1 * bins.bs;
    const Coord y0 = bins.oy + by0 * bins.bs, y1 = bins.oy + by1 * bins.bs;
    for (int i : ids) {
      const Part& pt = p.parts[z(i)];
      const Box& b = pt.geom[pl.rot[z(i)]].body;
      const Coord hw = (b.x1 - b.x0) / 2, hh = (b.y1 - b.y0) / 2;
      Point c = centre[z(i)];
      c.x = (x1 - x0 >= 2 * hw) ? std::clamp(c.x, x0 + hw, x1 - hw) : (x0 + x1) / 2;
      c.y = (y1 - y0 >= 2 * hh) ? std::clamp(c.y, y0 + hh, y1 - hh) : (y0 + y1) / 2;
      pl.pos[z(i)] = c - body_centre(pt, pl.rot[z(i)]);
    }
  }

  void split(int bx0, int by0, int bx1, int by1, std::vector<int> ids) {
    if (ids.empty()) return;
    const int w = bx1 - bx0, h = by1 - by0;
    if (ids.size() == 1 || (w == 1 && h == 1)) {
      place_leaf(bx0, by0, bx1, by1, ids);
      return;
    }
    int axis = w >= h ? 0 : 1;
    if ((axis == 0 && w == 1) || (axis == 1 && h == 1)) axis = 1 - axis;
    std::sort(ids.begin(), ids.end(), [&](int a, int b) {
      const Coord ca = axis == 0 ? centre[z(a)].x : centre[z(a)].y, cb = axis == 0 ? centre[z(b)].x : centre[z(b)].y;
      return ca != cb ? ca < cb : a < b;
    });
    double total = 0;
    for (int i : ids) total += demand(i);
    std::size_t nl = 0;
    double left = 0;
    while (nl < ids.size() && (left + demand(ids[nl])) * 2 <= total) left += demand(ids[nl++]);
    if (nl < ids.size() && left * 2 < total && nl + 1 < ids.size() && (left + demand(ids[nl])) * 2 - total < total - left * 2)
      left += demand(ids[nl++]);
    nl = std::clamp<std::size_t>(nl, 1, ids.size() - 1);
    left = 0;
    for (std::size_t k = 0; k < nl; ++k) left += demand(ids[k]);
    const double f = total > 0 ? left / total : 0.5;
    // Capacity per column (or row) of the region.
    const int len = axis == 0 ? w : h;
    std::vector<double> col(z(len), 0.0);
    for (int y = by0; y < by1; ++y)
      for (int x = bx0; x < bx1; ++x) col[z(axis == 0 ? x - bx0 : y - by0)] += bins.at(side, x, y);
    const double ctot = std::accumulate(col.begin(), col.end(), 0.0);
    int cut = 1;
    if (ctot <= 0) {
      cut = std::clamp(static_cast<int>(std::lround(f * len)), 1, len - 1);
    } else {
      double acc = 0, best = 1e300;
      for (int c = 1; c < len; ++c) {
        acc += col[z(c - 1)];
        const double d = std::fabs(acc / ctot - f);
        if (d < best) {
          best = d;
          cut = c;
        }
      }
    }
    std::vector<int> a(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(nl)), b(ids.begin() + static_cast<std::ptrdiff_t>(nl), ids.end());
    if (axis == 0) {
      split(bx0, by0, bx0 + cut, by1, std::move(a));
      split(bx0 + cut, by0, bx1, by1, std::move(b));
    } else {
      split(bx0, by0, bx1, by0 + cut, std::move(a));
      split(bx0, by0 + cut, bx1, by1, std::move(b));
    }
  }
};

}  // namespace

void quadratic_place(const Problem& p, Placement& pl, int relinearise, const std::vector<Point>* anchor,
                     const std::vector<double>* anchor_w) {
  std::vector<int> var(p.parts.size(), -1);
  int n = 0;
  for (std::size_t i = 0; i < p.parts.size(); ++i)
    if (p.parts[i].movable) var[i] = n++;
  if (n == 0) return;
  const double cx = static_cast<double>(p.region.x0 + p.region.x1) / 2 / kMm;
  const double cy = static_cast<double>(p.region.y0 + p.region.y1) / 2 / kMm;
  constexpr double kReg = 1e-4;  // weak pull to the board centre: makes the system strictly convex
  using SpMat = Eigen::SparseMatrix<double>;
  std::vector<Eigen::Triplet<double>> trip;
  for (int it = 0; it < relinearise; ++it) {
    for (int axis = 0; axis < 2; ++axis) {
      trip.clear();
      Eigen::VectorXd rhs = Eigen::VectorXd::Zero(n), x0(n);
      for (std::size_t i = 0; i < p.parts.size(); ++i)
        if (var[i] >= 0) x0(var[i]) = coord_mm(pl.pos[i], axis);
      auto edge = [&](int k, int l, double w) {
        const Pin& a = p.pins[z(k)];
        const Pin& b = p.pins[z(l)];
        if (a.part == b.part) return;
        const int i = var[z(a.part)], j = var[z(b.part)];
        const double ok = coord_mm(a.off[pl.rot[z(a.part)]], axis), ol = coord_mm(b.off[pl.rot[z(b.part)]], axis);
        if (i >= 0 && j >= 0) {
          trip.emplace_back(i, i, w);
          trip.emplace_back(j, j, w);
          trip.emplace_back(i, j, -w);
          trip.emplace_back(j, i, -w);
          rhs(i) += w * (ol - ok);
          rhs(j) += w * (ok - ol);
        } else if (i >= 0) {
          trip.emplace_back(i, i, w);
          rhs(i) += w * (coord_mm(pl.pin(p, l), axis) - ok);
        } else if (j >= 0) {
          trip.emplace_back(j, j, w);
          rhs(j) += w * (coord_mm(pl.pin(p, k), axis) - ol);
        }
      };
      for (const auto& net : p.nets) {
        const std::size_t np = net.pins.size();
        if (np < 2) continue;
        std::size_t kmin = 0, kmax = 0;
        std::vector<double> c(np);
        for (std::size_t k = 0; k < np; ++k) {
          c[k] = coord_mm(pl.pin(p, net.pins[k]), axis);
          if (c[k] < c[kmin]) kmin = k;
          if (c[k] > c[kmax]) kmax = k;
        }
        if (kmin == kmax) kmax = kmin == 0 ? 1 : 0;
        const double f = 2.0 / static_cast<double>(np - 1) * net.weight / static_cast<double>(kSignalWeight);
        edge(net.pins[kmin], net.pins[kmax], f / std::max(std::fabs(c[kmax] - c[kmin]), kEps));
        for (std::size_t k = 0; k < np; ++k) {
          if (k == kmin || k == kmax) continue;
          edge(net.pins[k], net.pins[kmin], f / std::max(std::fabs(c[k] - c[kmin]), kEps));
          edge(net.pins[k], net.pins[kmax], f / std::max(std::fabs(c[k] - c[kmax]), kEps));
        }
      }
      for (std::size_t i = 0; i < p.parts.size(); ++i) {
        const int v = var[i];
        if (v < 0) continue;
        trip.emplace_back(v, v, kReg);
        rhs(v) += kReg * (axis == 0 ? cx : cy);
      }
      if (anchor && anchor_w) {
        // Anchor pseudo-nets (SimPL): weight = factor × the part's own net weight in this linearisation, so a
        // factor of 1 pulls a part half way from its net optimum to its anchor whatever its connectivity.
        std::vector<double> diag(z(n), 0.0);
        for (const auto& tr : trip)
          if (tr.row() == tr.col()) diag[z(tr.row())] += tr.value();
        for (std::size_t i = 0; i < p.parts.size(); ++i) {
          const int v = var[i];
          if (v < 0 || (*anchor_w)[i] <= 0) continue;
          const double t = coord_mm((*anchor)[i], axis);
          const double w = (*anchor_w)[i] * diag[z(v)];
          trip.emplace_back(v, v, w);
          rhs(v) += w * t;
        }
      }
      SpMat A(n, n);
      A.setFromTriplets(trip.begin(), trip.end());
      Eigen::ConjugateGradient<SpMat, Eigen::Lower | Eigen::Upper, Eigen::DiagonalPreconditioner<double>> cg;
      cg.setTolerance(1e-10);
      cg.setMaxIterations(std::max(200, 4 * n));
      cg.compute(A);
      const Eigen::VectorXd x = cg.solveWithGuess(rhs, x0);
      for (std::size_t i = 0; i < p.parts.size(); ++i) {
        if (var[i] < 0) continue;
        const Coord v = geom::kiround(x(var[i]) * kMm);
        (axis == 0 ? pl.pos[i].x : pl.pos[i].y) = v;
      }
    }
  }
}

double density_overflow(const Problem& p, const Placement& pl) { return overflow_with(p, make_bins(p), pl); }

std::array<double, 2> utilisation(const Problem& p) {
  const Bins b = make_bins(p);
  std::array<double, 2> u{0, 0};
  for (int s = 0; s < 2; ++s) {
    double dem = 0, cap = 0;
    for (const auto& pt : p.parts)
      if (pt.movable && pt.side == s) dem += static_cast<double>(pt.area);
    for (double c : b.cap[s]) cap += c;
    u[z(s)] = cap > 0 ? dem / cap : (dem > 0 ? 1e9 : 0.0);
  }
  return u;
}

SpreadStats spread(const Problem& p, Placement& pl, int max_iterations, double target_overflow, std::vector<Placement>* trace) {
  SpreadStats st;
  const Bins bins = make_bins(p);
  Placement x = pl;
  st.overflow_start = overflow_with(p, bins, x);
  Placement u = x;
  std::vector<double> aw(p.parts.size(), 0.0);
  for (int k = 1; k <= max_iterations; ++k) {
    u = x;
    for (int s = 0; s < 2; ++s) {
      Spreader sp{p, bins, s, u, {}};
      sp.run();
    }
    st.iterations = k;
    if (trace) trace->push_back(u);
    st.overflow_end = overflow_with(p, bins, x);
    st.hpwl_lower = weighted_hpwl(p, x);
    st.hpwl_upper = weighted_hpwl(p, u);
    st.log.push_back("iter " + std::to_string(k) + ": overflow " + std::to_string(st.overflow_end) + ", HPWL lower " +
                     std::to_string(nm_to_mm(st.hpwl_lower / kSignalWeight)) + " mm, upper " +
                     std::to_string(nm_to_mm(st.hpwl_upper / kSignalWeight)) + " mm");
    if (st.overflow_end <= target_overflow) break;
    const double alpha = 0.1 * k;
    for (std::size_t i = 0; i < p.parts.size(); ++i) aw[i] = p.parts[i].movable ? alpha : 0.0;
    quadratic_place(p, x, 2, &u.pos, &aw);
  }
  pl = u;
  return st;
}

std::vector<std::vector<int>> part_nets(const Problem& p) {
  std::vector<std::vector<int>> out(p.parts.size());
  for (std::size_t i = 0; i < p.parts.size(); ++i) {
    for (int pi : p.parts[i].pins) out[i].push_back(p.pins[z(pi)].net);
    std::sort(out[i].begin(), out[i].end());
    out[i].erase(std::unique(out[i].begin(), out[i].end()), out[i].end());
  }
  return out;
}

int optimise_rotations(const Problem& p, Placement& pl, int max_passes) {
  const auto pn = part_nets(p);
  int changes = 0;
  for (int pass = 0; pass < max_passes; ++pass) {
    int pass_changes = 0;
    for (std::size_t i = 0; i < p.parts.size(); ++i) {
      if (!p.parts[i].movable || pn[i].empty()) continue;
      const std::uint8_t r0 = pl.rot[i];
      std::int64_t best = INT64_MAX;
      std::uint8_t best_r = r0;
      for (std::uint8_t r : {r0, with_turn(r0, r0 + 1), with_turn(r0, r0 + 2), with_turn(r0, r0 + 3)}) {
        // Rotate about the body centre so the part stays where the spreading put it.
        const Point c = pl.pos[i] + body_centre(p.parts[i], r0);
        const Point save = pl.pos[i];
        pl.rot[i] = r;
        pl.pos[i] = c - body_centre(p.parts[i], r);
        std::int64_t cost = 0;
        for (int n : pn[i]) cost += p.nets[z(n)].weight * net_hpwl(p, pl, n);
        pl.pos[i] = save;
        if (cost < best) {  // strict: ties keep the current rotation (listed first)
          best = cost;
          best_r = r;
        }
      }
      pl.rot[i] = r0;
      if (best_r != r0) {
        const Point c = pl.pos[i] + body_centre(p.parts[i], r0);
        pl.rot[i] = best_r;
        pl.pos[i] = c - body_centre(p.parts[i], best_r);
        ++pass_changes;
      }
    }
    changes += pass_changes;
    if (pass_changes == 0) break;
  }
  return changes;
}

}  // namespace tmk::place

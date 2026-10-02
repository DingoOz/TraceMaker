#pragma once
// Integer-nanometre points and KiCad-compatible rotation.
#include <cmath>
#include <cstdint>
#include <numbers>

#include "core/hd.hpp"
#include "core/units.hpp"

namespace tmk::geom {

struct Point {
  Coord x = 0, y = 0;
  TM_HD friend constexpr Point operator+(Point a, Point b) { return {a.x + b.x, a.y + b.y}; }
  TM_HD friend constexpr Point operator-(Point a, Point b) { return {a.x - b.x, a.y - b.y}; }
  TM_HD friend constexpr bool operator==(Point a, Point b) { return a.x == b.x && a.y == b.y; }
};

struct Box {
  Coord x0 = 0, y0 = 0, x1 = -1, y1 = -1;  // empty when x1 < x0
  constexpr bool empty() const { return x1 < x0 || y1 < y0; }
  constexpr void add(Point p) {
    if (empty()) { x0 = x1 = p.x; y0 = y1 = p.y; return; }
    if (p.x < x0) x0 = p.x;
    if (p.x > x1) x1 = p.x;
    if (p.y < y0) y0 = p.y;
    if (p.y > y1) y1 = p.y;
  }
  constexpr void add(const Box& b) {
    if (b.empty()) return;
    add(Point{b.x0, b.y0});
    add(Point{b.x1, b.y1});
  }
  constexpr Box inflated(Coord d) const { return empty() ? *this : Box{x0 - d, y0 - d, x1 + d, y1 + d}; }
  constexpr bool intersects(const Box& b) const { return !(b.x0 > x1 || b.x1 < x0 || b.y0 > y1 || b.y1 < y0); }
};

// Rounds half away from zero, like KiCad's KiROUND.
inline Coord kiround(double v) { return static_cast<Coord>(v < 0 ? v - 0.5 : v + 0.5); }

// Normalises degrees to [0, 360).
inline double norm_deg(double a) {
  a = std::fmod(a, 360.0);
  if (a < 0) a += 360.0;
  if (a >= 360.0) a -= 360.0;
  return a;
}

// Rotates p by `deg` degrees the way KiCad does: positive angles turn counter-clockwise on screen in KiCad's
// y-down coordinates. Multiples of 90° are exact.
inline Point rotate(Point p, double deg) {
  const double a = norm_deg(deg);
  if (a == 0.0) return p;
  if (a == 90.0) return {p.y, -p.x};
  if (a == 180.0) return {-p.x, -p.y};
  if (a == 270.0) return {-p.y, p.x};
  const double r = a * std::numbers::pi / 180.0;
  const double c = std::cos(r), s = std::sin(r);
  const double x = static_cast<double>(p.x), y = static_cast<double>(p.y);
  return {kiround(x * c + y * s), kiround(-x * s + y * c)};
}

}  // namespace tmk::geom

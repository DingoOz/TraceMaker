#pragma once
// Placement legality (design doc 04 §3 D/E): exact integer tests of courtyards, through-hole obstacles, the
// board outline, keepouts and pad-to-edge clearance, plus a bucket index of placed parts and a conservative
// occupancy raster used as a fast path by the legaliser.
#include <cstdint>
#include <vector>

#include "place/problem.hpp"

namespace tmk::place {

struct Placement {
  std::vector<Point> pos;           // footprint origin per part
  std::vector<std::uint8_t> rot;    // rotation index per part (angle0 + 90° * rot)

  static Placement initial(const Problem& p);  // every part where the board has it
  Point pin(const Problem& p, int pin_index) const {
    const Pin& q = p.pins[z(pin_index)];
    return pos[z(q.part)] + q.off[rot[z(q.part)]];
  }
};

// Exact test with translation (no copies): same semantics as geom::closer_than(a + da, b + db, clearance).
bool closer(const Shape& a, Point da, const Shape& b, Point db, Coord clearance);
// Copper clearance violation between two copper shapes (different nets, shared layer, max of the two needs).
bool copper_conflict(const CopperShape& a, Point pa, const CopperShape& b, Point pb);

inline constexpr Coord kThroughMargin = 100'000;   // through obstacle vs courtyard on the other side
inline constexpr Coord kThroughThrough = 250'000;  // through obstacle vs through obstacle

class Legality {
 public:
  explicit Legality(const Problem& p);

  // Board test (outline, cut-outs, keepouts, pad-to-edge clearance); independent of other parts.
  // `lenient`: pads only need to be inside the outline, without the copper-to-edge clearance.
  bool inside_ok(int part, Point pos, int rot, bool lenient = false) const;
  // Exact test of two parts.
  bool pair_conflict(int a, Point pa, int ra, int b, Point pb, int rb) const;

  // Index of placed parts (all parts present after reset).
  void reset(const Placement& pl);
  void clear();
  void insert(int part, Point pos, int rot);
  void remove(int part);
  bool present(int part) const { return present_[z(part)] != 0; }
  // First placed part that conflicts with `part` at (pos, rot), skipping `part` itself and skip1/skip2; -1 if none.
  int find_conflict(int part, Point pos, int rot, int skip1 = -1, int skip2 = -1) const;
  // Every placed part that conflicts (sorted).
  void conflicts(int part, Point pos, int rot, std::vector<int>& out) const;
  bool legal(int part, Point pos, int rot, int skip1 = -1, int skip2 = -1) const {
    return inside_ok(part, pos, rot) && find_conflict(part, pos, rot, skip1, skip2) < 0;
  }
  // Placed parts whose bodies are within `margin` of the box.
  void neighbours(const Box& box, std::vector<int>& out) const;

 private:
  void cells_of(const Box& b, int& cx0, int& cy0, int& cx1, int& cy1) const;
  const Problem& p_;
  Coord cell_ = 2'000'000;
  Coord ox_ = 0, oy_ = 0;
  int nx_ = 1, ny_ = 1;
  std::vector<std::vector<int>> cells_;
  std::vector<Point> pos_;
  std::vector<std::uint8_t> rot_, present_;
  mutable std::vector<std::uint32_t> stamp_;
  mutable std::uint32_t epoch_ = 0;
  // Board edge segments in a bucket grid.
  std::vector<Shape> segs_;
  std::vector<std::vector<int>> seg_cells_;
  std::vector<std::vector<int>> fixed_cells_;  // Problem::fixed_copper in the same grid
  Coord reach_ = 0;                            // neighbour search inflation (largest clearance of any kind)
};

// Conservative occupancy raster per side (cell 0.05–0.1 mm). `free()` returning true proves that the part is
// legal against the board and every part in the raster; false means "maybe illegal" (run the exact test).
class Raster {
 public:
  Raster(const Problem& p, Coord cell);
  void add(int part, Point pos, int rot, int delta);  // delta = +1 insert, -1 remove
  bool free(int part, Point pos, int rot) const;            // summed-area tables: O(shapes) per call
  bool free_reference(int part, Point pos, int rot) const;  // cell scan (reference path for tests)
  Coord cell() const { return h_; }
  // Fraction of cells inside the board on side s (for diagnostics).
  double inside_fraction(int side) const;

 private:
  bool range(const Box& b, int& x0, int& y0, int& x1, int& y1) const;
  bool any(const std::vector<std::uint16_t>& g, const Box& b) const;
  bool any_sat(const std::vector<std::int32_t>& sat, const Box& b) const;
  void build_sat(const std::vector<std::uint16_t>& g, std::vector<std::int32_t>& sat) const;
  bool free_impl(int part, Point pos, int rot, bool sat) const;
  void bump(std::vector<std::uint16_t>& g, const Box& b, int delta);
  const Problem& p_;
  Coord h_;
  Coord ox_ = 0, oy_ = 0;
  int nx_ = 0, ny_ = 0;
  std::vector<std::uint16_t> blocked_[2];  // static: not entirely inside the board
  std::vector<std::uint16_t> keep_[2];     // static: touched by a footprint keepout on that side
  std::vector<std::uint16_t> occ_[2];      // courtyard and through-obstacle boxes of placed parts
  std::vector<std::uint16_t> fixed_[2];    // static: fixed board copper boxes
  std::vector<std::int32_t> sat_blocked_[2], sat_keep_[2], sat_fixed_[2];  // summed-area tables (cell != 0)
  mutable std::vector<std::int32_t> sat_occ_[2];           // rebuilt lazily after add()
  mutable bool occ_dirty_ = true;
};

// Every conflict in a placement (for the final report and tests): movable-involved pairs and parts outside.
struct Violations {
  int overlaps = 0;          // conflicting pairs with at least one movable part
  int fixed_overlaps = 0;    // pairs of fixed parts (pre-existing, not ours)
  int outside = 0;           // movable parts failing inside_ok
  int fixed_outside = 0;
  std::vector<std::pair<int, int>> pairs;
  std::vector<int> outside_parts;
};
Violations check_all(const Problem& p, const Placement& pl);

}  // namespace tmk::place

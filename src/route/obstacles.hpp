#pragma once
// Obstacle model for routing: every copper item, hole, board edge and keepout, with exact legality tests that
// use the DRC's rule engine, so the router and the DRC can never disagree about what is legal.
#include <memory>
#include <vector>

#include "drc/copper.hpp"
#include "drc/rule_engine.hpp"
#include "index/uniform_grid.hpp"
#include "model/board.hpp"
#include "model/rules.hpp"

namespace tmk::route {

class Obstacles {
 public:
  // `board` is the router's working copy; items added later must also be appended to it.
  Obstacles(model::Board& board, const model::DesignRules& rules);

  // A track of `net` with half-width `hw` centred at p on `layer`: legal against all foreign copper, holes,
  // edges and keepouts with `margin` extra distance?
  bool disk_ok(geom::Point p, int layer, Coord hw, model::NetId net, Coord margin) const;
  // A straight track segment (exact final check).
  bool segment_ok(geom::Point a, geom::Point b, int layer, Coord width, model::NetId net) const;
  // A through via of diameter d / drill at p.
  bool via_ok(geom::Point p, Coord d, Coord drill, model::NetId net, Coord margin) const;

  // Registers committed copper (the board copy must already contain the track/via at `index`).
  void add_track(int index);
  void add_via(int index);

  const drc::CopperModel& copper() const { return cm_; }
  const drc::RuleEngine& rules() const { return *re_; }
  index::UniformGrid& grid() { return *grid_; }
  geom::Box bounds() const { return bounds_; }
  // Board outline polygon (largest Edge.Cuts loop) when it could be assembled; empty otherwise.
  const std::vector<geom::Point>& outline() const { return outline_; }
  bool inside_board(geom::Point p, Coord margin) const;
  // Rejection counters (diagnostics): outside board, copper, holes/edges/keepouts.
  mutable long rej_outside = 0, rej_copper = 0, rej_other = 0, checks = 0;

 private:
  bool copper_ok(const geom::Shape& s, const drc::CopperItem& probe, int layer) const;
  bool holes_edges_ok(const geom::Shape& s, model::NetId net, int layer, bool is_via_hole, Coord hole_r) const;

  model::Board& b_;
  const model::DesignRules& r_;
  drc::CopperModel cm_;
  std::unique_ptr<drc::RuleEngine> re_;
  std::unique_ptr<index::UniformGrid> grid_;   // copper items
  std::unique_ptr<index::UniformGrid> hgrid_;  // holes
  std::unique_ptr<index::UniformGrid> egrid_;  // board-edge segments
  std::vector<geom::Shape> edge_segs_;
  geom::Box bounds_;
  Coord reach_ = 0;
  std::vector<geom::Point> outline_;
  std::vector<std::pair<geom::Shape, const model::Zone*>> keepouts_;
};

}  // namespace tmk::route

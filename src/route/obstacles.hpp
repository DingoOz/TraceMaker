// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Obstacle model for routing: every copper item, hole, board edge and keepout, with exact legality tests that
// use the DRC's rule engine, so the router and the DRC can never disagree about what is legal.
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "drc/connectivity.hpp"
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

  // Legality of a probe shape. Result: 0 = free, 1 = only conflicts with rippable routed copper (owners are
  // appended to `owners` when given), 2 = blocked by fixed copper, holes, edges or keepouts.
  // With `ignore_routed` = false, routed copper counts as blocking (result 0 or 2 only).
  int disk_state(geom::Point p, int layer, Coord hw, model::NetId net, Coord margin, bool ignore_routed,
                 std::vector<int>* owners = nullptr) const;
  int segment_state(geom::Point a, geom::Point b, int layer, Coord width, model::NetId net, bool ignore_routed,
                    std::vector<int>* owners = nullptr) const;
  // A via on copper layers [l0, l1] only (blind or buried via, or a micro via with `type`); via_state covers all layers.
  int via_state_span(geom::Point p, Coord d, Coord drill, model::NetId net, Coord margin, bool ignore_routed, std::vector<int>* owners, int l0,
                     int l1, model::ViaType type = model::ViaType::Blind) const;
  int via_state(geom::Point p, Coord d, Coord drill, model::NetId net, Coord margin, bool ignore_routed,
                std::vector<int>* owners = nullptr) const;
  bool disk_ok(geom::Point p, int layer, Coord hw, model::NetId net, Coord margin) const {
    return disk_state(p, layer, hw, net, margin, false) == 0;
  }
  bool segment_ok(geom::Point a, geom::Point b, int layer, Coord width, model::NetId net) const {
    return segment_state(a, b, layer, width, net, false) == 0;
  }
  bool via_ok(geom::Point p, Coord d, Coord drill, model::NetId net, Coord margin) const {
    return via_state(p, d, drill, net, margin, false) == 0;
  }
  // Custom disallow rules that depend on position, size or footprint (doc 05 §34), for new copper of `net`: a
  // track whose copper is `s` with `width` on `layer`, or a via of diameter `d` spanning [l0, l1]. The state
  // checks above include them (blocked, 2); a disk's rules are judged on its copper, without the margin.
  bool positional_rules() const { return re_->positional_disallow(); }
  bool track_disallowed(const geom::Shape& s, int layer, Coord width, model::NetId net) const;
  bool via_disallowed(geom::Point p, Coord d, model::NetId net, int l0, int l1, model::ViaType type) const;

  // Registers committed copper (the board copy must already contain the track/via at `index`). Returns the
  // copper item index.
  int add_track(int index, int owner);
  int add_via(int index, int owner);
  // Rips up a routed item (by copper item index).
  void remove_item(int item);

  // Fixed-obstacle code of a disk (fixed copper, fixed holes, edges, keepouts, outline), independent of the
  // probe's own net: kFree, kBlocked, or the single net id whose own copper is the only conflict (so the disk
  // is legal for that net only). Clearances use `probe_net`'s class; diff-pair relief is not applied (safe).
  static constexpr std::int32_t kFree = -1, kBlocked = -2;
  std::int32_t fixed_code(geom::Point p, int layer, Coord hw, Coord margin, model::NetId probe_net, bool via_probe = false) const;
  std::int32_t fixed_via_code(geom::Point p, Coord d, Coord drill, Coord margin, model::NetId probe_net) const;
  // Per-layer reference for fixed_via_code.
  std::int32_t fixed_via_code_reference(geom::Point p, Coord d, Coord drill, Coord margin, model::NetId probe_net) const;
  // Routed (rippable) copper only: 0 free, 1 conflicts (owners appended), 2 blocked when !soft.
  int routed_state(const geom::Shape& s, int layer, model::NetId net, drc::ItemKind kind, bool soft, std::vector<int>* owners,
                   bool via_hole = false, Coord hole_r = 0) const;
  // Maximum routed_state over `layers` for a via and its hole; layer-independent hole checks run once.
  int routed_via_state(const geom::Shape& s, model::LayerMask layers, model::NetId net, bool soft, Coord hole_r) const;
  // KiCad's diff-pair coupling for a new track of a pair net with a bounded gap (doc 05 §59): 0, 1 (a routed partner track; owners
  // appended) or 2. Part of segment_state; the search also asks it for each step of a net that coupling_net() names.
  int coupling_state(geom::Point a, geom::Point b, int layer, Coord width, model::NetId net, bool ignore_routed, std::vector<int>* owners) const;
  bool coupling_net(model::NetId net) const {
    return coupling_on_ && net > 0 && static_cast<std::size_t>(net) < net_tracks_.size() && re_->dp_partner(net) > 0;
  }
  // Rules outside the per-class cache require exact per-point checks.
  bool needs_exact_routing() const { return re_->needs_exact_routing(); }
  // Ids of live routed items whose box meets `box`.
  void routed_items_in(const geom::Box& box, std::vector<int>& out) const;

  const drc::CopperModel& copper() const { return cm_; }
  // Copper text as the model holds it: one estimated rectangle per line (copper index, rectangle).
  const std::vector<std::pair<int, geom::Shape>>& copper_texts() const { return texts_; }
  const drc::RuleEngine& rules() const { return *re_; }
  index::UniformGrid& grid() { return *grid_; }
  geom::Box bounds() const { return bounds_; }
  // Board outline polygon (largest Edge.Cuts loop) when it could be assembled; empty otherwise.
  const std::vector<geom::Point>& outline() const { return outline_; }
  bool inside_board(geom::Point p, Coord margin) const;
  // Exact inside test against the outline and cut-outs: by the edges of the point's raster row, and the reference that
  // tests every edge (the two must agree; test "[geom][route]" inside board).
  bool inside_exact(geom::Point p) const;
  bool inside_exact_linear(geom::Point p) const;
  // Solder-mask expansion the checks give untented vias (0 when the board tents them). Escape analysis sets it
  // to 0 to ask "would a via fit here if vias were tented?"; routing never changes it.
  Coord via_mask() const { return via_mask_; }
  void set_via_mask(Coord m) { via_mask_ = m; }
  // Zone fills are ignored as obstacles (they are refilled after routing); they remain connection targets.
  void set_soft_zones(bool on) { soft_zones_ = on; }
  // Net-less solder-mask openings drawn as footprint graphics (a solder-jumper's bridge, a MEMS microphone's pad group) no longer keep copper out; KiCad's
  // mask checks flag what then lies in them (judgement, not a short). Off by default.
  void set_soft_mask_graphics(bool on) { soft_mask_graphics_ = on; }
  // Net-less copper drawn as BOARD graphics (an EasyEDA import's pours as polygons on a copper layer) is fixed copper that no track may touch, and pins inside it are
  // enclosed; soft, it is ignored like a zone fill (KiCad then flags what touches it). Off by default.
  void set_soft_copper_graphics(bool on) { soft_copper_graphics_ = on; }  // (unused since the net inference of board copper graphics: Board::infer_graphic_nets)
  bool soft_zones() const { return soft_zones_; }
  // Vias keep clear of SMD pads narrower than `max_pad` (any net, net-class clearance), as if they were another
  // net's copper; larger pads (exposed thermal pads) still take vias. set_pad_via_exempt(true) lifts this around the
  // checks of one deliberate via in pad (the router's --via-in-pad).
  void set_vias_off_pads(bool on, Coord max_pad) { vias_off_pads_ = on; vias_off_pads_max_ = max_pad; }
  void set_pad_via_exempt(bool on) const { pad_via_exempt_ = on; }
  // Reference path: test probes against every edge of a zone fill instead of the fill's edge index (the answers
  // are the same; tests/test_design_rules.cpp).
  void set_linear_zone_tests(bool on) { linear_zone_tests_ = on; }
  // Rejection counters (diagnostics): outside board, copper, holes/edges/keepouts.
  mutable long rej_outside = 0, rej_copper = 0, rej_other = 0, checks = 0;

 private:
  int copper_state(const geom::Shape& s, const drc::CopperItem& probe, int layer, bool ignore_routed, std::vector<int>* owners) const;
  // True if `s` is closer than `req` to the copper of `it`. A zone fill answers from its edge index
  // (geom::PolygonIndex, exact), so a probe costs the fill's nearby edges instead of all of them.
  bool item_closer(const geom::Shape& s, const drc::CopperItem& it, Coord req) const;
  int holes_edges_state(const geom::Shape& s, model::NetId net, int layer, bool is_via_hole, Coord hole_r, bool ignore_routed,
                        std::vector<int>* owners) const;
  // Custom physical_hole_clearance rules, any net: new copper against existing holes, and a new via's hole
  // against existing copper, on `layer`. 0 free, 1 conflict with routed copper (when soft), 2 blocked.
  int physical_copper_state(const geom::Shape& s, drc::ItemKind kind, model::NetId net, int layer, bool fixed, bool routed, bool soft,
                            std::vector<int>* owners) const;
  int physical_hole_state(const geom::Shape& hole, model::NetId net, int layer, bool fixed, bool routed, bool soft, std::vector<int>* owners) const;
  int routed_copper_part(const geom::Shape& s, int layer, model::NetId net, drc::ItemKind kind, bool soft, std::vector<int>* owners) const;
  int routed_via_holes_part(const geom::Shape& s, model::NetId net, Coord hc, bool soft, std::vector<int>* owners) const;
  int routed_hole_copper_part(const geom::Shape& h, int layer, model::NetId net, Coord hc, bool soft, std::vector<int>* owners) const;
  int routed_hole_to_hole_part(const geom::Shape& hole, bool soft, std::vector<int>* owners) const;
  std::int32_t via_hole_code(geom::Point p, Coord drill, Coord margin, model::NetId probe_net, std::int32_t code) const;
  void setup_coupling();
  struct GapBounds {
    std::optional<Coord> min, max;
    bool set = false;
  };
  bool coupling_on_ = false;               // some pair net has a diff_pair_gap rule with a minimum or maximum
  std::vector<GapBounds> gap_bounds_;      // by net * copper count + layer
  std::vector<std::vector<int>> net_tracks_;  // copper item ids of each net's tracks (fixed and routed; removed ones are skipped)

  model::Board& b_;
  const model::DesignRules& r_;
  drc::CopperModel cm_;
  std::unique_ptr<drc::RuleEngine> re_;
  std::unique_ptr<drc::ZoneFills> fills_;     // edge index of every zone fill present at construction
  bool linear_zone_tests_ = false;
  std::unique_ptr<index::UniformGrid> grid_;   // copper items (fixed and routed)
  std::unique_ptr<index::UniformGrid> rgrid_;  // routed copper items only
  std::unique_ptr<index::UniformGrid> hgrid_;  // holes
  std::unique_ptr<index::UniformGrid> egrid_;  // board-edge segments
  std::vector<geom::Shape> edge_segs_;
  geom::Box bounds_;
  Coord reach_ = 0;
  std::vector<geom::Point> outline_;
  std::vector<std::vector<geom::Point>> cutouts_;
  // Inside-board raster: 0 outside, 1 inside, 2 boundary cell (exact test needed).
  std::vector<std::uint8_t> inside_raster_;
  int ir_w_ = 0, ir_h_ = 0;
  Coord ir_cell_ = 250'000;
  // Edges of each loop (outline first, then the cut-outs) by raster row: the exact test of a point in a boundary cell
  // needs only the edges whose y-range meets the point's row (LeeChee_1800: 572 outline edges, 28% of the run before).
  struct LoopBands {
    std::vector<std::uint32_t> start;  // ir_h_ + 1 offsets into edges
    std::vector<std::uint32_t> edges;  // edge indices, ascending within a row
  };
  std::vector<LoopBands> loop_bands_;

  bool soft_zones_ = false;
  bool soft_mask_graphics_ = false;
  bool soft_copper_graphics_ = false;
  bool soft_item(const drc::CopperItem& it) const {
    return soft_zones_ && it.kind == drc::ItemKind::Zone;
  }
  bool vias_off_pads_ = false;
  Coord vias_off_pads_max_ = 2'000'000;
  mutable bool pad_via_exempt_ = false;
  bool via_keeps_off(const drc::CopperItem& it) const;
  std::vector<std::pair<geom::Shape, const model::Zone*>> keepouts_;
  std::vector<geom::Shape> mask_open_[2];  // solder-mask openings drawn as graphics (front, back)
  struct Aperture {
    std::vector<geom::Shape> shapes;  // pad copper shape; the opening is this inflated by `margin`
    Coord margin = 0;
    model::NetId net = 0;
    geom::Box box;
  };
  std::vector<Aperture> apertures_[2];               // pad solder-mask openings per side
  std::unique_ptr<index::UniformGrid> agrid_[2];
  std::vector<std::pair<int, geom::Shape>> texts_;   // copper text boxes (copper index, rectangle)
  Coord via_mask_ = 0;
  Coord max_hole_local_ = 0;                         // largest local (pad/footprint) clearance on a hole                               // mask expansion of untented vias (0 when tented)
  // Mask-opening and copper-text conflicts for new copper on `layer` (fixed obstacles only).
  void aperture_codes(const geom::Shape& s, int layer, bool via_probe, const std::function<void(model::NetId)>& hit) const;
};

// Filled area of zone fill items (nm^2), shoelace over each item's outline.
long double zone_fill_area(const drc::CopperModel& cm, const std::vector<int>& items);
// With soft zones a fill of at least this area that no pad touches is its net's plane: a connection target of its
// own (doc 05 §26). Smaller ones are slivers of a stale fill, which a refill may drop or reshape.
inline constexpr long double kPlaneTargetArea = 1e12L;  // 1 mm^2

}  // namespace tmk::route

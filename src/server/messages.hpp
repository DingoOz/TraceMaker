// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Builders for viewer protocol v1 messages (docs/13-viewer-protocol.md). Pure functions returning one JSON
// object per message, ready for events::Sink::publish(). No Boost or networking: the router links only this
// library (tm::viz) and publishes into whatever Sink it was given.
//
// Every message starts with its "type" key (insertion-ordered JSON), so a server can classify messages by
// looking at the first bytes without parsing them.
#include <cstdint>
#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "model/board.hpp"

namespace tmk::server {

using ObjectId = std::int64_t;

// Ids used in the `board` snapshot: tracks and vias use their index in Board::tracks / Board::vias, pads their
// index in Board::pads. Arc tracks are sent as chains of straight segments with negative ids
// (-1, -2, …) so producers that number new tracks from Board::tracks.size() upwards never collide with them.

// Full scene snapshot (`board`).
// `freeze` false adds `"freeze": false` to the snapshot: the viewer does not hold the scene on a routing stall (route --no-freeze, issue #8).
std::string board_snapshot_json(const model::Board& b, const std::string& name, bool freeze = true);

std::string track_add(ObjectId id, const model::Track& t);
std::string track_add(ObjectId id, model::Point a, model::Point b, Coord width, int layer, model::NetId net);
std::string track_remove(ObjectId id);
std::string via_add(ObjectId id, const model::Via& v);
std::string via_remove(ObjectId id);
std::string footprint_move(std::string_view ref, model::Point pos, double angle);

struct RatsEdge {
  model::Point a, b;
  model::NetId net = 0;
};
std::string ratsnest(std::span<const RatsEdge> edges);

// Transient: sampled points of an active search frontier on one copper layer.
std::string frontier(std::int64_t conn, int layer, std::span<const model::Point> pts);

struct PathPoint {
  model::Point p;
  int layer = 0;
};
// Transient: a candidate path being evaluated.
std::string path_try(std::int64_t conn, std::span<const PathPoint> pts);

struct Failure {
  std::int64_t conn = 0;
  model::NetId net = 0;
  int rung = 0;
  std::string cause;
  model::Point a, b;
  std::vector<ObjectId> blockers;
  geom::Box region;  // empty box = none
  // Why the attempt was rejected (doc 13, issue #8); all optional, and an old reader ignores them. Lengths in nm; unset = left out.
  std::vector<model::NetId> blocker_nets;    // nets of the connections the rejected route would have ripped
  std::string reason;                        // boxed_in, no_legal_path, exact_check, rip_limit, ...
  std::int64_t width_nm = 0, clearance_nm = 0, min_width_nm = 0;  // the attempt's track width and clearance, and the narrowest legal width
  std::int64_t gap_nm = -1, gap_fixed_nm = -1;                    // widest track that gets through (routed copper counted / only fixed copper)
  int gap_layer = -1;
  std::optional<model::Point> at;            // where the way is narrowest (or the closest approach)
  std::string verdict;                       // placement | rule | contested | unknown
  std::string rule;                          // where the width and clearance came from
  std::vector<PathPoint> rejected;           // the path that was tried
};
std::string failure(const Failure& f);

struct Stats {
  std::string stage;
  std::int64_t iteration = 0;
  std::int64_t routed = 0, total = 0, unrouted = 0;
  std::int64_t rips = 0, failures = 0;
  double elapsed_s = 0;
  std::vector<std::pair<std::string, double>> extra;  // shown as extra counters in the HUD
};
std::string stats(const Stats& s);

// state is "begin" or "end".
std::string stage(std::string_view name, std::string_view state, std::string_view detail = {});
// level: "debug" | "info" | "warn" | "error".
std::string log(std::string_view level, std::string_view text);

// Overlay grid: data is row-major w*h bytes (0–255), cell size in nm, origin (x0, y0); layer -1 = all layers.
std::string heatmap(std::string_view name, model::Point origin, Coord cell, int w, int h, int layer, double max,
                    std::span<const std::uint8_t> data);

// Returns the value of the top-level "type" key of a message, or an empty string. Fast path for messages built
// here ("type" first); falls back to a full parse otherwise.
std::string message_type(std::string_view json);

}  // namespace tmk::server

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Edits a loaded .kicad_pcb document in place, KiCad style (design doc 08 §3). Only the nodes TraceMaker
// owns change: new segment/via nodes, removed tracks, and footprint placement. Everything else is kept
// byte for byte. Re-read the document after editing to get an updated board model.
#include <cstdint>
#include <string>
#include <string_view>

#include "io/kicad/board_reader.hpp"

namespace tmk::io {

class BoardEditor {
 public:
  // `seed` makes generated UUIDs deterministic.
  BoardEditor(LoadedBoard& board, std::uint64_t seed = 1) : lb_(board), seed_(seed) {}

  void add_track(const model::Track& t);
  void add_via(const model::Via& v);
  void remove_track(std::size_t index);
  void remove_via(std::size_t index);
  // Moves footprint `index` to `pos` with orientation `angle` (degrees). Pad and text orientations, which
  // KiCad stores as absolute angles, are rotated by the same delta. Flipping sides is not supported here.
  void move_footprint(std::size_t index, model::Point pos, double angle);
  // Moves footprint `index` to the other side of the board the way KiCad does (FOOTPRINT::Flip, top/bottom, about
  // its own origin: local y and pad orientations negated, text angles 180° − a, F.* <-> B.* layers, texts
  // mirrored), then puts it at `pos` with the final orientation `angle` in degrees (flipped orientation −angle0
  // plus any rotation). Only for footprints where flip_supported() holds.
  void flip_footprint(std::size_t index, model::Point pos, double angle);

  // Adds a board-level copper zone as KiCad writes one: net, layer (the zone's single copper layer), outline (first
  // ring), clearance, minimum width, thermal and island settings, and its fills when it has any (doc 05 §53).
  void add_zone(const model::Zone& z);
  // Replaces the filled polygons of zone `index` (board.zones order) with `fills` (copper index, fractured polygon, as
  // drc::refill_zones gives them) and marks the zone filled.
  void set_zone_fills(std::size_t index, const std::vector<std::pair<int, std::vector<model::Point>>>& fills);
  // Gives the board `n` copper layers (even, 2..32; doc 05 §53): inner layers are added after the last one, or the last
  // ones removed. Removing needs those layers empty (no track, arc, blind via, zone, graphic, text or pad that names
  // them); through vias and plated holes follow the layer count by themselves. A (stackup) block is removed, since its
  // copper and dielectrics no longer match, and KiCad builds its default stackup for the new count. Returns false with
  // the reason in `why` (and the document unchanged) when it cannot; `notes` receives what changed besides the layers.
  bool set_copper_layer_count(int n, std::string* why = nullptr, std::vector<std::string>* notes = nullptr);

  std::string write() const { return lb_.doc.write(); }
  void save(const std::string& path) const { lb_.doc.save(path); }

  // Deterministic RFC-4122-style v4 UUID string.
  std::string next_uuid();

 private:
  std::string net_expr(model::NetId net) const;

  LoadedBoard& lb_;
  std::uint64_t seed_;
  std::uint64_t uuid_counter_ = 0;
};

// True if BoardEditor::flip_footprint mirrors every item of footprint `index` exactly: no copper on inner
// layers, no padstacks, no text on copper, and nothing it does not know (zones, text boxes, dimensions, private
// layers, ...). `why` receives the first reason it cannot.
bool flip_supported(const LoadedBoard& board, std::size_t index, std::string* why = nullptr);
// KiCad's FlipLayer for outer layers: the first and last copper layer of `board` (by their names in the file) and
// F.X <-> B.X for the other side-specific layers; any other name unchanged.
std::string flip_layer_name(const model::Board& board, std::string_view name);

// Formats an angle the way KiCad writes it: shortest decimal, no trailing zeros ("90", "45.5").
std::string format_angle(double deg);

}  // namespace tmk::io

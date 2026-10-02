#pragma once
// Edits a loaded .kicad_pcb document in place, KiCad style (design doc 08 §3). Only the nodes TraceMaker
// owns change: new segment/via nodes, removed tracks, and footprint placement. Everything else is kept
// byte for byte. Re-read the document after editing to get an updated board model.
#include <cstdint>
#include <string>

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

// Formats an angle the way KiCad writes it: shortest decimal, no trailing zeros ("90", "45.5").
std::string format_angle(double deg);

}  // namespace tmk::io

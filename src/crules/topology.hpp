#pragma once
// Netlist views of a board for component recognition (doc 15 §3.1 "Topology", §8.0 topology detectors): pads per
// net, nets per footprint, part kinds from reference letters, and the decoupling-capacitor ties of decision D25.
// Every list is in ascending index order so results never depend on hash order (CLAUDE.md rule 2).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "model/board.hpp"

namespace tmk::crules {

using geom::Point;

class BoardIndex {
 public:
  explicit BoardIndex(const model::Board& b);

  const model::Board& board() const { return b_; }
  const std::vector<int>& net_pads(model::NetId n) const { return net_pads_[static_cast<std::size_t>(n)]; }
  // Distinct nets (non-zero) on a footprint's pads, ascending.
  const std::vector<model::NetId>& fp_nets(int fi) const { return fp_nets_[static_cast<std::size_t>(fi)]; }
  // Footprints with a pad on the net, ascending.
  std::vector<int> parts_on(model::NetId n) const;
  // First pad (ascending index) of footprint `fi` on net `n`, or -1.
  int pad_on(int fi, model::NetId n) const;
  const std::string& letters(int fi) const { return letters_[static_cast<std::size_t>(fi)]; }
  const std::string& net_name(model::NetId n) const { return b_.nets[static_cast<std::size_t>(n)].name; }
  int pad_count(int fi) const;  // pads with a net or copper (mechanical NPTH excluded)

  bool ground(model::NetId n) const { return n > 0 && ground_[static_cast<std::size_t>(n)] != 0; }
  bool supply(model::NetId n) const { return n > 0 && supply_[static_cast<std::size_t>(n)] != 0; }  // not ground
  // Neither ground nor supply.
  bool signal(model::NetId n) const { return n > 0 && !ground(n) && !supply(n); }

  bool is_ic(int fi) const;         // U*, IC* (D25's definition)
  bool is_regulator_ref(int fi) const;  // U*, IC*, VR*, REG*, PS*
  bool is_cap(int fi) const;        // C* with exactly two pads
  bool is_series(int fi) const;     // two-pad R*, FB*, L*, RN? (series element for net walks)
  bool is_connector(int fi) const;  // J*, P*, CN*, CON*, USB*, XS* followed by a digit (doc 04 connectors)
  bool has_pin_names() const { return has_pin_names_; }

 private:
  const model::Board& b_;
  std::vector<std::vector<int>> net_pads_;
  std::vector<std::vector<model::NetId>> fp_nets_;
  std::vector<std::string> letters_;
  std::vector<std::uint8_t> ground_, supply_;
  bool has_pin_names_ = false;
};

// Decision D25 (doc 04 §2): a two-pad capacitor (C*) between a supply net and a ground net is tied to the nearest
// pad, in the input placement, of an IC (U*, IC*) on the same supply; ties keep the lowest pad index.
// `usable(fp)` filters footprints (the placer passes "has a part"). With `generalised` (doc 15 ic_decoupling),
// the supply may also be an analog supply/reference name (VREF, AREF, VCAP, ...) or a net that reaches an IC pad
// whose pin name is a supply pin, and the ground may be a net reaching an IC ground pin by name.
struct DecapTie {
  int cap_fp = -1;
  int cap_pad = -1;   // the capacitor's supply pad
  int ic_pad = -1;    // the IC pad it is tied to
};
std::vector<DecapTie> decap_ties(const model::Board& b, const std::function<bool(int)>& usable, bool generalised);

}  // namespace tmk::crules

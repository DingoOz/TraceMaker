#pragma once
// The board's physical stackup as KiCad stores it in (setup (stackup ...)) (design doc 15 §5.3): read-only,
// used to turn impedance rules into widths and gaps. Boards without a stackup block (KiCad 5 and older, or
// boards whose stackup was never edited) have `present == false`; nothing is guessed (CLAUDE.md rule 6).
#include <string>
#include <vector>

#include "core/units.hpp"

namespace tmk::model {

struct StackupLayer {
  std::string name;          // "F.Cu", "dielectric 1", "F.Mask", ...
  std::string type;          // "copper", "core", "prepreg", "Top Solder Mask", ... as written
  std::string material;      // first sublayer's material ("FR4"), empty if not given
  Coord thickness = 0;       // nm; a dielectric with sublayers (addsublayer) sums them
  // Relative permittivity; 0 = not given. Sublayers combine as parallel-plate capacitors in series:
  // er = sum(h) / sum(h / er_i) (exact for a field normal to the layers).
  double epsilon_r = 0;
  double loss_tangent = 0;   // thickness-weighted mean over sublayers; 0 = not given
  int sublayers = 1;
  bool epsilon_complete = false;  // every sublayer gave epsilon_r
  int copper_index = -1;     // stack index for copper layers, -1 otherwise

  bool is_copper() const { return type == "copper"; }
  bool is_mask() const;      // "Top Solder Mask" / "Bottom Solder Mask"
};

// The dielectric between two adjacent copper layers, combined over every stackup layer between them.
struct DielectricGap {
  Coord thickness = 0;       // nm
  double epsilon_r = 0;      // series combination (see StackupLayer::epsilon_r); 0 = unknown
  double loss_tangent = 0;
  bool complete = false;     // thickness > 0 and every layer gave epsilon_r
  std::string materials;     // "FR4" or "FR4+R-1551(W)"
};

struct Stackup {
  bool present = false;      // the board has a (stackup ...) block
  std::vector<StackupLayer> layers;  // file order (top to bottom)
  std::string copper_finish;
  bool dielectric_constraints = false;

  // Stack position of copper layer `copper_index` in `layers`, -1 if the stackup does not list it.
  int position_of_copper(int copper_index) const;
  // Dielectric between copper layers `a` and `b` (stack indices, a < b, any distance apart; copper layers in between
  // are not counted). complete = false if either copper layer is missing from the stackup.
  DielectricGap between(int a, int b) const;
  // Copper thickness of copper layer `copper_index`, 0 if unknown.
  Coord copper_thickness(int copper_index) const;
  // Solder mask over the given outer side (front = true), nullptr if the stackup has none.
  const StackupLayer* mask(bool front) const;
};

}  // namespace tmk::model

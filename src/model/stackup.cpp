#include "model/stackup.hpp"

namespace tmk::model {

bool StackupLayer::is_mask() const { return type.find("Solder Mask") != std::string::npos; }

int Stackup::position_of_copper(int copper_index) const {
  for (std::size_t i = 0; i < layers.size(); ++i)
    if (layers[i].is_copper() && layers[i].copper_index == copper_index) return static_cast<int>(i);
  return -1;
}

DielectricGap Stackup::between(int a, int b) const {
  DielectricGap g;
  const int pa = position_of_copper(a), pb = position_of_copper(b);
  if (pa < 0 || pb < 0 || pa >= pb) return g;
  double h_over_er = 0, tand_h = 0;
  bool all_er = true;
  for (int i = pa + 1; i < pb; ++i) {
    const StackupLayer& l = layers[static_cast<std::size_t>(i)];
    if (l.is_copper()) continue;  // an intermediate copper layer: its own thickness is not dielectric
    g.thickness += l.thickness;
    if (l.thickness <= 0) continue;
    if (!l.epsilon_complete || l.epsilon_r <= 0) {
      all_er = false;
      continue;
    }
    h_over_er += static_cast<double>(l.thickness) / l.epsilon_r;
    tand_h += l.loss_tangent * static_cast<double>(l.thickness);
    if (!l.material.empty() && g.materials.find(l.material) == std::string::npos) g.materials += (g.materials.empty() ? "" : "+") + l.material;
  }
  g.complete = all_er && g.thickness > 0 && h_over_er > 0;
  if (g.complete) {
    g.epsilon_r = static_cast<double>(g.thickness) / h_over_er;
    g.loss_tangent = tand_h / static_cast<double>(g.thickness);
  }
  return g;
}

Coord Stackup::copper_thickness(int copper_index) const {
  const int p = position_of_copper(copper_index);
  return p < 0 ? 0 : layers[static_cast<std::size_t>(p)].thickness;
}

const StackupLayer* Stackup::mask(bool front) const {
  for (const auto& l : layers)
    if (l.is_mask() && (l.type.starts_with("Top") == front)) return &l;
  return nullptr;
}

}  // namespace tmk::model

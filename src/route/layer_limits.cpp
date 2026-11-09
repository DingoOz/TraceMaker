// SPDX-License-Identifier: GPL-3.0-or-later
#include "route/layer_limits.hpp"

#include <cmath>
#include <stdexcept>

#include "core/parse_double.hpp"

namespace tmk::route {

namespace {

std::string copper_names(const model::Board& b) {
  std::string s;
  for (int i = 0; i < b.copper_count(); ++i) s += (i ? ", " : "") + b.copper_name(i);
  return s;
}

int find_copper(const model::Board& b, const std::string& name) {
  int idx = b.copper_index(name);
  for (int i = 0; idx < 0 && i < b.copper_count(); ++i) {
    const auto& l = b.layers[static_cast<std::size_t>(b.copper[static_cast<std::size_t>(i)])];
    if (!l.user_name.empty() && l.user_name == name) idx = i;
  }
  return idx;
}

int copper_layer(const model::Board& b, const std::string& name, const char* option) {
  const int idx = find_copper(b, name);
  if (idx < 0) throw std::invalid_argument(std::string(option) + ": \"" + name + "\" is not a copper layer of this board (" + copper_names(b) + ")");
  return idx;
}

struct DirEntry {
  std::string layer;  // "" = every copper layer (diagonal penalty only)
  std::int8_t dir = kLayerDirNone;
  double pen = 0;
};

std::vector<DirEntry> parse_dir(const std::vector<std::string>& specs) {
  std::vector<DirEntry> out;
  for (const auto& entry : specs) {
    const auto eq = entry.rfind('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 == entry.size()) throw std::invalid_argument("--layer-dir: \"" + entry + "\" is not LAYER=h|v|none");
    DirEntry e{entry.substr(0, eq)};
    const std::string v = entry.substr(eq + 1);
    if (v == "h" || v == "horizontal") e.dir = kLayerDirHorizontal;
    else if (v == "v" || v == "vertical") e.dir = kLayerDirVertical;
    else if (v == "none" || v == "-") e.dir = kLayerDirNone;
    else throw std::invalid_argument("--layer-dir: the direction in \"" + entry + "\" must be h, v or none");
    for (const auto& o : out)
      if (o.layer == e.layer) throw std::invalid_argument("--layer-dir: layer " + e.layer + " is given twice");
    out.push_back(std::move(e));
  }
  return out;
}

std::vector<DirEntry> parse_diag(const std::vector<std::string>& specs) {
  std::vector<DirEntry> out;
  for (const auto& entry : specs) {
    const auto eq = entry.rfind('=');
    if (eq == 0 || (eq != std::string::npos && eq + 1 == entry.size())) throw std::invalid_argument("--layer-diag-pen: \"" + entry + "\" is not LAYER=PITCHES or PITCHES");
    DirEntry e{eq == std::string::npos ? std::string() : entry.substr(0, eq)};
    const auto v = parse_double(std::string_view(entry).substr(eq == std::string::npos ? 0 : eq + 1));
    if (!v || !(*v >= 0.0) || *v > kMaxDiagPen)
      throw std::invalid_argument("--layer-diag-pen: the penalty in \"" + entry + "\" must be a number from 0 to " + std::to_string(static_cast<int>(kMaxDiagPen)));
    e.pen = *v;
    for (const auto& o : out)
      if (o.layer == e.layer) throw std::invalid_argument("--layer-diag-pen: " + (e.layer.empty() ? std::string("the value for every layer") : "layer " + e.layer) + " is given twice");
    out.push_back(std::move(e));
  }
  return out;
}

}  // namespace

void check_layer_dir_specs(const std::vector<std::string>& dir_specs, const std::vector<std::string>& diag_specs) {
  (void)parse_dir(dir_specs);
  (void)parse_diag(diag_specs);
}

LayerDirections layer_directions(const model::Board& b, const std::vector<std::string>& dir_specs, const std::vector<std::string>& diag_specs) {
  LayerDirections out;
  const auto nl = static_cast<std::size_t>(b.copper_count());
  auto unknown = [&](const char* option, const std::string& name) {
    out.warnings.push_back(std::string(option) + ": \"" + name + "\" is not a copper layer of this board (" + copper_names(b) + "); ignored");
  };
  const auto dirs = parse_dir(dir_specs);
  if (!dirs.empty()) out.dir.assign(nl, kLayerDirNone);
  for (const auto& e : dirs) {
    const int idx = find_copper(b, e.layer);
    if (idx < 0) unknown("--layer-dir", e.layer);
    else out.dir[static_cast<std::size_t>(idx)] = e.dir;
  }
  const auto diags = parse_diag(diag_specs);
  if (!diags.empty()) out.diag_pen.assign(nl, 0.0);
  for (const auto& e : diags)  // the bare value first, so named layers override it whatever the order
    if (e.layer.empty()) out.diag_pen.assign(nl, e.pen);
  for (const auto& e : diags) {
    if (e.layer.empty()) continue;
    const int idx = find_copper(b, e.layer);
    if (idx < 0) unknown("--layer-diag-pen", e.layer);
    else out.diag_pen[static_cast<std::size_t>(idx)] = e.pen;
  }
  return out;
}

LayerLimits layer_limits(const model::Board& b, const std::vector<std::string>& no_tracks_on, const std::vector<std::string>& layer_costs) {
  LayerLimits out;
  for (const auto& name : no_tracks_on) out.no_track_layers |= model::layer_bit(copper_layer(b, name, "--no-tracks-on"));
  const model::LayerMask all = b.copper_count() >= 64 ? ~model::LayerMask{0} : model::layer_bit(b.copper_count()) - 1;
  if (!no_tracks_on.empty() && (out.no_track_layers & all) == all) throw std::invalid_argument("--no-tracks-on: no copper layer is left for tracks");
  for (const auto& entry : layer_costs) {
    // The layer name may itself contain '=' only in theory; the factor is what follows the last one.
    const auto eq = entry.rfind('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 == entry.size()) throw std::invalid_argument("--layer-cost: \"" + entry + "\" is not LAYER=FACTOR");
    const int idx = copper_layer(b, entry.substr(0, eq), "--layer-cost");
    const double f = parse_double(std::string_view(entry).substr(eq + 1)).value_or(0.0);
    // Below 1 a track would cost less than its length, which the search's distance bounds do not allow for.
    if (!(f >= 1.0) || f > kMaxLayerCost)
      throw std::invalid_argument("--layer-cost: the factor in \"" + entry + "\" must be a number from 1 to " + std::to_string(static_cast<int>(kMaxLayerCost)));
    if (out.layer_cost_pm.empty()) out.layer_cost_pm.assign(static_cast<std::size_t>(b.copper_count()), 1000);
    out.layer_cost_pm[static_cast<std::size_t>(idx)] = static_cast<std::int32_t>(std::llround(f * 1000.0));
  }
  return out;
}

}  // namespace tmk::route

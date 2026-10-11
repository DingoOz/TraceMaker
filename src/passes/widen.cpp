// SPDX-License-Identifier: GPL-3.0-or-later
#include "passes/widen.hpp"

#include <algorithm>
#include <cmath>

#include "route/obstacles.hpp"

namespace tmk::passes {
namespace {

using model::NetId;
using model::Point;

Coord length_of(const model::Track& t) {
  return static_cast<Coord>(std::llround(std::hypot(static_cast<double>(t.b.x - t.a.x), static_cast<double>(t.b.y - t.a.y))));
}

}  // namespace

WidenResult widen_tracks(const model::Board& board, const model::DesignRules& rules, std::vector<model::Track>& tracks,
                         const std::vector<model::Via>& vias, const WidenOptions& opt) {
  WidenResult res;
  // The router's own obstacle model over the board plus the new copper (rule 1): a wider track is accepted only if
  // segment_state calls it free against everything, the copper routed in this run included.
  model::Board w = board;
  route::Obstacles obs(w, rules);
  obs.set_soft_zones(opt.soft_zones);
  std::vector<int> item(tracks.size(), -1);  // copper item of each new track
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    w.tracks.push_back(tracks[i]);
    item[i] = obs.add_track(static_cast<int>(w.tracks.size()) - 1, static_cast<int>(i));
  }
  for (const auto& v : vias) {
    w.vias.push_back(v);
    obs.add_via(static_cast<int>(w.vias.size()) - 1, 0);
  }

  const std::size_t nets = board.nets.size();
  std::vector<Coord> class_width(nets, 0), target(nets, 0);
  std::vector<std::string> source(nets);
  std::vector<std::uint8_t> skip(nets, 0);
  for (std::size_t n = 1; n < nets; ++n) class_width[n] = std::max(rules.class_for(board.nets[n].name).track_width, rules.minimums.track_width);
  for (const NetId n : opt.skip_nets)
    if (n > 0 && static_cast<std::size_t>(n) < nets) skip[static_cast<std::size_t>(n)] = 1;
  for (const auto& t : opt.targets) {
    const auto n = static_cast<std::size_t>(t.net);
    if (t.net <= 0 || n >= nets || skip[n] || t.width <= target[n]) continue;
    target[n] = t.width;
    source[n] = t.source;
  }
  // A stated target wins over the class width even when it is narrower: the user or the rule named that width.
  std::vector<std::uint8_t> explicit_target(nets, 0);
  for (std::size_t n = 1; n < nets; ++n) {
    if (target[n] > 0) explicit_target[n] = 1;
    else if (opt.restore_necked && !skip[n]) target[n] = class_width[n], source[n] = "net class";
  }

  // What a track end sits on. A track wider than the via or pad it ends on would stick out around it, so such a
  // track is widened to that size at most (doc 05 §39.1): the neck stays at the pin, as a designer would draw it.
  struct PadBox {
    std::vector<geom::Shape> shapes;
    geom::Box box;
    model::LayerMask copper = 0;
    Coord size = 0;
  };
  std::vector<std::vector<PadBox>> pads(nets);
  for (const auto& p : board.pads) {
    if (p.net <= 0 || p.copper == 0 || static_cast<std::size_t>(p.net) >= nets || target[static_cast<std::size_t>(p.net)] == 0) continue;
    PadBox pb{drc::pad_shapes(p), {}, p.copper, std::min(p.size_x, p.size_y)};
    for (const auto& s : pb.shapes) pb.box.add(s.box);
    pads[static_cast<std::size_t>(p.net)].push_back(std::move(pb));
  }
  auto end_limit = [&](const model::Track& t, Point e) {
    Coord lim = INT64_MAX;
    for (const auto& v : w.vias)
      if (v.net == t.net && v.pos == e && v.layer_top <= t.layer && t.layer <= v.layer_bottom) lim = std::min(lim, v.size);
    const geom::Shape probe = geom::Shape::point(e, 0);
    for (const auto& pb : pads[static_cast<std::size_t>(t.net)]) {
      if (!(pb.copper & model::layer_bit(t.layer)) || pb.box.empty() || e.x < pb.box.x0 || e.x > pb.box.x1 || e.y < pb.box.y0 || e.y > pb.box.y1) continue;
      for (const auto& s : pb.shapes)
        if (geom::closer_than(probe, s, 1)) {
          lim = std::min(lim, pb.size);
          break;
        }
    }
    return lim;
  };

  std::vector<int> report_of(nets, -1);
  auto report = [&](NetId n) -> WidenNet& {
    auto& slot = report_of[static_cast<std::size_t>(n)];
    if (slot < 0) {
      slot = static_cast<int>(res.nets.size());
      res.nets.push_back({});
      res.nets.back().net = n;
      res.nets.back().target = target[static_cast<std::size_t>(n)];
      res.nets.back().source = source[static_cast<std::size_t>(n)];
    }
    return res.nets[static_cast<std::size_t>(slot)];
  };

  // Nets with a stated target first (they are the ones that carry current), then restored neck-downs; within each
  // group in commit order. Each accepted width is entered in the model at once, so later checks see it.
  std::vector<std::size_t> order;
  for (int pass = 1; pass >= 0; --pass)
    for (std::size_t i = 0; i < tracks.size(); ++i) {
      const auto n = static_cast<std::size_t>(tracks[i].net);
      if (tracks[i].net > 0 && n < nets && explicit_target[n] == pass) order.push_back(i);
    }
  const std::vector<model::Track> original = tracks;
  for (const std::size_t i : order) {
    auto& t = tracks[i];
    const auto n = static_cast<std::size_t>(t.net);
    if (t.locked || t.a == t.b || target[n] <= t.width) continue;
    auto& rn = report(t.net);
    ++rn.tracks;
    Coord goal = std::min({target[n], end_limit(t, t.a), end_limit(t, t.b)});
    {
      // A custom or board rule that caps the width holds for the widened track too.
      drc::CopperItem probe;
      probe.kind = drc::ItemKind::Track;
      probe.net = t.net;
      probe.layers = model::layer_bit(t.layer);
      probe.shapes = {geom::Shape::segment(t.a, t.b, t.width / 2)};
      probe.box = probe.shapes[0].box;
      probe.pos = t.a;
      probe.width = t.width;
      const Coord mx = obs.rules().track_width(probe, t.layer).second;
      if (mx > 0) goal = std::min(goal, mx);
    }
    if (goal <= t.width) {
      rn.at_target += goal < target[n];  // held by its pad or via: as wide as it may get
      continue;
    }
    if (res.out_of_work) continue;
    Coord got = 0;
    Coord last = 0;
    for (int k = opt.steps; k >= 1 && got == 0; --k) {
      Coord c = t.width + (goal - t.width) * k / std::max(1, opt.steps);
      if (k < opt.steps) c -= c % 1'000;
      if (c <= t.width || c == last) continue;
      last = c;
      if (res.checks >= opt.work_limit) {
        res.out_of_work = true;
        break;
      }
      ++res.checks;
      if (obs.segment_state(t.a, t.b, t.layer, c, t.net, false) == 0) got = c;
    }
    if (got == 0) continue;
    t.width = got;
    obs.remove_item(item[i]);
    w.tracks.push_back(t);
    item[i] = obs.add_track(static_cast<int>(w.tracks.size()) - 1, static_cast<int>(i));
    ++rn.widened;
    ++res.widened;
    rn.at_target += got == goal;
    rn.widened_length += length_of(t);
  }
  // Lengths per width, before and after, for every net the pass looked at.
  for (std::size_t i = 0; i < tracks.size(); ++i) {
    const auto n = static_cast<std::size_t>(tracks[i].net);
    if (tracks[i].net <= 0 || n >= nets || report_of[n] < 0) continue;
    auto& rn = res.nets[static_cast<std::size_t>(report_of[n])];
    rn.before[original[i].width] += length_of(original[i]);
    rn.after[tracks[i].width] += length_of(tracks[i]);
  }
  std::sort(res.nets.begin(), res.nets.end(), [](const WidenNet& a, const WidenNet& b) { return a.net < b.net; });
  return res;
}

}  // namespace tmk::passes

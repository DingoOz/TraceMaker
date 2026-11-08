// SPDX-License-Identifier: GPL-3.0-or-later
#include "passes/repair.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <set>

#include "drc/connectivity.hpp"
#include "drc/copper.hpp"
#include "index/uniform_grid.hpp"

namespace tmk::passes {
namespace {

using drc::ItemKind;
using model::NetId;
using model::Point;

std::string fmt_mm(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%g", v);
  return buf;
}

// The copper model of a board with a grid over it, for the connectivity questions below.
struct CopperIndex {
  drc::CopperModel cm;
  std::unique_ptr<index::UniformGrid> grid;
  explicit CopperIndex(const model::Board& b) : cm(drc::build_copper(b)) {
    geom::Box all;
    for (const auto& it : cm.items) all.add(it.box);
    if (all.empty()) all = geom::Box{0, 0, 1, 1};
    all = all.inflated(1'000'000);
    grid = std::make_unique<index::UniformGrid>(all, 1'000'000, cm.items.size());
    for (std::size_t i = 0; i < cm.items.size(); ++i) grid->insert(static_cast<int>(i), cm.items[i].box);
  }
};

// Per pad: its copper cluster (the router's connectivity: end-point anchors, no overlap links), -1 for pads without copper.
std::vector<int> pad_roots(const model::Board& b) {
  CopperIndex ci(b);
  const auto con = drc::compute_connectivity(b, ci.cm, *ci.grid);
  std::vector<int> root(b.pads.size(), -1);
  for (std::size_t i = 0; i < ci.cm.items.size(); ++i) {
    const auto& it = ci.cm.items[i];
    if (it.kind == ItemKind::Pad && it.bare_of < 0 && it.index >= 0 && static_cast<std::size_t>(it.index) < root.size()) root[static_cast<std::size_t>(it.index)] = con.root[i];
  }
  return root;
}

int open_of(const model::Board& b, const std::vector<int>& root) {
  std::map<NetId, std::set<int>> groups;
  for (std::size_t p = 0; p < b.pads.size(); ++p)
    if (b.pads[p].net > 0 && root[p] >= 0) groups[b.pads[p].net].insert(root[p]);
  int open = 0;
  for (const auto& [n, g] : groups) open += static_cast<int>(g.size()) - 1;
  return open;
}

// True when two pads joined in `before` are apart in `after` (rule 4: nothing that was connected may end worse).
bool splits(const std::vector<int>& before, const std::vector<int>& after) {
  std::map<int, int> image;
  for (std::size_t p = 0; p < before.size(); ++p) {
    if (before[p] < 0) continue;
    const auto [it, fresh] = image.emplace(before[p], after[p]);
    if (!fresh && it->second != after[p]) return true;
  }
  return false;
}

// The tracks and vias of a board as a graph of contacts, to lift copper by branches: the chain of tracks and vias of one net between two
// stops, a stop being a pad, a zone fill, an arc or copper graphic, or a junction (three or more tracks or vias meeting). Lifting a whole
// branch leaves no fragment in the way (the user's manual attempt in issue #56 cut tracks at a radius and left 11 open instead of 3);
// lifting branches, not the whole tree of a net, keeps the rest of a big net where it is.
class CopperGraph {
 public:
  explicit CopperGraph(const model::Board& b) : b_(b), ci_(b), lifted_t_(b.tracks.size(), 0), lifted_v_(b.vias.size(), 0) {
    track_item_.assign(b.tracks.size(), -1);
    via_item_.assign(b.vias.size(), -1);
    for (std::size_t i = 0; i < ci_.cm.items.size(); ++i) {
      const auto& it = ci_.cm.items[i];
      if (it.bare_of >= 0) continue;
      if (it.kind == ItemKind::Track) track_item_[static_cast<std::size_t>(it.index)] = static_cast<int>(i);
      if (it.kind == ItemKind::Via) via_item_[static_cast<std::size_t>(it.index)] = static_cast<int>(i);
    }
  }

  // A track or via: kind (0 track, 1 via) and index in the board.
  struct Ref {
    int kind = 0, index = -1;
    bool operator<(const Ref& o) const { return kind != o.kind ? kind < o.kind : index < o.index; }
    bool operator==(const Ref& o) const = default;
  };
  struct Contacts {
    std::vector<Ref> movable;  // tracks and vias of the net touching here
    bool stop = false;         // a pad, zone fill, arc or copper graphic of the net touches here
  };

  // Gap from `p` to the item's copper (0 on it), nm.
  double gap(Ref r, Point p) const {
    if (r.kind == 0) {
      const auto& t = b_.tracks[static_cast<std::size_t>(r.index)];
      return static_cast<double>(std::max(0.0L, geom::point_seg_dist(p, t.a, t.b) - t.width / 2.0L));
    }
    const auto& v = b_.vias[static_cast<std::size_t>(r.index)];
    return static_cast<double>(std::max(0.0L, geom::point_seg_dist(p, v.pos, v.pos) - v.size / 2.0L));
  }
  NetId net(Ref r) const { return r.kind == 0 ? b_.tracks[static_cast<std::size_t>(r.index)].net : b_.vias[static_cast<std::size_t>(r.index)].net; }
  bool fixed(Ref r) const {
    if (r.kind == 0) {
      const auto& t = b_.tracks[static_cast<std::size_t>(r.index)];
      return t.locked || t.tuning_pattern;
    }
    const auto& v = b_.vias[static_cast<std::size_t>(r.index)];
    return v.locked || v.free;
  }
  bool lifted(Ref r) const { return (r.kind == 0 ? lifted_t_ : lifted_v_)[static_cast<std::size_t>(r.index)] != 0; }
  void set_lifted(Ref r, bool on) { (r.kind == 0 ? lifted_t_ : lifted_v_)[static_cast<std::size_t>(r.index)] = on ? 1 : 0; }
  void clear_lifted() {
    std::fill(lifted_t_.begin(), lifted_t_.end(), 0);
    std::fill(lifted_v_.begin(), lifted_v_.end(), 0);
  }

  // Tracks and vias within `r` of `p` (their copper), in index order.
  std::vector<Ref> near(Point p, Coord r) {
    std::vector<Ref> out;
    const geom::Box q{p.x - r, p.y - r, p.x + r, p.y + r};
    ci_.grid->query(q, [&](int id) {
      const auto& it = ci_.cm.items[static_cast<std::size_t>(id)];
      if (it.bare_of >= 0 || (it.kind != ItemKind::Track && it.kind != ItemKind::Via)) return;
      const Ref ref{it.kind == ItemKind::Track ? 0 : 1, it.index};
      if (gap(ref, p) <= static_cast<double>(r)) out.push_back(ref);
    });
    std::sort(out.begin(), out.end());
    return out;
  }

  // The ends of an item: a track's two end points on its layer, a via's centre on its layers.
  std::vector<std::pair<Point, model::LayerMask>> ends(Ref r) const {
    if (r.kind == 0) {
      const auto& t = b_.tracks[static_cast<std::size_t>(r.index)];
      return {{t.a, model::layer_bit(t.layer)}, {t.b, model::layer_bit(t.layer)}};
    }
    const auto& v = b_.vias[static_cast<std::size_t>(r.index)];
    return {{v.pos, ci_.cm.items[static_cast<std::size_t>(via_item_[static_cast<std::size_t>(r.index)])].layers}};
  }

  // What touches the end `p` (on `layers`) of item `self`, lifted items left out. A track touches when one of its end points or its
  // centreline lies within 1 um of `p`, or, for a via's centre, within the via; a via when `p` lies in its copper; anything else of the
  // net when its copper contains `p`.
  Contacts contacts(Ref self, Point p, model::LayerMask layers, bool skip_lifted) {
    Contacts c;
    const NetId n = net(self);
    const Coord reach = self.kind == 1 ? b_.vias[static_cast<std::size_t>(self.index)].size / 2 + 1 : kTouch;
    const geom::Box q{p.x - reach, p.y - reach, p.x + reach, p.y + reach};
    const geom::Shape pt = geom::Shape::point(p, 0);
    ci_.grid->query(q, [&](int id) {
      const auto& it = ci_.cm.items[static_cast<std::size_t>(id)];
      if (it.bare_of >= 0 || it.net != n || (it.layers & layers) == 0) return;
      if (it.kind == ItemKind::Track) {
        const Ref r{0, it.index};
        if (r == self || (skip_lifted && lifted(r))) return;
        const auto& t = b_.tracks[static_cast<std::size_t>(it.index)];
        if (geom::point_seg_closer(p, t.a, t.b, reach)) c.movable.push_back(r);
      } else if (it.kind == ItemKind::Via) {
        const Ref r{1, it.index};
        if (r == self || (skip_lifted && lifted(r))) return;
        const auto& v = b_.vias[static_cast<std::size_t>(it.index)];
        if (geom::point_seg_closer(p, v.pos, v.pos, v.size / 2 + reach)) c.movable.push_back(r);
      } else {
        for (const auto& sh : it.shapes)
          if (geom::closer_than(sh, pt, 1)) c.stop = true;
      }
    });
    std::sort(c.movable.begin(), c.movable.end());
    c.movable.erase(std::unique(c.movable.begin(), c.movable.end()), c.movable.end());
    return c;
  }

  // The branch through `seed` (seed included), or empty when it holds a fixed item. A via that three or more tracks and vias touch
  // is a junction: no branch takes it.
  std::vector<Ref> branch(Ref seed) {
    std::vector<Ref> out;
    if (is_junction_via(seed)) return out;
    std::set<Ref> seen{seed};
    std::vector<Ref> stack{seed};
    while (!stack.empty()) {
      const Ref x = stack.back();
      stack.pop_back();
      if (fixed(x)) return {};
      out.push_back(x);
      for (const auto& [p, layers] : ends(x)) {
        const Contacts c = contacts(x, p, layers, false);
        if (x.kind == 0 && (c.stop || c.movable.size() != 1)) continue;  // a stop, a junction or a dangling end
        for (const Ref& y : c.movable) {
          if (seen.count(y) || is_junction_via(y)) continue;
          seen.insert(y);
          stack.push_back(y);
        }
      }
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  // After lifting: tracks and vias of the net left with an end that touched something and now touches nothing (a track) or with one
  // contact left of two or more (a via that is no longer a way through) are lifted too, until none is left; fixed ones stay.
  std::vector<Ref> prune(const std::vector<Ref>& lifted_now) {
    std::vector<Ref> extra;
    std::vector<Ref> work;
    auto push_contacts = [&](Ref r) {
      for (const auto& [p, layers] : ends(r)) {
        const Contacts c = contacts(r, p, layers, true);
        work.insert(work.end(), c.movable.begin(), c.movable.end());
      }
    };
    for (const Ref& r : lifted_now) push_contacts(r);
    while (!work.empty()) {
      const Ref y = work.back();
      work.pop_back();
      if (lifted(y) || fixed(y)) continue;
      bool dead = false;
      for (const auto& [p, layers] : ends(y)) {
        const Contacts now = contacts(y, p, layers, true);
        const int n_now = static_cast<int>(now.movable.size()) + (now.stop ? 1 : 0);
        if (y.kind == 0 && n_now == 0) dead = true;
        if (y.kind == 1 && n_now <= 1) dead = true;
      }
      if (!dead) continue;
      set_lifted(y, true);
      extra.push_back(y);
      push_contacts(y);
    }
    return extra;
  }

 private:
  static constexpr Coord kTouch = 1000;  // 1 um: end points that KiCad would join although they are a few nm apart
  bool is_junction_via(Ref r) {
    if (r.kind != 1) return false;
    const auto e = ends(r);
    return contacts(r, e[0].first, e[0].second, false).movable.size() >= 3;
  }
  const model::Board& b_;
  CopperIndex ci_;
  std::vector<int> track_item_, via_item_;
  std::vector<std::uint8_t> lifted_t_, lifted_v_;
};

}  // namespace

int open_connections(const model::Board& b, const model::DesignRules& rules) {
  (void)rules;
  return open_of(b, pad_roots(b));
}

nlohmann::json RepairResult::summary() const {
  nlohmann::json st = nlohmann::json::array();
  for (const auto& s : steps)
    st.push_back({{"connection", s.conn}, {"radius_mm", s.radius_mm}, {"branches", s.pieces}, {"tracks", s.tracks}, {"vias", s.vias},
                  {"open_before", s.open_before}, {"open_after", s.open_after}, {"split", s.split}, {"kept", s.kept}, {"work", s.work}});
  int kept = 0;
  for (const auto& s : steps) kept += s.kept;
  return {{"open_before", open_before}, {"open_after", open_after}, {"attempts", steps.size()}, {"kept", kept},
          {"lifted_tracks", ripped_tracks.size()}, {"lifted_vias", ripped_vias.size()}, {"work", work}, {"seconds", seconds}, {"steps", st}};
}

RepairResult repair_open(const model::Board& base, const model::DesignRules& rules, const route::RouteResult& res, const route::RouterOptions& opt,
                         const RepairOptions& ro, const std::function<void(const std::string&)>& log) {
  const auto t0 = std::chrono::steady_clock::now();
  RepairResult out;
  out.tracks = res.tracks;
  out.vias = res.vias;
  out.unrouted = res.unrouted;
  out.failures = res.failures;
  std::vector<std::uint8_t> base_t_gone(base.tracks.size(), 0), base_v_gone(base.vias.size(), 0);
  // The current board: the base's tracks and vias that are left, then the new copper. src >= 0: index in base; < 0: -(k + 1), index k
  // in out.tracks / out.vias.
  std::vector<int> t_src, v_src;
  auto current = [&] {
    model::Board cur = base;
    cur.tracks.clear();
    cur.vias.clear();
    t_src.clear();
    v_src.clear();
    for (std::size_t i = 0; i < base.tracks.size(); ++i)
      if (!base_t_gone[i]) cur.tracks.push_back(base.tracks[i]), t_src.push_back(static_cast<int>(i));
    for (std::size_t i = 0; i < base.vias.size(); ++i)
      if (!base_v_gone[i]) cur.vias.push_back(base.vias[i]), v_src.push_back(static_cast<int>(i));
    for (std::size_t k = 0; k < out.tracks.size(); ++k) cur.tracks.push_back(out.tracks[k]), t_src.push_back(-static_cast<int>(k) - 1);
    for (std::size_t k = 0; k < out.vias.size(); ++k) cur.vias.push_back(out.vias[k]), v_src.push_back(-static_cast<int>(k) - 1);
    return cur;
  };
  std::set<NetId> keep(ro.keep_nets.begin(), ro.keep_nets.end());
  // Nets the router does not route here: their copper stays (lifted, it would never come back).
  for (const auto& n : base.nets) {
    if (n.id <= 0) continue;
    const bool skipped = std::find(opt.skip_nets.begin(), opt.skip_nets.end(), n.name) != opt.skip_nets.end();
    if (skipped || (!opt.only_net.empty() && n.name != opt.only_net)) keep.insert(n.id);
  }

  model::Board cur = current();
  std::vector<int> cur_root = pad_roots(cur);
  int cur_open = open_of(cur, cur_root);
  out.open_before = out.open_after = cur_open;
  auto graph = std::make_unique<CopperGraph>(cur);
  std::set<std::pair<int, int>> tried;
  int conns_tried = 0;
  for (bool progress = true; progress;) {
    progress = false;
    for (std::size_t ui = 0; ui < out.unrouted.size(); ++ui) {
      const route::RouteResult::Unrouted u = out.unrouted[ui];  // a copy: a kept attempt replaces the list
      if (conns_tried >= ro.max_conns) break;
      if (ro.time_limit_s > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > ro.time_limit_s) break;
      if (u.pad_a < 0 || !tried.insert({u.pad_a, u.pad_b}).second) continue;
      ++conns_tried;
      const NetId own = base.pads[static_cast<std::size_t>(u.pad_a)].net;
      // The window is measured from the pads' edges (centre plus half the pad's larger side).
      std::vector<std::pair<Point, Coord>> ends;
      for (const int pi : {u.pad_a, u.pad_b})
        if (pi >= 0) {
          const auto& pad = base.pads[static_cast<std::size_t>(pi)];
          ends.push_back({pad.pos, std::max(pad.size_x, pad.size_y) / 2});
        }
      const std::string label = u.net + " " + u.a + "-" + u.b;
      int last_count = -1;
      for (const double r_mm : ro.radii_mm) {
        // The window: every branch of another net with copper within r of an end of the connection, and what lifting them leaves dead.
        graph->clear_lifted();
        RepairStep st;
        st.conn = label;
        st.radius_mm = r_mm;
        st.open_before = cur_open;
        std::vector<CopperGraph::Ref> lifted;
        for (const auto& [e, half] : ends)
          for (const auto& seed : graph->near(e, half + static_cast<Coord>(r_mm * 1e6))) {
            const NetId n = graph->net(seed);
            if (n <= 0 || n == own || keep.count(n) || graph->fixed(seed) || graph->lifted(seed)) continue;
            const auto br = graph->branch(seed);
            if (br.empty()) continue;
            bool fresh = false;
            for (const auto& x : br)
              if (!graph->lifted(x)) graph->set_lifted(x, true), lifted.push_back(x), fresh = true;
            st.pieces += fresh ? 1 : 0;
          }
        const auto extra = graph->prune(lifted);
        lifted.insert(lifted.end(), extra.begin(), extra.end());
        if (lifted.empty() || static_cast<int>(lifted.size()) == last_count) continue;  // the same window as before: nothing new to try
        last_count = static_cast<int>(lifted.size());
        if (st.pieces > ro.max_branches) {
          if (log) log("repair: " + label + ": a " + fmt_mm(r_mm) + " mm window would lift " + std::to_string(st.pieces) + " branches (limit " +
                       std::to_string(ro.max_branches) + "); the window stops growing");
          break;
        }
        std::vector<std::uint8_t> t_lift(cur.tracks.size(), 0), v_lift(cur.vias.size(), 0);
        for (const auto& x : lifted) {
          if (x.kind == 0) t_lift[static_cast<std::size_t>(x.index)] = 1, ++st.tracks;
          else v_lift[static_cast<std::size_t>(x.index)] = 1, ++st.vias;
        }
        model::Board step = cur;
        step.tracks.clear();
        step.vias.clear();
        for (std::size_t i = 0; i < cur.tracks.size(); ++i)
          if (!t_lift[i]) step.tracks.push_back(cur.tracks[i]);
        for (std::size_t i = 0; i < cur.vias.size(); ++i)
          if (!v_lift[i]) step.vias.push_back(cur.vias[i]);
        route::RouterOptions o = opt;
        o.work_budget = ro.step_work;
        o.sink = nullptr;
        o.buffer_events = false;
        o.deadline = nullptr;
        o.warm = nullptr;
        o.start_routes = nullptr;
        o.warm_tracks.clear();
        o.warm_vias.clear();
        o.state_in.clear();
        o.state_out.clear();
        o.keep_final = false;
        o.live_name = "repair";
        if (u.pad_b >= 0) o.priority.insert(o.priority.begin(), {u.a, u.b});
        const route::RouteResult r = route::Router(step, rules, o).run();
        st.work = r.expansions;
        out.work += r.expansions;
        model::Board after = step;
        after.tracks.insert(after.tracks.end(), r.tracks.begin(), r.tracks.end());
        after.vias.insert(after.vias.end(), r.vias.begin(), r.vias.end());
        const std::vector<int> after_root = pad_roots(after);
        st.open_after = open_of(after, after_root);
        st.split = splits(cur_root, after_root);
        st.kept = !st.split && st.open_after < cur_open;
        if (log)
          log("repair: " + label + ": " + fmt_mm(r_mm) + " mm window, " + std::to_string(st.pieces) + " branches lifted (" + std::to_string(st.tracks) +
              " tracks, " + std::to_string(st.vias) + " vias); open " + std::to_string(cur_open) + " -> " + std::to_string(st.open_after) +
              (st.split ? ", joined pads came apart" : "") + (st.kept ? " (kept)" : " (not kept)"));
        out.steps.push_back(st);
        if (!st.kept) continue;
        // Commit: the lifted copper goes (the base's by index, the new copper by position), the attempt's routes are added.
        std::vector<model::Track> nt;
        std::vector<model::Via> nv;
        for (std::size_t i = 0; i < cur.tracks.size(); ++i) {
          const int s = t_src[i];
          if (t_lift[i] && s >= 0) base_t_gone[static_cast<std::size_t>(s)] = 1;
          if (!t_lift[i] && s < 0) nt.push_back(out.tracks[static_cast<std::size_t>(-s - 1)]);
        }
        for (std::size_t i = 0; i < cur.vias.size(); ++i) {
          const int s = v_src[i];
          if (v_lift[i] && s >= 0) base_v_gone[static_cast<std::size_t>(s)] = 1;
          if (!v_lift[i] && s < 0) nv.push_back(out.vias[static_cast<std::size_t>(-s - 1)]);
        }
        nt.insert(nt.end(), r.tracks.begin(), r.tracks.end());
        nv.insert(nv.end(), r.vias.begin(), r.vias.end());
        out.tracks = std::move(nt);
        out.vias = std::move(nv);
        out.unrouted = r.unrouted;
        out.failures = r.failures;
        out.changed = true;
        cur = current();
        cur_root = pad_roots(cur);
        cur_open = open_of(cur, cur_root);
        graph = std::make_unique<CopperGraph>(cur);
        progress = true;
        break;
      }
      if (progress) break;  // out.unrouted changed: start again from its first connection not tried yet
    }
  }
  out.open_after = cur_open;
  for (std::size_t i = 0; i < base_t_gone.size(); ++i)
    if (base_t_gone[i]) out.ripped_tracks.push_back(static_cast<int>(i));
  for (std::size_t i = 0; i < base_v_gone.size(); ++i)
    if (base_v_gone[i]) out.ripped_vias.push_back(static_cast<int>(i));
  out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return out;
}

}  // namespace tmk::passes

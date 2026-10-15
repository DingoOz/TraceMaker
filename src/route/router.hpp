// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// TraceMaker's detailed router, version 1 (roadmap M4; design doc 05 §2, §5).
//
// Octilinear A* on a fine lattice (optimal under its cost model), with legality decided lazily by exact
// clearance tests against the DRC's rule engine, then exact verification of every committed segment and via.
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core/events.hpp"
#include "model/board.hpp"
#include "model/rules.hpp"
#include "route/global_router.hpp"

namespace tmk::route {

// The copper of one routed connection, keyed by its two pads: what a later run can start from (RouterOptions::warm).
struct ConnRoute {
  int pad_a = -1, pad_b = -1;
  std::vector<model::Track> tracks;
  std::vector<model::Via> vias;
};

struct RouterOptions {
  Coord pitch = 0;              // lattice pitch; 0 = automatic from net-class widths and clearances
  // Auto pitch multiplier on small lattices (< 1M points per layer): a finer lattice finds fan-outs the default one
  // misses and small boards have the time for it (doc 05 §25).
  double pitch_scale_small = 1.0;
  bool fine_variant = true;      // portfolio: the cheap-vias variant routes small boards at half pitch
  double pitch_scale = 1.0;      // auto pitch multiplier, applied only on large lattices (>= 3M points per layer)
  double time_limit_s = 120;    // wall-clock safety limit for the whole run
  // Portfolio with more variants than threads: successive halving (Jamieson and Talwalkar, AISTATS 2016) instead of
  // running only `threads` variants. Every variant runs on a small budget, the better half runs again on twice as
  // much, until `threads` variants are left; each rung takes an equal share of the wall time (or work) that
  // `threads` variants would have used. Runs restart from scratch at each rung (a router run cannot be resumed).
  bool halving = false;
  long work_budget = 0;         // deterministic budget in search expansions (0 = none): same input + seed => same output
  // Per search attempt. One variant at a fixed budget prefers 1M (10 hard boards: 3M / 2M / 1M / 750k / 500k routed
  // 8,285 / 8,309 / 8,381 / 8,371 / 8,363), but the 8-variant 120 s tiers did not confirm it (tier B 5,167 -> 5,161,
  // tier C 9,523 -> 9,512 and 63.3 % -> 60.0 % clean), so it stays 3M (doc 05 §13; --max-expansions to experiment).
  long max_expansions = 3'000'000;
  bool reach_verify = false;        // test only: run the A* even where the pre-check proved no path, count mismatches
  int reach_check = 1;              // flood-fill reachability check before strict searches (exact): 0 off, 1 likely failures, 2 all
  bool bend_states = true;          // direction in the A* state (exact bend costs) vs. parent-direction approximation
  double heuristic_weight = 1.0;
  // Plane-aware routing of all-SMD boards (doc 05 §26), all opt-in:
  bool soft_zones = false;          // zone fills do not block other nets (refilled afterwards); unused fills become plane targets
  bool via_in_pad = false;          // inner balls / enclosed SMD pads may take a minimum-size via in the pad
  bool vias_off_pads = false;       // vias keep clear of SMD pads narrower than vias_off_pads_below_mm (except via-in-pad)
  double vias_off_pads_below_mm = 2.0;
  bool field_heuristic = true;      // GPU cost-to-go field as the A* heuristic on large windows
  int field_min_cells = 60'000;     // window size (lattice points x layers) from which the field is used
  int gpu_device = 0;              // GPU backend device for fields (-1 = CPU reference)
  int max_attempts = 4;         // per connection (window growth and learned blocks between attempts)
  int soft_attempts = 3;        // window sizes tried by negotiated searches
  double via_cost_mm = 3.0;     // equivalent track length of one via
  bool allow_vias = true;
  double polish_fraction = 0;   // share of the budget reserved for rebuilding the open connections of the best state (doc 16)
  int polish_trials = 20;       // seeded orders tried per open connection in the polish
  int lns_open_trials = 0;      // regional rebuild of open connections: seeded orders tried per open connection (0 = off, doc 16 F4)
  double lns_open_radius_mm = 2.5;  // copper within this distance of the open connection's first pad is lifted
  double layer_dir_pen = -1;    // negative = automatic (-1: 0.3; otherwise the strength is the absolute value) on breakout boards only, else 0; extra cost (pitches per orthogonal step) against a layer's preferred direction: even layers horizontal, odd vertical (doc 16)
  bool layer_dir_swap = false;  // preferred directions the other way round: even layers vertical, odd horizontal
  double package_pen = -1;      // -1 = automatic (3 on a two-layer board with a ball-grid package, else 0); pitches per lattice step for tracks on the pad layer inside a ball-grid package (doc 16 F3; 0 = off)
  bool layer_assign_angle = false;  // layer preference by chord direction instead of by crossings (uses layer_assign_pen)
  bool layer_assign = false;    // doc 16 F2: prefer, per connection, the layer that leaves the fewest crossings (two-layer boards)
  double layer_assign_pen = 0.4;  // pitches per lattice step on the other layer
  bool strict_first = true;     // the first pass is strict (no crossing of routed copper); off: negotiate from the first pass
  bool rip_up = true;           // negotiated rip-up and reroute (design doc 05 §6 rung R2, doc 06 §3)
  double history_weight = 2.0;  // PathFinder history: pitches of cost per recorded conflict on a cell
  int max_victims = 0;          // a negotiated route may rip at most this many connections at once (0 = no limit)
  double txn_temp = -1;         // transactional negotiation: a route that loses k connections net is kept with probability exp(-k / txn_temp) (0 = never, -1 = automatic: 1.5 on a breakout board)
  int txn_cool = 0;             // 1: the temperature falls linearly to zero over the work budget (simulated-annealing schedule)
  int stall_passes = 0;         // > 0: a restart begins when this many passes in a row set no new best for the run since its last restart
  long txn_cap = 0;             // transactional negotiation: expansion cap of each victim's re-route search (0 = none)
  int txn_depth = 0;            // transactional negotiation: victims that cannot be re-routed strictly get a nested transaction of their own, this deep
  int txn_nested = 4;           // nested transactions allowed per top-level transaction
  int field_cache_mb = 0;       // > 0: cost-to-go fields are kept (this many MB per router) and reused when every input is identical; results are unchanged
  int flood_work_pct = 100;     // reachability-flood cells count this percent of an expansion against the work budget (a flood cell costs far less time)
  int field_prefill = 0;        // 1: fill a class's fixed-copper cache for the whole board before its first field, so fields depend on nothing learned later (and can be cached)
  int tidy = 0;                 // > 0: every this many passes, with connections still open, each routed connection is re-routed alone and kept if shorter (frees room)
  int txn_orders = 1;           // transactional negotiation: orders tried for re-routing the victims (the ones that failed go first in the next try)
  int breakout_profile = 1;     // 1: a breakout board (D98) also gets reach_check 2, flood_work_pct 25, field prefill and cache (when those are at their defaults)
  const std::vector<ConnRoute>* warm = nullptr;  // start from these routes (a previous result's conn_routes): they are kept, negotiation starts at once
  std::string state_out;        // portfolio: after every round the best routes so far are written here (atomically), so an interrupted or out-of-time run can be resumed
  std::string state_in;         // portfolio: start from the routes saved in this file (written by state_out); a missing file is ignored
  bool relax_late = false;      // with relax: plain rounds under the board's rules first; when they stall, start the stages afresh (not from the stalled state), and keep the best legal result of all
  bool relax = false;           // portfolio: relax, route, legalise: ramp stages at 60 % and 80 % of the board's clearance with legalisation and breathing between (D104); needs round_work (20 M by default)
  double ramp_from_mm = 0;      // portfolio: > 0 starts at this clearance (every class and the board minimum lowered by the difference) and raises it by ramp_step_mm per stage up to the board's own; each stage warm-starts from the last one's routes that are still legal (clearance continuation, doc 16 §12.3)
  double ramp_step_mm = 0.005;  // clearance added per stage
  int ramp_rounds = 2;          // a ramp stage ends after this many rounds in a row without gain over the best round of the stage (or when complete)
  int ramp_cycle = 0;           // > 0: when the final stage (the board's rules) has not improved for ramp_stall rounds, go back this many stages and climb again ("breathing"); the best result under the board's rules is returned
  bool ramp_late = false;       // start at the board's rules and use the ramp (with ramp_cycle) only once those stall
  int ramp_fresh = 0;           // > 0: every n-th breathing cycle starts from nothing instead of the best state (a new topology; the best result under the board's rules is kept)
  int ramp_stall = 3;           // rounds without improvement at the final stage before breathing
  int island_slack = -1;        // portfolio rounds: >= 0 lets every variant continue the next round from its own final state when that routes at least (best - island_slack) connections, instead of all from the best (islands; the best state ever seen is returned)
  bool keep_final = false;      // also return the live state at the end of the run (RouteResult::final_routes)
  std::vector<model::Track> warm_tracks;  // copper of a routed board (--warm-pcb) to start from: a net with one connection takes all of its tracks and vias; only what is legal under this board's rules is kept
  std::vector<model::Via> warm_vias;
  bool legalize = false;        // warm start: first move the vertices of the taken-over routes until the board's clearances hold (route/legalize.hpp), then keep what is legal
  bool state_recheck = false;   // warm start: take over only the routes that are legal under this board's rules (continuation from looser rules)
  int breakout_portfolio = 1;   // 1: with more than one variant, a board that looks like a ball-grid breakout gets the breakout portfolio (D101) instead of the designed eight
  long round_work = 0;          // portfolio: > 0 gives every round this work budget (instead of splitting the total), and rounds continue until the board is complete, `rounds` is reached or `time_limit_s` of the whole portfolio is used (--until-complete)
  int rounds = 0;               // portfolio: the work budget is split into this many rounds; after each, every variant continues from the best result so far (0 = automatic: 4 with the breakout portfolio, else 1)
  int txn = -1;                 // transactional negotiation (doc 16 §12): a route that rips victims is kept only if the victims can be re-routed
                                // without ripping: 0 = off, 1 = keep ties (net 0), 2 = strict gain only, -1 = automatic (1 on a breakout board, else 0)
  int max_rips_per_connection = 40;  // was 8: at 50 M work on dense breakouts 33 % -> 54 % clean (doc 16 §4)
  int max_passes = 40;          // passes over still-unrouted connections (was 12)
  // Micro vias (outer layer to the next one, the net class's microvia size) where a through via is blocked, on
  // boards whose rules allow them (doc 05 §21).
  bool micro_vias = false;
  bool blind_vias = false;      // use blind/buried vias where a through via is blocked, if the board allows them
  bool diff_pairs = false;      // route P/N pairs together as coupled tracks first (falls back to single routing)
  // Component rules (doc 15 P3): these net pairs only are routed coupled first when diff_pairs is off (e.g. USB 2.0
  // D+/D- bound by tm::crules, whose names need not end in P/N or +/-). Empty = none.
  std::vector<std::pair<model::NetId, model::NetId>> pair_nets;
  // Intra-pair skew limit for pairs routed coupled (0 = only KiCad custom `skew` rules): the shorter half gets meanders
  // in the clean-up until the halves differ by at most half of it (length tuning code, doc 05 §15).
  // Coupled pairs may twist: change layer one half after the other and cross in between, so that a pair whose
  // pin order is mirrored between its ends can still be routed coupled (doc 05 §24).
  bool pair_twists = true;
  Coord pair_skew = 0;
  // Per-pair limits for RouterOptions::pair_nets (same index; 0 or missing = pair_skew): component rules give each
  // interface its own limit (USB 2.0 1.27 mm, doc 15 USB2-02).
  std::vector<Coord> pair_net_skew;
  bool global_route = false;    // plan every connection on a coarse tile graph first; detailed search follows the corridors
  // Global router v2 (M6): the first search of each connection is confined to its corridor (cells outside are
  // blocked, window cropped to the corridor); only if that fails do the usual unconfined windows run.
  bool global_confine = false;
  // Strict pass with corridors: a connection that cannot be routed inside its corridor goes straight to
  // negotiation instead of flooding the wide strict windows (on logicbone those failed floods were 76 % of all
  // search expansions; doc 05 §13).
  bool global_strict_corridor_only = false;
  // Global congestion map (M6, doc 05 §16): plan on the tile graph, then charge detailed steps through tiles the
  // plan fills beyond global_congestion_from eighths of capacity, except near the connection's own ends. No corridors.
  bool global_congestion = false;
  int global_congestion_from = 6;
  double global_congestion_pen = 0.5;  // pitches per lattice step, per eighth of capacity above the threshold
  // Connection planning measures pad copper to pad copper instead of centre to centre (doc 05 §17).
  bool plan_gap = true;
  bool optimize = true;         // post-routing clean-up: re-route connections to save vias and length
  bool escape_plan = false;     // reserve escape corridors for the pins of dense packages (route/escape.hpp, M9)
  bool escape_second_ring = false;  // second-ring balls escape between two outer balls instead of by dog-bone
  // Escape planning version 2 (route/escape_flow.hpp): deep ball-grid arrays get min-cost-flow channel and layer
  // assignment; other dense packages keep version 1's corridors. Implies escape_plan.
  bool escape_flow = false;
  // R6 cut proofs (doc 05 §20): measure every straight line across the board (capacity past fixed copper against
  // the nets that must cross) and fill RouteResult::over_cuts / tightest_cut. Costs the global router's set-up.
  bool cut_report = false;
  bool escape_report = false;   // fill RouteResult::escape_rings (pins of deep arrays per ring, and how many connected)
  int max_restarts = 6;         // full restarts (hardest first, history kept) when negotiation stalls
  double soft_growth = 0.0;     // crossing cost grows by this fraction of soft_cost_mm per negotiation pass (PathFinder, doc 16)
  double soft_cost_mm = 1.0;    // base cost of crossing another net's routed copper (before history)
  std::uint64_t seed = 1;
  int order = 0;                // connection order: 0 shortest first, 1 longest first, 2 shortest first with seeded jitter
  // Connections to route first ("REF.NUM" pairs, either orientation): learned from earlier failures (doc 06 T3).
  std::vector<std::pair<std::string, std::string>> priority;
  std::vector<std::string> first_nets;  // routed first, also after restarts; other nets do not rip them (crystal lines)
  // Buses (doc 05 §38): groups of nets routed lane by lane. The connections of a group between two components are
  // routed one after the other in the order of their pins, each steered along the neighbour routed before it, on
  // the same layers; nets outside the groups do not rip them. `skew` > 0: meanders in the clean-up bring the
  // members' lengths (tracks and via height, as KiCad's skew check) within it. Empty = off.
  struct Bus {
    std::string name;
    std::vector<model::NetId> nets;  // in bus order
    Coord skew = 0;
  };
  std::vector<Bus> buses;
  bool bus_first = false;  // the groups' connections before all others (after first_nets), also after restarts
  // Path guidance (doc 16 F1): a preferred path for a connection, found by matching its two pad centres. Cells off
  // the guide cost `guide_pen` pitches per lattice step in every search of that connection (a preference, never a
  // block), so a guide that does not pass the exact checks only costs search effort. `--guide FILE` fills it
  // from JSON (experiments); the breakout planner fills it itself.
  struct GuidePath {
    geom::Point a{}, b{};                       // pad centres of the two ends
    std::vector<std::array<geom::Point, 2>> segs;  // track segments
    std::vector<int> seg_layer;                 // copper layer index of each segment
    std::vector<geom::Point> vias;              // through vias (all layers)
  };
  std::vector<GuidePath> guides;
  double guide_pen = 0.1;       // pitches per lattice step off the guide
  double guide_radius = 1.5;    // pitches around the guide that count as on it
  std::string only_net;         // debugging: route only this net
  // Plan the connections and stop: RouteResult::connections and ::unrouted list every connection that is open (reason `not_attempted`), nothing is
  // searched or committed. A board session (doc 19) lists the open connections of a state this way. Off by default: a run is what it was.
  bool plan_only = false;
  std::vector<std::string> skip_nets;  // experiments: leave these nets unrouted (e.g. nets a designer would pour)
  // Layer limits (doc 05 §29; route/layer_limits.hpp turns layer names into these). Layers in no_track_layers get
  // no new tracks: vias still pass through them and still connect into zone fills there, and pads that exist only
  // on such layers stay unrouted. layer_cost_pm[l] is the cost of a track on copper layer l per mille of its
  // length (1000 = normal, never less; missing entries are 1000).
  model::LayerMask no_track_layers = 0;
  std::vector<std::int32_t> layer_cost_pm;
  // Critical nets (issue #24, doc 05 §44; route_job fills these from --critical-nets and the component rules' roles). `critical` is net name and tier
  // (1 = most critical). With `critical_first` they are routed before all other nets, tier by tier (after first_nets), also after restarts. A via
  // costs them `critical_via_mult` times as much (>= 1). A track of theirs over a void of the adjacent reference plane (route/ref_map.hpp) costs
  // `split_cost` pitches more per lattice step; `split_hard` closes those cells to them instead. All off by default: a run without these options is
  // the run it was.
  std::vector<std::pair<std::string, int>> critical;
  bool critical_first = false;
  double critical_via_mult = 1.0;
  double split_cost = 0;
  bool split_hard = false;
  double split_min_coverage = 0.3;  // a layer is a reference plane when its zone fills cover this share of the board
  // The board with its zones refilled by the engine, for the reference map: made once by the caller when the file's zones have no fills (every portfolio variant
  // would otherwise refill the whole board itself). Null = use the board's own fills, refilling here if it has none.
  std::shared_ptr<const model::Board> reference_fills;
  // Rule areas and custom rules that exist only in this router's working copy of the board and rules (advice regions, doc 17
  // §4): never written to the output, judged by the same exact checks as the board's own positional rules (doc 05 §34).
  std::vector<model::Zone> extra_zones;
  std::vector<model::CustomRule> extra_rules;
  // Hints from an advice file (src/advice, doc 17). They are not applied to the variants the portfolio would run
  // anyway: route_portfolio adds `variants` extra variants that carry them (the next ones of its list, the variants a plain run
  // with that many more would have added), so the winner of the total order can only be better than without advice,
  // never worse (rule 4).
  struct AdviceSet {
    std::string source;                                         // the advice file, for messages
    std::vector<std::pair<std::string, std::string>> priority;  // "REF.NUM" pairs routed first (as RouterOptions::priority)
    std::vector<std::string> first_nets;                        // as RouterOptions::first_nets
    model::LayerMask no_track_layers = 0;                       // added to RouterOptions::no_track_layers
    std::vector<std::int32_t> layer_cost_pm;                    // per copper layer, per mille (1000 or more); replaces entries that are lower
    std::vector<model::Zone> zones;                             // regions: rule areas (no keep-out flags) ...
    std::vector<model::CustomRule> rules;                       // ... and the disallow rules that name them and their nets
    int variants = 4;                                           // advised variants added to the portfolio (at most the variants that run)
    bool active() const {
      return !priority.empty() || !first_nets.empty() || no_track_layers != 0 || !layer_cost_pm.empty() || !zones.empty() || !rules.empty();
    }
  } advice;
  events::Sink* sink = nullptr;
  // Shared by portfolio variants (wall-clock mode only): when one variant has routed everything at time T, the
  // others may continue until 2T + 5 s; a variant that is complete always finishes its clean-up.
  std::atomic<double>* deadline = nullptr;
  // Recording: every variant buffers its events and only the winning variant's are forwarded to `sink`
  // (each message gains a leading "t" field, seconds since that variant started).
  bool buffer_events = false;
};

struct Connection {
  model::NetId net = 0;
  int pad_a = -1, pad_b = -1;   // board pad indices (pad_b = -1 when the target is a zone fill)
  int zone_b = -1;              // copper item index of a zone fill (plane) to connect into, or -1
  Coord length = 0;             // straight-line distance
};

// Work profile (doc 10 §2, D88): how many of each costly operation a run did, per phase. The counts depend only on
// the board, the options and the work budget, so the same job gives the same profile on any machine and at any
// thread count. The --work budget charges expansions and flood cells; the other counts are what it does not charge.
struct WorkCounts {
  long expansions = 0;     // A* states expanded (strict, negotiated and coupled-pair searches)
  long flood_cells = 0;    // cells visited by the reachability pre-check
  long searches = 0;       // A* searches and pre-checks started
  long cell_checks = 0;    // lattice points whose legality a search evaluated (per-search cache misses)
  long fixed_checks = 0;   // fixed-copper legality computations (per-class cache misses, tracks and vias)
  long field_cells = 0;    // cost-to-go field cells computed (window x layers per field)
  long fields = 0;
  long commits = 0;        // found paths checked exactly before committing
  long rips = 0;           // routed connections ripped up by negotiation
  WorkCounts& operator+=(const WorkCounts& o);
};
enum class WorkPhase { Setup, FirstPass, Negotiation, Cleanup };  // setup: plan, escape reservations, coupled pairs
inline constexpr int kWorkPhases = 4;
const char* work_phase_name(WorkPhase p);
struct WorkProfile {
  std::array<WorkCounts, kWorkPhases> phase{};
  WorkCounts total() const;
};

struct RouteResult {
  std::vector<model::Track> tracks;  // new copper, in commit order
  std::vector<model::Via> vias;
  std::vector<ConnRoute> conn_routes;  // the same copper per connection (best state), for a warm start
  std::vector<ConnRoute> final_routes;  // the state the search ended in (RouterOptions::keep_final): legal, perhaps worse than the best
  int final_routed = 0;
  int connections = 0, routed = 0;
  long expansions = 0;
  long pair_work = 0;           // the part of `expansions` spent in coupled pair searches (closed states and leg checks)
  int rips = 0, passes = 0;
  int enclosed = 0;             // searches that proved the source boxed in (no larger window tried)
  long nogood_skips = 0;        // attempts skipped because an identical attempt already failed
  int necked = 0;               // connections routed at the neck-down width
  int restarts = 0;
  int lns_fixed = 0;            // open connections closed by the regional rebuild (doc 16 F4)
  int tidied = 0;               // connections shortened by tidy sweeps
  int txn_tried = 0, txn_kept = 0;  // transactional negotiation: routes with victims tried, and kept
  int optimized = 0;
  int pairs = 0;
  int length_tuned = 0;
  // Buses: connections routed as a lane of their group (leaders and the members that followed one), and connections
  // of a group routed otherwise ("left the bus"), in the state returned.
  int bus_lanes = 0, bus_left = 0;
  int micro_vias = 0;           // micro vias placed
  int blind_vias = 0;           // blind/buried vias placed
  int escape_corridors = 0;     // escape corridors reserved (M9)          // nets brought into their custom length range by meanders                // differential pairs routed coupled            // connections improved by the clean-up pass
  // With escape_report: per ring of the deep arrays (index 0 = perimeter), {pins to route, pins with every
  // connection routed}.
  std::vector<std::pair<int, int>> escape_rings;
  // With cut_report: lines across the board that more nets must cross than tracks fit, and the tightest line.
  std::vector<CutLine> over_cuts;
  CutLine tightest_cut;
  double seconds = 0;
  WorkProfile work;
  Coord pitch = 0;
  std::vector<std::string> failures;  // one line per unrouted connection
  // One unrouted connection of the state returned, structured for the stuck report (doc 17 §3): where it is, why the
  // last attempt failed (a code, and the search's own words), how often it was ripped or failed, and which nets stood in the way.
  struct Unrouted {
    std::string net, a, b;  // "REF.NUM" (b = "zone" for plane connections)
    std::string reason;     // code: boxed_in, exact_check, rip_limit, ripped_not_rerouted, layer_limit, out_of_budget, rolled_back, not_attempted, no_legal_path
    std::string why;        // the last explanation as the router wrote it (net and pad names are the board's)
    int rips = 0, fails = 0;
    int pad_a = -1, pad_b = -1;       // board pad indices (pad_b = -1: a zone fill)
    geom::Point pos_a{}, pos_b{};     // pad centres (pos_b = pos_a for a zone)
    std::vector<std::string> blockers;  // nets whose routed copper the last rejected route would have had to rip (at most 6)
  };
  std::vector<Unrouted> unrouted;
};

// The reason code of a failure explanation (RouteResult::Unrouted::reason). The text is matched, not parsed: the
// router's explanations are sentences, and the code is only the class of the cause.
std::string failure_reason_code(const std::string& why);

// Runs several differently configured routers (portfolio variants) and keeps the best result. Variant i is a pure
// function of (base options, i): it never depends on the thread count, and with a work budget each variant's result
// is deterministic. The variants are scheduled on `threads` worker threads (0 = one per variant) in index order and
// the winner is the minimum of a total order (most connections routed, then fewest vias, then shortest copper, then
// the lowest variant index), so with `base.work_budget` > 0 the output is the same for any `threads` (requirement
// N3, decision D47). Only the first variant streams events live.
struct PortfolioResult {
  RouteResult best;
  int best_variant = 0;               // position in `variants`
  std::vector<int> indices;            // portfolio variant index per position
  std::vector<std::string> variants;   // description per variant
  std::vector<int> routed;             // routed count per variant
  std::vector<double> seconds;         // wall time per variant (its own clock)
  std::vector<WorkProfile> work;       // work profile per variant
};
// `variants`: how many variants to run (the first `variants` of the portfolio); `pick`: which variant indices to
// run instead (non-empty: overrides `variants`). `threads`: concurrency only (0 = one thread per variant).
PortfolioResult route_portfolio(const model::Board& board, const model::DesignRules& rules, const RouterOptions& base, int variants,
                                const std::vector<int>& pick = {}, int threads = 0);
int portfolio_size();
// The portfolio's total order on results: true when `a` beats `b` (more routed, then fewer connections off their bus lanes, then
// fewer vias, then shorter copper). Ties are not "better": the caller keeps the earlier result.
bool route_result_better(const RouteResult& a, const RouteResult& b);
// Variant indices from here up are the advised copies the portfolio adds when RouterOptions::advice is active (position
// k of the advice's variants has index kAdvisedIndex + k).
inline constexpr int kAdvisedIndex = 1000;

class Router {
 public:
  Router(const model::Board& board, const model::DesignRules& rules, RouterOptions opt);
  RouteResult run();

 private:
  struct Impl;
  const model::Board& in_;
  const model::DesignRules& rules_;
  RouterOptions opt_;
};

}  // namespace tmk::route

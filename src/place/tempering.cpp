// Parallel tempering (replica exchange) for detailed placement (design doc 04 §3 E).
//
// Swendsen & Wang, "Replica Monte Carlo simulation of spin glasses", PRL 57 (1986); Hukushima & Nemoto,
// "Exchange Monte Carlo method and application to spin glass simulations", J. Phys. Soc. Jpn. 65 (1996).
// R replicas hold R temperatures on a geometric ladder. The whole ladder follows the annealing schedule
// (T0 → 1e-4·T0 times a fixed factor per slot), i.e. annealed replica exchange: a fixed ladder from T0 to
// 1e-4·T0 accepted only ~3 % of the exchanges with 4 replicas and cost 5 % more than independent runs. Every round each replica runs a fixed number of moves at its temperature
// (in parallel threads); then neighbouring temperatures (even pairs on even rounds, odd pairs on
// odd rounds) swap replicas with probability min(1, exp((1/T_i − 1/T_j)(E_i − E_j))). The exchanges are decided
// by one thread at the barrier from a dedicated random stream, and each replica's moves depend only on its own
// stream and state, so the result is identical for any thread count.
#include <algorithm>
#include <barrier>
#include <cmath>
#include <memory>
#include <thread>

#include "place/anneal_state.hpp"

namespace tmk::place::detail {

namespace {
constexpr std::uint32_t kStageExchange = 0x504C5845u;  // "PLXE"
// Ladder (hottest / coldest, and the hottest slot relative to the annealing temperature as a power of the span),
// tuned on 8 PCBench boards at equal move budgets (doc 04 §8): refine starts cool (p0 = 0.05) and gained most from
// a ladder reaching 16x hotter (cost −8 % vs independent runs); full starts hot (p0 = 0.3), where a ladder
// centred on the schedule tied with independent runs (−0.3 %) and hotter ladders lost.
constexpr double kSpanRefine = 16.0, kTopRefine = 1.0;
constexpr double kSpanFull = 8.0, kTopFull = 0.5;
}  // namespace

AnnealResult anneal_tempering(const Problem& p, const std::vector<std::vector<int>>& pn, const Placement& start, const AnnealOptions& o,
                              std::uint64_t moves) {
  AnnealResult res;
  const int R = std::max(2, o.runs);
  std::vector<std::unique_ptr<Annealer>> an(z(R));
  for (int r = 0; r < R; ++r) an[z(r)] = std::make_unique<Annealer>(p, pn, start, o, r);

  const double t0 = an[0]->estimate_t0(o.refine ? 0.05 : 0.3);
  const double ln_end = std::log(1e-4);  // the annealing schedule's end temperature ratio
  std::vector<Slot> slots(z(R));
  std::vector<double> factor(z(R));
  const double span = o.refine ? kSpanRefine : kSpanFull, top = o.refine ? kTopRefine : kTopFull;
  std::vector<int> holder(z(R)), slot_of(z(R));
  for (int i = 0; i < R; ++i) {
    factor[z(i)] = std::pow(span, top - static_cast<double>(i) / (R - 1));
    slots[z(i)].t = t0 * factor[z(i)];
    slots[z(i)].radius = static_cast<double>(an[0]->r0());
    holder[z(i)] = slot_of[z(i)] = i;
  }
  const std::uint64_t interval =
      o.exchange_interval > 0 ? static_cast<std::uint64_t>(o.exchange_interval) : std::clamp<std::uint64_t>(moves / 200, 500, 20'000);
  const std::uint64_t rounds = (moves + interval - 1) / interval;
  const RngStream ex(o.seed, kStageExchange, 0);
  std::uint64_t ex_ctr = 0, round = 0;
  bool stop = false;
  const bool has_deadline = o.time_limit_s > 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long>(o.time_limit_s * 1000));

  auto exchange = [&]() noexcept {
    for (int i = static_cast<int>(round & 1); i + 1 < R; i += 2) {
      const int a = holder[z(i)], b = holder[z(i + 1)];
      const double ea = static_cast<double>(an[z(a)]->cost()), eb = static_cast<double>(an[z(b)]->cost());
      const double arg = (1.0 / slots[z(i)].t - 1.0 / slots[z(i + 1)].t) * (ea - eb);
      const double u = ex.uniform(ex_ctr++);
      ++res.exchanges_tried;
      if (arg >= 0 || u < std::exp(arg)) {
        std::swap(holder[z(i)], holder[z(i + 1)]);
        slot_of[z(holder[z(i)])] = i;
        slot_of[z(holder[z(i + 1)])] = i + 1;
        ++res.exchanges_accepted;
      }
    }
    ++round;
    const double frac = static_cast<double>(round) / static_cast<double>(rounds);
    for (int i = 0; i < R; ++i) slots[z(i)].t = t0 * std::exp(ln_end * frac) * factor[z(i)];
    if (has_deadline && std::chrono::steady_clock::now() > deadline) stop = true;
  };
  const int T = std::clamp(o.threads, 1, R);
  std::barrier sync(T, exchange);
  std::vector<std::thread> pool;
  for (int t = 0; t < T; ++t)
    pool.emplace_back([&, t] {
      for (std::uint64_t k = 0; k < rounds; ++k) {
        if (stop) break;  // written only in the completion step, read after the barrier: same for every thread
        const std::uint64_t n = std::min(interval, moves - k * interval);
        const bool lns = static_cast<double>(k) >= (1.0 - kLnsPhase) * static_cast<double>(rounds);
        for (int r = t; r < R; r += T) {
          an[z(r)]->set_lns(lns);
          an[z(r)]->sweep(slots[z(slot_of[z(r)])], n);
        }
        sync.arrive_and_wait();
      }
    });
  for (auto& th : pool) th.join();
  res.time_limited = stop;
  for (int r = 0; r < R; ++r) {
    const auto& a = *an[z(r)];
    res.run_costs.push_back(a.best_cost());
    res.moves += a.moves();
    res.accepted += a.accepted();
    res.illegal += a.illegal();
    res.lns_tried += a.lns_tried();
    res.lns_improved += a.lns_improved();
    if (res.best_run < 0 || a.best_cost() < res.cost) {
      res.best_run = r;
      res.cost = a.best_cost();
    }
  }
  const auto& best = *an[z(res.best_run)];
  res.pl = best.best();
  res.start_cost = best.start_cost();
  res.trace = best.trace();
  finish_result(p, o, res);
  return res;
}

}  // namespace tmk::place::detail

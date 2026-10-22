---
status: current
code: [bench, scripts/fetch_fixtures.sh]
---

# Fetch boards and run benchmarks

The method, the sets and the gates are explained in [Benchmarking](../10-benchmarking.md); the results kept so far
are in [Benchmark results](../reference/benchmarks.md). This page is the recipe.

## Fetch the boards

```bash
scripts/fetch_fixtures.sh                 # freerouting, dac2020, kicad-demos, derived (about 2.4 GB), once
scripts/fetch_fixtures.sh tscircuit       # tscircuit's BGA breakout problems (srj19, srj20)
scripts/fetch_fixtures.sh pcbworld        # PCBWorld-Bench
python3 bench/check_env.py                # what is there and what is missing
```

The boards go to `bench/data/`, which git ignores. They are **never committed**: about half of them state no
licence. The default sets are pinned to the commits the published results were measured on;
`TM_FIXTURES=latest` takes upstream's current state instead. PCBench (1,158 boards, each with `.kicad_pcb` and
`.dsn`) ends up in `bench/data/freerouting/scripts/benchmark/fixtures/PCBench`.

## The quick tier

Run it before finishing any change to routing or placement:

```bash
python3 bench/run.py --set quick --work 1000000 --route-args "--variants 1" --jobs 4 --name my-change-quick
```

Thirty PCBench tier-A boards with a deterministic work budget, about 30 s. Compare with a run of the unchanged
program on the same machine; no board may get worse in clean pass or completion
([doc 10 §5](../10-benchmarking.md)). For changes to search, plans or rip-up also run `--set mid --work 20000000`.

## Tiers A–D (release numbers)

```bash
python3 bench/run.py --tier B --limit 40 --time 120 --threads 8 --jobs 2 --name my-run-tierB
```

`--limit 40` for A and B, 30 for C and 40 for D (the whole tier, 22 boards), with the default seed 1, give the same
boards as the release records. Without `--limit` the default of 50 picks a different set.

**Timed runs depend on machine load.** A loaded machine can halve the search steps in 120 s. To tell a regression
from load, run the previous release's binary beside the new one at the same time (the release 0.10.0 record shows
how).

## Results

Each run writes `bench/results/<run>/` (git-ignored): the routed boards, `boards.jsonl` with one result per board,
`report.md` and `summary.json` with `set`, `boards`, `clean_pass`, `completion` and `seconds`, which the progress
dashboard shows. `python3 bench/compare_runs.py <run> <run>` compares two runs board by board.

The judge is `kicad-cli` 10.0.6: a board is clean when every connection is made and no physical violation was
added against the input board ([D106](../decisions/D106.md)). A board is *unknown*, never clean, when an error type
is at KiCad's report cap before or after.

## Keep the results

`bench/results/` is not in the repository. Before a milestone or a change is called done, copy what it rests on into
`bench/records/<name>/`: the run folders' `summary.json`, `boards.jsonl` and `report.md`, the option or advice files
used, test output, and a `README.md` with the tables and what they mean. Then regenerate the results page:

```bash
python3 scripts/gen_reference.py bench
```

Report losses as plainly as gains, and say what was not measured.

# 03 — Core data structures, spatial indexing and DRC

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md).
> Rule for every structure here: **speed without loss of quality**. Accelerators may only prune work that
> an exact test would also reject; the exact test always has the final word.

## 1. Geometry kernel (`src/geom`)

- Coordinates: `int64` nanometres (KiCad's internal unit is 1 nm, so file round-trips are exact).
- Primitives: point, segment-with-width (stadium), arc-with-width, circle, octilinear polygon,
  general polygon with holes.
- Exact integer predicates (orientation, segment intersection, point–segment distance squared in
  `__int128`), no epsilons.
- Clearance between two primitives = exact distance minus half-widths, compared with the rule value.
- Polygon booleans and offsets: Clipper2 (Boost licence).
- Struct-of-arrays storage of segments/vias/pads per layer for cache-friendly scans and cheap GPU upload.

## 2. Spatial indices (`src/index`)

| Structure | Holds | Query | Why this one |
|---|---|---|---|
| **R-tree** per layer (Boost.Geometry, R* split; packed STR bulk-load for static objects) | All copper, keepouts, outline; bboxes bloated per clearance class | Window, nearest | Dynamic, general shapes, exact follow-up |
| **Uniform spatial hash** (cell = max clearance + max feature size) | Moving or numerous small items: tracks during routing, airwires during placement | Broad-phase pairs, crossing counts | O(1) insert/remove, trivially parallel, GPU-friendly |
| **Bit-packed layer grids** (`uint64` words, one plane per layer × clearance class) | Conservative rasterised obstacles for the lattice router; occupancy for placement | Free test, BFS waves with word-wide ops | 64 cells per instruction on CPU, coalesced on GPU |
| **Owner grids** (`uint32` net id per cell, sparse by tile) | Which net owns a lattice cell | Rip-up victim lookup | |
| **Corner-stitched tile planes** (later phase) | Gridless free space per layer | Tile walk for gridless search | Contour; space-efficient on large empty areas |
| **Global tile graph** (CSR, edge arrays SoA) | Capacity, usage, history per edge | GPU sweeps, A* | Contiguous, GPU-uploadable |
| **LBVH** (Karras 2012) on GPU | Static copper for DRC broad-phase and field computation | Parallel overlap queries | Built in O(n) on GPU each stage |

Incrementality: every index supports `insert/remove` inside a transaction overlay. Committing a
transaction applies its delta; rolling back drops the overlay.

## 3. Hash tables and caches

| Use | Structure |
|---|---|
| Id → object, nogood store, rule cache, airwire crossing pairs | `ankerl::unordered_dense` (or `absl::flat_hash_map`) — open addressing, robin-hood, fast iteration |
| Region signatures | 64-bit Zobrist keys per (4×4 block, layer, owner class), XOR-maintained |
| Rule lookup | Dense tables `clearance[classA][classB][layer]` compiled once; custom-rule predicates only for items whose conditions match (precomputed bitmask per item) |
| Pair-clearance memo for DRC | Hash of (object id pair, version) → result, cleared per dirty region |
| GPU hash maps | cuCollections `static_map` where supported, else a simple linear-probing table in CUDA |

**Never iterate a hash map in an order that affects output** (determinism rule). Sort keys first.

## 4. Search structures

- **Radix heap** for A* with monotone integer keys; **bucket (Dial) queue** for small-integer-cost BFS.
- **Generation-stamped arrays** for visited/g-values per window: reset is a single counter increment.
- **Lookahead tables**: per layer-pair minimal via-cost to change layers; per (dx, dy) octile distances.
- **GPU cost-to-go fields** (doc 07) cached per (target, window, cost version) in a size-bounded LRU.

## 5. Connectivity

- **Union-find with rollback** (union by rank, no path compression inside transactions) tracks which pads of
  a net are connected. Used for completion counts, the score, and for picking the next two-pin connection.
- **Incremental dynamic MST per net** over unconnected sub-trees: next connection = cheapest edge between
  components (Euclidean, then refined by global routing).

## 6. DRC (`src/drc`)

The own DRC must match `kicad-cli pcb drc` (verified per KiCad version in CI on fixture boards).

1. **Broad phase**: candidate pairs from the uniform spatial hash (CPU) or LBVH (GPU), restricted to dirty
   regions during routing, full board at sign-off.
2. **Rule resolution**: per pair, compiled constraint table + matching custom rules → required clearance.
3. **Narrow phase**: exact integer distance between primitives.
4. **Checks** (KiCad names): `clearance`, `track_width`, `via_diameter`, `annular_width`, `hole_clearance`,
   `hole_to_hole`, `copper_edge_clearance`, `shorting_items`, `unconnected_items`, `solder_mask_bridge`,
   `courtyards_overlap` (placement), `text`/graphics clearance, `disallow` (keepouts), `length`/`skew`
   (phase 3), dangling tracks/vias.
5. **Both-sides rule**: any check triggered by new geometry also re-checks the other object of every pair.
6. Output: markers with KiCad's violation type names, so reports compare 1:1 with `kicad-cli`.
7. **Broken boards** (KiCad 10 semantics, D45; regression test `drc_broken_parity`): the checks run on the nets
   KiCad assigns when it loads a board. KiCad links connectivity items of *any* nets whose copper touches (except
   two pads/zone fills of different nets) and propagates the pads' net to the tracks, vias and board graphics of
   every cluster whose pads carry a single net; a via touching only zone fills takes a zone's net. So a stray via
   on another net's track is not a short, while a track bridging two nets' pads is. Classification: copper
   overlapping a zone fill is `clearance` (gap 0); a track or via overlapping a pad, track or via of another net
   is `shorting_items` even when one side is net-less; two pads short only when both have nets. Dangling tracks
   and vias are judged on the any-net graph (a track ending on another net's copper is not dangling; a via is
   dangling when all its links start on one layer). Same-net items join where their copper overlaps, not only
   at end points. Free vias (`(free yes)`) keep their net. A zone's own clearance is a local clearance (max
   with the net class) for its fill; an item is reported once per zone and layer. KiCad tests vias per copper
   layer, so a via against a through pad or via is reported on each common layer (hole clearances too, plus
   the pad loop's via-hole test when the hole clearance is positive); via and pad holes are tested against
   other nets' fills. Tracks and vias are tested against holes, and copper against the board edge, even at
   zero clearance; slots are left out of hole-to-hole. Copper of a net may touch a net-tie footprint's items
   on its own net-tie pad. Harness: `scripts/drc_broken_parity.py` injects defects (`tracemaker
   selftest-defects`: stubs, stray and stitched vias, cut and shortened tracks, crossings, overlaps, vias on
   other nets' tracks and fills, tracks into pads) and matches violations one to one (`--base-delta` for bases
   that are not themselves at parity). Also KiCad's: a via or round pad wholly inside another net's plain
   rectangular pad is `clearance` (its circle-in-rectangle gap is not 0); a via written without a drill takes
   its net class drill for size and hole-to-hole tests but has a point hole for hole clearances. Documented
   exceptions: TraceMaker's DRC does not check copper text, so KiCad violations involving text are excluded;
   a net-less footprint graphic touching a track is a short or a clearance violation in KiCad depending on the
   two items' UUID order (TraceMaker: clearance).

## 7. Memory management

- Arena allocators per transaction and per stage (`std::pmr::monotonic_buffer_resource`); rollback and
  stage end free in O(1).
- Board-scale arrays (lattice grids, fields) in huge-page backed pools (`madvise(MADV_HUGEPAGE)`); the 150 GB
  RAM budget allows keeping all layers' grids, owner grids and several portfolio branches resident.
- Pinned host memory for GPU transfers; device memory pools via `cudaMallocAsync`.

## 8. Rough memory budget (dense 300 × 300 mm, 12 layers, 25 µm lattice)

| Item | Size |
|---|---|
| Obstacle bit-planes, 3 clearance classes | 3 × 216 MB = 650 MB |
| Owner grid (sparse tiles, ~30% touched) | ~2 GB |
| Learned-penalty and history (per 4×4 block, `uint16`, two maps) | ~430 MB |
| Search state per thread (window-sized) | ~50 MB × 56 = 2.8 GB |
| Portfolio branches (overlays) × 8 | ~4 GB |
| **Total** | **< 12 GB**, well inside 150 GB; V100 holds a full board's planes for field computation |

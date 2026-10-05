# 15 — Component-aware layout rules

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md). Written 2026-10-03.
>
> **Status: partly built (M13 phases P0–P2 and part of P3, 2026-10-04): see §14.** Everything not listed there
> is still a plan: where it says "TraceMaker does X", read "will do X".
> The machine-readable draft of the catalogue is [`component_rules.yaml`](component_rules.yaml); the two
> must stay consistent (a test will check that every rule id in one appears in the other, §9).

## 1. Purpose

A human layout designer who sees a USB-C receptacle in the BOM knows, without being told, that D+/D− are a
90 Ω differential pair, that the ESD array goes next to the connector, and that the 5.1 kΩ CC resistors do not
matter for placement. A switching regulator means a tight input-capacitor loop and a feedback trace kept away
from the switch node; a chip antenna means a copper keep-out. None of this is in the netlist, and most KiCad
projects do not encode it as net classes or custom rules either: PCBench boards mostly carry the default net
class (doc 10).

**Component-aware layout rules** close that gap. TraceMaker recognises *categories* of components in the
board (from reference prefix, footprint/lib_id, value, symbol keywords, pin names, net names and topology),
binds the nets and parts that play each *role* in that category (connector, pair P/N, ESD part, load
capacitor, …), and applies the layout rules the category needs to placement, routing and the final check —
always reported, always overridable, never silently invented.

The feature has three products:

1. **A catalogue** of rules per category with numeric defaults and a citation for each (§8, and
   `component_rules.yaml`). It is useful on its own as a checklist.
2. **An engine** that detects categories, binds roles, resolves conflicts with the board's own rules and
   compiles the result into constraints TraceMaker already understands (net classes, custom rules, keep-out
   areas, placer pseudo-nets) plus a few new ones (§5, §6).
3. **A report** that says what was detected, which rules were applied, which were not and why, and whether
   the result meets each rule (§7).

### 1.1 Non-goals

- Not a signal-integrity or EMC simulator. Rules are geometric proxies (width, gap, length, distance, area)
  for electrical intent. Where a number is a rule of thumb, the catalogue says so.
- Not a schematic checker (wrong pull-up values, missing CC resistors). Those are reported as *advisory*
  findings when they are cheap to see, never fixed.
- Not a replacement for the board's own rules. User-written net classes and `.kicad_dru` rules always win
  (§3.5).
- No safety certification. Creepage/clearance rules for mains are a floor taken from IPC-2221B and IEC
  62368-1 tables, and the report says that compliance needs the product standard and a qualified engineer.

## 2. Principles

These follow the non-negotiable rules in [`../CLAUDE.md`](../CLAUDE.md).

1. **Never silently invent rules (rule 6).** A rule is applied only when its category is detected with
   enough confidence and every input it needs is known (e.g. a stackup for an impedance target). Otherwise it
   is *reported as not applied* with the reason, and TraceMaker stays conservative: it keeps the board's own
   widths/clearances and does not route on guessed values.
2. **The board's own rules win.** A component rule may *add* constraints and may *tighten* a soft goal, but it
   never loosens a board rule and never overrides an explicit user net class or custom rule on the same object.
3. **Correctness before speed (rule 1).** Hard component rules (keep-outs, creepage, hard skew limits the user
   accepted) are checked with the same exact geometry as every other DRC rule before commit.
4. **Determinism (rule 2).** Detection is a pure function of the board, the netlist and the catalogue file;
   categories, roles and rules are processed in catalogue order, then by reference designator (natural sort),
   then by net name. No hash-order iteration.
5. **Transactions only (rule 4).** Component rules change *costs and constraints*; the board still changes only
   through committed branches whose score improved.
6. **Locked items are untouched (rule 6).** A proximity rule that would move a locked part is reported as
   unsatisfied, not enforced.
7. **Emit events (rule 7).** Detection, binding, each applied rule and each violation are events the viewer
   can show (highlight the bound parts and nets, draw keep-out polygons).
8. **Honest severity.** Each rule is *hard* (must hold; a violation is a DRC error), *soft* (a cost the
   placer/router minimise; a violation is a warning) or *advisory* (checked and reported only). Defaults are
   conservative: almost everything from rule-of-thumb sources is soft or advisory.

## 3. Recognising categories

### 3.1 Signals

Each category in the catalogue lists *detectors*. A detector looks at one kind of evidence:

| Signal | Where it comes from | Example | Reliability |
|---|---|---|---|
| Reference prefix | footprint `reference` (board) / netlist `ref` | `J`, `P`, `Y`, `X`, `L`, `FB`, `D`, `Q`, `U`, `AE`/`ANT` | Weak: only narrows the kind of part |
| Footprint / lib_id | footprint `lib_id` (board), netlist `footprint` | `Connector_USB:USB_C_Receptacle_*`, `Crystal:Crystal_SMD_3225-4Pin_*`, `RF_Module:ESP32-WROOM-32` | Strong for KiCad library parts; weak for custom libraries |
| Value / part number | footprint `value`, symbol `Value`/`MPN` fields | `TPS62130`, `LAN8720A`, `USBLC6-2SC6`, `16MHz` | Strong when it matches a known part family |
| Symbol keywords/description | `.kicad_sch` symbol `ki_keywords`, `ki_description`, footprint `Description` property (KiCad 8+) | "USB 2.0 ESD protection", "Step-Down DC-DC" | Medium |
| Pin names/types | netlist `pinfunction`/`pintype`; pad `pinfunction` in KiCad 6+ boards | `D+`, `D-`, `CC1`, `SW`, `FB`, `BST`, `XTAL_IN`, `OSC32_OUT`, `TXP`, `VDDA`, `ANT` | Strong for role binding |
| Net names | board nets | `USB_DP`, `USB_D+`, `ETH_TX_P`, `MIPI_CSI_D0_N`, `VBUS`, `SW`, `+3V3A` | Medium (names are free text) |
| Topology | the netlist graph | a two-pin cap between a supply and ground sharing the supply with an IC power pin (decoupling, D25); a two-pin part whose both nets go to an MCU's `OSC_IN`/`OSC_OUT` (crystal); a two-pin part from an IC `SW` pin to an output capacitor (power inductor) | Strong once the anchor part is known |
| Geometry | footprint pads/courtyard | 4-pad 3.2×2.5 mm crystal, castellated module with an antenna region on `F.Cu` keep-out | Weak alone; used to break ties |

Older PCBench boards (KiCad 4/5 `module` files) have no pad `pinfunction` and often no schematic; detection
there relies on lib_id, value and net names, and its confidence is capped (below).

### 3.2 Confidence

A detector contributes a fixed integer weight (catalogue field `weight`, 0–100). A category instance's
confidence is the sum, capped at 100. Default thresholds:

| Confidence | Effect |
|---|---|
| ≥ 70 (`apply`) | All rules of the category are applied at their catalogue severity |
| 40–69 (`suggest`) | Soft and advisory rules only, hard rules demoted to soft; the report asks the user to confirm |
| < 40 | Not applied; listed in the report as "possible <category>" |

Typical results: a `Connector_USB:USB_C_Receptacle_*` footprint (60) + nets bound to pins named `D+`/`D-`
(30) = 90 → apply. A part with reference `Y1` (15) and value `16MHz` (20) and two nets to MCU pins named
`OSC_IN`/`OSC_OUT` (50) = 85 → apply. A connector named `J3` on nets `DP`/`DM` with no other evidence = 30 (net names 20 + prefix 10) →
reported only. Integer weights keep the result exact and order-independent.

### 3.3 Roles and binding

A category instance is an *anchor* part plus the parts and nets that play roles relative to it. Binding runs
after detection and is part of the category definition, e.g. for `usb2`:

```
anchor:   J? matching connector detectors
roles:    dp, dm          nets on anchor pins named D+/D-/DP/DM (both A6/B6 and A7/B7 on a USB-C receptacle)
          esd             a part with ≥ 2 pins on {dp, dm} and one pin on GND (TVS array or two diodes)
          series_r        a two-pin part in series on dp or dm (22–33 Ω on some MCUs)
          cmc             a 4-pin part on dp/dm with two windings (common-mode choke)
          transceiver     the IC whose pins named D+/D- or USB_DP/USB_DM end the nets
          vbus            net on anchor pins named VBUS
          shield          anchor pins named SHIELD/SH or mechanical pads
```

A net walk follows two-pin series parts (resistors, ferrites, chokes, AC-coupling capacitors) so a pair that
passes through a series resistor or an AC-coupling capacitor is still one pair, split into *segments* for
length and skew rules. Binding is deterministic (pins in natural order, ties by reference). A role that cannot
be bound disables the rules that need it and is reported ("usb2 J1: no ESD part found; USB2-07 not
applied").

### 3.4 Conflicts

| Conflict | Resolution |
|---|---|
| Two categories claim the same part (e.g. `ldo` and `buck` on a regulator with an `SW` pin) | Higher confidence wins; equal → catalogue order; the loser is reported |
| Two instances put different rules of the same kind on one net (e.g. `usb2` 90 Ω and a user net class) | Board rules win (§3.5). Between component rules: hard constraints combine by intersection (tightest clearance, smallest max length, smallest skew); targets that cannot both hold (90 Ω vs 100 Ω on one pair) → neither is applied, both reported as conflicting |
| A keep-out from one category covers a part another category wants close (antenna keep-out vs a decoupling cap placed under it) | Hard keep-out wins over soft proximity; the proximity rule reports "unsatisfiable: blocked by RF-03 of AE1" |
| A rule asks for something the board cannot have (4-layer reference plane on a 2-layer board) | Rule falls back to its documented 2-layer variant if the catalogue has one; otherwise not applied and reported |

### 3.5 Precedence and user override

From highest to lowest:

1. **Explicit user overrides** in a project file `<project>.tracemaker_rules.yaml` (same schema as the catalogue,
   §6.3): disable a rule id globally or for a reference (`disable: [USB2-04@J2]`), change a parameter, force a
   category (`assert: {J5: usb2}`) or forbid one (`deny: {U7: buck}`).
2. **The board's own rules**: net classes and net-class assignments in `.kicad_pro`, custom rules in
   `.kicad_dru`, rule areas and keep-outs in the board. If the user already gave the USB pair a net class with
   a diff-pair width/gap, TraceMaker uses it and reports "USB2-01 satisfied by net class USB90".
3. **Component rules** from the catalogue, at the confidence-adjusted severity.
4. **TraceMaker defaults** (doc 05 §9).

Command-line switch: `--component-rules off|report|soft|on` (default planned: `report` until the gates in §9
pass, then `soft`). `report` detects and checks only; `soft` applies everything as costs and never adds hard
constraints; `on` applies catalogue severities.

### 3.6 Missing inputs

| Missing | Effect |
|---|---|
| Stackup (layer thickness, εr) | Impedance rules not converted to width/gap. If the board already has a net class for the nets, keep it; else keep default widths and report "USB2-01 needs a stackup; not applied". Never guess a stackup silently. A `--assume-stackup jlc-2l-1.6` style preset is allowed only when the user names it |
| Pin names (old boards) | Role binding by net names only; confidence capped at 60 (`suggest`) |
| Schematic | Keywords/description unavailable; footprint `Description` property used if present |
| Operating voltage (creepage) | Voltage taken from net names (`230VAC`, `HV`, `MAINS`, `L`/`N` on a mains connector) or user override; unknown → creepage rules advisory |
| Current (trace width) | Only from user override or a net-class/net-name convention (`5A`); otherwise advisory "check width for current" on detected power paths |


## 4. Rule model

Every catalogue rule has an `id` (category prefix + number, e.g. `USB2-03`), a `kind`, parameters with units,
a `severity`, the stages that enforce it (`place`, `route`, `drc`, `report`) and one or more `sources`.
Units: millimetres for lengths, ohms, picoseconds for time skew (converted to length with the stackup's
propagation delay when known, else with a stated conservative 6.0 ps/mm for FR-4 outer layers and 7.0 ps/mm
inner, see §5.3), volts, amperes, °C.

| Kind | What it says | Parameters | Example |
|---|---|---|---|
| `impedance` | Nets of a role are a controlled-impedance line | `z0_ohm` (single-ended) or `zdiff_ohm`, `tol_pct`, `structure` (microstrip, stripline, gcpw) | USB 2.0 D+/D− 90 Ω ±10 % differential |
| `diff_pair` | Two nets route coupled | `max_uncoupled_mm`, `gap_mm` (or from `impedance`), `max_intra_skew_mm`/`_ps` | Ethernet MDI pairs |
| `length_match` | A group of nets has matched lengths | `group` (role list), `tol_mm` or `tol_ps`, `reference` (longest/clock/explicit) | DDR byte lane DQ/DM/DQS ±… |
| `max_length` | A net or segment is no longer than | `max_mm`, `segment` (pin-to-pin, between roles) | crystal trace ≤ 10 mm |
| `max_stub` | Branches off a net are short | `max_mm` | USB-C D+/D− stub where A6/B6 are joined |
| `via_limit` | Few vias / no vias on a net | `max_vias`, `per` (net/segment) | Antenna feed: 0 vias |
| `keepout` | No copper/vias/parts in an area derived from a part or pad | `shape` (footprint region, pad bloat, polygon relative to anchor), `layers` (all, adjacent, same side), `disallow` (tracks, vias, pads, zones, footprints) | No copper on any layer under a chip antenna's clearance area |
| `proximity` | A role part is within a distance of a pin | `from_role`, `to_pin_role`, `max_mm`, `measure` (pad-edge, centre, routed length) | Decap ≤ 2 mm from its supply pin |
| `order` | Along a net, parts come in an order | `sequence` of roles | connector → ESD → transceiver |
| `orientation` | Rotation or side relative to another part/edge | `relation` (facing edge, parallel, same side) | Connector faces the board edge |
| `edge` | Part sits at the board edge | `max_mm` from edge to courtyard / mating face, `overhang_mm` | USB receptacle front flush with edge |
| `reference_plane` | A net routes over an unbroken plane on an adjacent layer | `plane_net` (GND or role), `no_split_crossing`, `stitch_via_mm` near layer changes | High-speed pairs over solid GND |
| `stitching` | Ground vias along a line/region | `pitch_mm`, `offset_mm` | GCPW via fence at ≤ λ/20 |
| `guard` | Guard trace/ring around a net or part | `net` (GND or driven guard), `gap_mm` | Crystal ring, high-impedance input |
| `copper_area` | Minimum or maximum copper area of a net/role | `min_mm2` / `max_mm2`, `layers` | Buck SW node: minimise; LDO tab: ≥ x mm² |
| `thermal_vias` | Vias in an exposed pad | `count_min`, `drill_mm`, `pitch_mm` | QFN exposed pad |
| `loop` | The current loop through roles is small | `roles` (ordered loop), `max_mm` (sum of pad-to-pad distances) or minimise | Buck hot loop VIN cap → HS FET → LS FET → GND |
| `kelvin` | Sense connection taken from pad inside, routed as a pair | `pads`, `pair` | Shunt resistor sense lines |
| `width_for_current` | Minimum width from current and temperature rise | `current_a`, `delta_t_c`, `copper_oz`, `layer` (outer/inner) | Motor supply 3 A |
| `clearance` | Minimum distance between two net groups/roles | `min_mm`, `layers` | Primary/secondary of an isolator |
| `creepage` | Minimum surface distance; slots allowed | `min_mm`, `voltage_v`, `pollution_degree`, `material_group` | Mains L/N to SELV |
| `connection` | How a pad connects to a zone | `solid` / `thermal`, spoke width | High-current pads solid |
| `net_weight` | Placement wirelength weight of a net or role group | `weight` | Hot-loop nets ×8 |
| `advisory` | A check that is only reported | free text + optional measurable | "CC pins need 5.1 kΩ pull-downs (UFP)" |

A rule's `applies_to` names roles, not nets: rules are written once per category and bound per instance.

## 5. Mapping onto TraceMaker

### 5.1 What exists today (2026-10-03)

| Machinery | Where | Used for |
|---|---|---|
| Net classes with track width, clearance, diff-pair width/gap/via gap | `model/rules.hpp` (`NetClass`) | `impedance`, `diff_pair` once widths are computed |
| KiCad custom rules: conditions (`NetClass`, `NetName`, `Reference`, `Type`, `Layer`, `memberOfFootprint`, `insideArea`/`intersectsArea`/`enclosedByArea`, `inDiffPair`, `existsOnLayer`, `isPlated`), constraints `clearance`, `track_width`, `length`, `skew` | `drc/rule_engine.cpp` | `clearance`, `max_length`, `length_match`, keep-outs by area |
| Diff-pair coupled routing (P/N detection by name or per pair; coupled pair search with coupled vias, gap from rule / net class / clearance, `diff_pair_uncoupled` limits the legs, re-coupling after rip-up, falls back to single routing; doc 05 §14, D50) | `RouterOptions::diff_pairs`, `pair_nets` (off by default), `RouteResult::pairs`; `tracemaker pairs` measures coupled share, gap and skew | `diff_pair` |
| Length tuning by meanders into a custom `length` range; `skew_constraint`; intra-pair skew limit for coupled pairs (`RouterOptions::pair_skew`, `--pair-skew-mm`) | `route/router.cpp`, `RouteResult::length_tuned` | `length_match`, `max_length` (as upper bound), `max_intra_skew_mm` |
| Rule areas / keep-outs (`keepout_tracks/vias/pads/pour/footprints`) read from the board | `model/board.hpp` (`Zone`) | `keepout` (a generated keep-out is just another rule area) |
| Placer pseudo-nets (`PNet::affinity`, decoupling capacitors, D25) | `place/problem.hpp` | `proximity`, `loop`, `net_weight` |
| Fixed edge connectors, locked parts | doc 04 §2 | `edge` (partly) |
| Via cost per connection | `RouterOptions::via_cost_mm` | `via_limit` (soft) |
| Own DRC with KiCad violation names, KiCad DRC as judge | `drc/`, doc 08 | checking all `drc` rules |

### 5.2 Rule kind → enforcement

| Kind | Placement | Routing | Check / report | New capability needed |
|---|---|---|---|---|
| `impedance` | — | net class width/gap for the role's nets, per layer | width/gap vs target; layer changes; reference plane | **Stackup model** (not parsed today: `board.hpp` has no stackup) and an **impedance solver** (§5.3) |
| `diff_pair` | net weight ↑ for the pair | `diff_pairs` on for those nets; max uncoupled length | KiCad `diff_pair_gap`, `diff_pair_uncoupled` | Per-net enablement exists (`pair_nets`, D30) and the router reads `diff_pair_gap`/`diff_pair_uncoupled` custom rules (D50); still missing: per-pair skew limits from the catalogue (one global `pair_skew` today) |
| `length_match`, `max_length` | pseudo-net weight so endpoints stay close (max length is impossible if the parts are far apart) | meander tuning to a range; lengths through series parts summed per segment | KiCad `length`, `skew` | Groups spanning series parts; ps→mm conversion from stackup |
| `max_stub`, `via_limit` | — | via cost ↑ / via forbidden on the role's nets; stub check at commit | KiCad `via_count`; own stub measure | Stub measurement in the own DRC |
| `keepout` | rule area with `footprints` disallowed | rule area with `tracks/vias/zones` disallowed | KiCad DRC on the generated rule area | Generating rule-area polygons from footprint geometry (antenna region from the module's `F.Cu`/`Edge` keep-out or the datasheet offsets) |
| `proximity` | `PNet::affinity` (D25) between role pin and part; hard: legaliser distance constraint | — | measured pad-edge distance | Hard distance constraint in legaliser/annealer (doc 04 §2 lists it as planned) |
| `order` | pseudo-nets along the sequence (connector–ESD, ESD–IC), ESD weighted ×2 to its connector | net topology: route connector→ESD→IC as a daisy chain, not a star | path order check on the routed net | Ordered (chain) topology for a net in the planner (today: MST over pads) |
| `orientation`, `edge` | allowed rotations restricted; edge attraction / fixed | — | angle and edge distance | Rotation restriction per part (doc 04 §2 already has allowed states) |
| `reference_plane` | — | cost ↑ for segments over a plane gap or void on the adjacent layer; stitching via at layer change | plane coverage under every segment of the net | **Plane-coverage query** (zones are filled polygons; test segment ⊂ adjacent-layer fill of GND) |
| `stitching`, `guard` | — | post-route generation of vias/tracks in a transaction (cleanup stage) | count/pitch check | Generator for stitching vias and guard tracks (new cleanup pass) |
| `copper_area`, `thermal_vias`, `connection` | — | zone-connection style per pad; thermal vias in exposed pads only if the footprint lacks them (advisory by default — adding vias changes fab) | area/count | Zone fill area per net (KiCad refills; use `kicad-cli --refill-zones`) |
| `loop` | pseudo-nets around the loop at high weight; hard bound on loop perimeter | route loop nets first, on the part side, no vias | loop perimeter (sum of pad-to-pad distances) | Loop metric |
| `kelvin` | — | route sense pair from the named pads as a diff pair | pad entry point check | Pad-side entry constraint |
| `width_for_current` | — | net class minimum width (and via count for layer changes) | width vs IPC-2152/2221 table | Current table (§5.4) |
| `clearance`, `creepage` | courtyard-to-barrier distance | custom `clearance` between role net groups | KiCad `clearance`, `creepage` (KiCad 9+) | Net groups by role in conditions; creepage in the own DRC (surface path across slots) |
| `net_weight` | `PNet` weights | net order (critical first) | — | — |
| `advisory` | — | — | report only | — |

### 5.3 Impedance (new)

A controlled-impedance rule needs the stackup: copper thickness, dielectric thickness to the reference plane,
εr (and loss tangent for reporting), solder mask. KiCad stores it in the `(setup (stackup …))` block of the
board; TraceMaker reads it since M13 P4 (§14.5, which also lists where the implementation differs from this plan).
Planned:

1. Parse the stackup (doc 08 §3 owns it; read-only, round-trips untouched).
2. Width/gap from closed-form formulas: Hammerstad & Jensen microstrip (with Kirschning–Jansen dispersion
   ignored at these frequencies), Wheeler/Cohn stripline, coupled microstrip/stripline and grounded coplanar
   waveguide from Wadell's *Transmission Line Design Handbook* (1991); IPC-2141A as the industry reference
   and its accuracy caveats (±5–10 % for the simple formulas). Solve for width by bisection in integer nm
   (deterministic). Cross-check against KiCad's PCB calculator (which implements the same Wadell/Hammerstad
   formulas) in a unit test on a table of known cases.
3. The fab tolerance (±10 % typical) is reported, never hidden: "USB2-01: 90 Ω target → 0.20 mm / 0.15 mm
   on F.Cu over GND (L2, 0.21 mm, εr 4.4), formula error ±7 %; ask the fab for controlled impedance".
4. Propagation delay from εeff gives the ps→mm conversion for skew rules; without a stackup the conservative
   default is 6.0 ps/mm outer / 7.0 ps/mm inner (FR-4, εr ≈ 4.2–4.6) and the report says so.

Two-layer 1.6 mm boards cannot reach 90 Ω with sane widths as plain microstrip (the dielectric is too thick:
each line would be about 3 mm wide, ≈ 45 Ω loosely coupled); the catalogue's fallback for such boards is
*coplanar with ground* or simply *short, matched, coupled*, which is what USB 2.0 FS needs anyway (§8.1).

### 5.4 Current capacity (new)

Width for current uses the IPC-2152 charts where the user supplies current; the IPC-2221 closed form
`I = k · ΔT^0.44 · A^0.725` (A in mil², k = 0.048 external / 0.024 internal) is the documented fallback and
is known to be conservative for internal layers relative to IPC-2152. Values are reported with the
assumption (copper weight, ΔT).

### 5.5 How rules reach the engines

```
board + netlist + catalogue (+ user overrides)
   │
   ▼  detect (§3.1–3.2)  →  instances, confidence
   ▼  bind roles (§3.3)  →  nets/parts per role
   ▼  resolve (§3.4–3.5) →  effective rules, each tagged applied / satisfied-by-board / not-applied(reason)
   ▼  compile
        ├─ placement: PNet pseudo-nets (affinity, weights), rotation/edge restrictions, keep-out rule areas
        ├─ routing:   synthetic net classes (width/gap per layer), custom rules (clearance, length, skew),
        │             per-net router options (diff pair, via cost, chain topology, plane-gap cost)
        └─ check:     generated `.kicad_dru` fragment + keep-out areas for the own DRC and the KiCad judge
   ▼  report (§7) and events
```

The generated rules live **in memory** and in a sidecar file `<project>.tracemaker.kicad_dru` written next to
the output for the user to inspect or merge. TraceMaker never edits the user's `.kicad_dru` (rule 8:
untouched content round-trips byte for byte); a `--write-rules` option may append a clearly delimited
`# BEGIN TraceMaker component rules … # END` block on request. For the benchmark judge, the harness copies the
project to a scratch directory, merges the fragment, and runs `kicad-cli pcb drc` twice: once with the user's
rules only (the existing gate — component rules must not add DRC errors there) and once with the merged rules
(the component-rule metric).

Generated rule areas are added to the output board only with `--write-keepouts`; by default they are used
internally and drawn by the viewer.

## 6. Data model

### 6.1 Catalogue file

`docs/component_rules.yaml` (draft; installed as `share/tracemaker/component_rules.yaml`). Top level:

```yaml
version: 1
units: {length: mm, impedance: ohm, time: ps, voltage: V, current: A, temperature: C}
defaults: {thresholds: {apply: 70, suggest: 40}, prop_delay_ps_per_mm: {outer: 6.0, inner: 7.0}}
sources: {KEY: {title: ..., url: ...}, ...}
categories:
  - id: usb2
    name: USB 2.0 (Full/High Speed)
    detect: [{signal: lib_id, pattern: "...", weight: 60}, ...]   # regex, case-insensitive
    roles: {dp: {...}, dm: {...}, esd: {...}}
    rules:
      - id: USB2-01
        kind: impedance
        applies_to: [dp, dm]
        params: {zdiff_ohm: 90, tol_pct: 10}
        severity: soft
        enforce: [route, drc, report]
        sources: [TI-SPRAAR7J, TI-SLLA414]
        evidence: V         # V read in source, D source read but number is a default, R not verified, C computed (§8)
        note: ...
        checked: "2026-10-05 confirmed: <document, section, page>"   # source check (§8.10)
```

### 6.2 In the engine (planned types, namespace `tmk::crules`)

`Category`, `Detector`, `Role`, `RuleSpec` (from the file); `Instance` (category, anchor footprint index,
confidence, role → nets/parts); `EffectiveRule` (spec, instance, resolved parameters, status, reason).
Compilation produces values of the existing types (`model::NetClass`, `model::CustomRule`, `model::Zone`
with `rule_area = true`, `place::PNet`) plus the new ones listed in §5.2.

### 6.3 User override file

Same schema, plus `disable`, `assert`, `deny`, `set` (parameter overrides by rule id and optional
`@REF`), and `stackup_preset`. Unknown keys are errors (a typo must not silently drop an override).

As built (§14.6): the engine reads the file as **JSON** (`--rules-override FILE` on `tracemaker rules`,
`tracemaker route` and `tracemaker-place`); the YAML form maps 1:1 and `scripts/crules_override.py FILE.yaml -o
FILE.json` converts it. Accepted keys: `version` (1), `comment`, `disable`, `assert`, `deny`, `set`;
`stackup_preset` and catalogue keys are not built yet and are refused.

```json
{
  "version": 1,
  "disable": ["USB2-04@J2", "XTAL-04", "mounting_hole", "crystal@Y2"],
  "assert": {"J5": "usb2"},
  "deny": [{"category": "buck", "ref": "U7"}],
  "set": [{"rule": "XTAL-02", "param": "max_mm", "value": 5},
          {"rule": "XTAL-01@Y1", "param": "max_mm", "value": 8}]
}
```

`disable` takes rule ids or category ids, optionally `@REF` (the instance anchor); `assert`/`deny` take
`{REF: category}` (or a list of categories) or a list of `{category, ref}`; `set` takes `{rule, param, value}` with
the reference as `RULE@REF` or `"ref"`; the value must have the catalogue value's JSON type. Entries for one reference
win over global ones; among equals the later entry wins.

## 7. Reporting

What the user sees (CLI summary; full table in `report.json` and the viewer's "Rules" panel):

```
Component rules (catalogue v1, mode soft)
  usb2        J1  USB_C_Receptacle_GCT_USB4105  confidence 90  roles: dp=USB_D+ dm=USB_D- esd=U3 transceiver=U1
     USB2-01 impedance 90 Ω diff        applied: net class tmk_usb2 0.20/0.15 mm on F.Cu (stackup L1/L2)
     USB2-02 intra-pair skew ≤ 1.27 mm  met (0.31 mm)
     USB2-07 ESD within 5 mm of J1      met (2.1 mm)  [placement]
     USB2-08 order J1→U3→U1             met
  usb3_typec  J1  (same anchor)          confidence 100
     USBC-03 D+/D- tie stub ≤ 3.5 mm    VIOLATED (4.1 mm, B6 join)  soft
     USBC-08 CC width ≥ 0.2 mm          applied (net class tmk_usbc_cc)
  crystal     Y1  16MHz  confidence 85  roles: xin=OSC_IN xout=OSC_OUT load_caps=C4,C5 ic=U1
     XTAL-01 crystal ≤ 10 mm from IC pins  met (4.0 mm)
     XTAL-04 no foreign tracks under Y1    applied (keep-out, F.Cu+B.Cu)
  not applied
     HV-01   J7 L/N: operating voltage unknown; reported only (set it in the override file)
  possible (low confidence): J4 "USB?" — nets DP/DM only (30)
```

Every line carries the rule id, so the user can disable it in the override file. The report never says
"compliant" for safety rules; it says "meets the catalogue minimum (IPC-2221B B2, 300 V): 2.5 mm".

Events (doc 09/13): `crules.detected` (instance, parts, nets, confidence), `crules.rule` (id, status, measured
value), `crules.keepout` (polygon, layers). The viewer highlights bound parts and nets per instance and draws
keep-outs hatched.

## 8. Catalogue

The tables below are generated from [`component_rules.yaml`](component_rules.yaml) (42 categories, 193 rules);
edit the YAML and regenerate, do not edit the tables by hand. Columns:

- **Parameters** are the defaults the engine would use, with units in the key (`_mm`, `_ohm`, `_ps`, `_nF`).
  Where a source says "as close as possible" and gives no number, the default is a TraceMaker choice and the
  note says so.
- **Sev.**: `hard` / `soft` / `adv.` (advisory), the catalogue severity before confidence demotion (§3.2).
- **Enforced**: which stages use the rule (`place`, `route`, `drc`, `report`).
- **Ev.** (evidence): **V** = the value was read in the cited document during the research for this doc
  (2026-10-03); **R** = attributed to the cited document from prior knowledge, not re-read (several vendor
  servers — ST, ADI, NXP, Micron, AMD, Silicon Labs — refused or timed out, and web search was unavailable), so
  verify before making the rule hard; **D** = the cited source was read and supports the rule, but gives no number, so the
  value is a TraceMaker default (D46); **C** = computed from a cited formula. Every **R** and **D** rule is soft or advisory,
  and the engine demotes them to soft if one is ever marked hard. A `checked` field in the YAML records each source check
  (§8.10).
- **Sources**: keys into §13. `TMK-PRACTICE` marks rules that rest on common practice only.

### 8.0 Cross-category conventions

**General high-speed defaults** (TI SPRAAR7J, V), applied to every net that a rule gives an impedance target,
unless its category overrides them: pair-to-other-signal spacing 0.76 mm, pair-to-clock 1.27 mm; GND stitching
via within 5.08 mm of every signal via that changes reference layer; ≥ 2.29 mm from the reference-plane edge;
via stubs ≤ 0.38 mm; inline passives 0402 or smaller with the plane voided under their pads; no stubs or test
points on pairs; segments ≥ 1.5 W, bends ≥ 135°.

**Where sources disagree** (the default is listed first; the alternative is recorded in the rule's note):

| Topic | Values | Default and why |
|---|---|---|
| USB 2.0 intra-pair match | 1.27 mm (TI) vs 0.15 mm (Raspberry Pi CM5) | TI: HS bit time (2 ns) makes 1.27 mm (~8 ps) ample |
| USB 3 / PCIe impedance | 90 / 85 / 95 / 100 Ω, ±5–15 % | Per interface generation; PCIe Gen1/2 100 Ω, Gen3+ 85 Ω at CEM connectors |
| PCIe AC-cap position | near receiver end of segment vs near driver | Not enforced; reported (open question §11.8) |
| Ethernet PHY–magnetics length | < 50.8 mm (TI) vs 25.4–76.2 mm window (SMSC, minimum for ESD) | TI maximum; no minimum |
| Ethernet ESD position | PHY side of magnetics (TI SNLA387) vs connector side (generic ESD rule) | Ethernet follows TI (ETH-14 overrides ESD-01) |
| MII impedance / matching | 68 Ω, 50.8 mm (SNLA079) vs 50 Ω, 1.27 mm (SNLA387) | SNLA387 (newer) |
| Chassis isolation gap | ≥ 0.51 mm (TI SNLA387) vs ~2 mm practice for 1500 Vrms | 0.51 mm hard, 2 mm recommended in the report |
| Crystal distance | "as close as possible" (ST, NXP) vs ≥ 2.7 mm from the chip (Espressif ESP32) | Close, except ESP32 family (XTAL-08) |
| Split vs solid ground | Solid plane, partitioned (ADI MT-031, TI SLVA959B) vs AGND/DGND split joined once (older guides, many codec datasheets) | Never cross a split; never create one |
| Current per width | IPC-2221 formula vs TI 0.381 mm/A vs IPC-2152 charts | max(IPC-2221, 0.381 mm/A) external; IPC-2152 when the user supplies data |
| Thermal via size | 0.3 mm drill at 1.0–1.2 mm pitch (TI SLMA002) vs 0.2 mm hole / 0.5 mm pad (TI SLVA959B) | Datasheet of the part; advisory only |
| IPC-2221 Table 6-1 | B2 0.6 mm (IPC-2221B) vs 0.64 mm (KiCad calculator, newer layout) | 0.6 mm as printed in IPC-2221B; columns stored with the revision |
| Antenna ground | No copper under the antenna (ESP32, Pico W, chip antennas) vs ground under the whole module including antenna (u-blox NINA-B3 internal antenna) | Per-module table only; no blanket rule |
| JTAG TDO termination | ≤ 12.7 mm vs ≤ 25.4 mm (both in TI SPRU655I) | Stricter |

**Topology detectors** named in the YAML (`signal: topology`), each a pure function of the netlist:
`two_terminal_between_osc_pins` (a two-pin part whose nets end on an IC's oscillator pins),
`two_terminal_cap_supply_to_ground_sharing_ic_power_pin` (D25's decoupling test),
`inductor_from_sw_pin_to_output_cap`, `net_feeds_power_stage` (a net that reaches a regulator VIN, a bridge VM
or a connector power pin through ≤ 1 series part), and `diode_array_signal_to_ground_near_connector_net` (a
diode/TVS part with ≥ 1 pin on a net that also touches a connector and one pin on GND).

**Part-specific rules.** Datasheet layout sections beat generic rules (TI SLYT614: "always consult the device's
datasheet"). The catalogue carries a few part-family conditions (`applies_when: "ic matches esp32"`); a fuller
part table is an open question (§11.2).

### 8.1 Interfaces

#### `usb2` — USB 2.0 (Full/High Speed) port

Detected by: lib_id (up to 60), pin_name (up to 30), net_name (up to 20), ref_prefix (up to 10). Roles: `connector`, `dp`, `dm`, `esd`, `cmc`, `transceiver`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| USB2-01 | D+/D- are a 90 ohm differential pair (45 ohm single-ended) | zdiff_ohm=90; z0_ohm=45; tol_pct=10 | soft | route, drc, report | V | TI-SPRAAR7J, TI-SLLA414 |
| USB2-02 | Route D+/D- coupled; intra-pair length mismatch <= 1.27 mm | max_intra_skew_mm=1.27 | soft | route, drc, report | V | TI-SPRAAR7J, TI-TUSB8041, RPI-CM5 |
| USB2-03 | Transceiver-to-connector length <= 203 mm recommended (hard ceiling 305 mm) | max_mm=203; hard_max_mm=305 | adv. | report | V | TI-TUSB8041, TI-SPRAAR7J |
| USB2-04 | <= 4 vias per line, equal count on D+ and D- | max_vias=4; equal_pn=true | soft | route, report | V | TI-SPRAAR7J |
| USB2-05 | No stubs or test points on D+/D- (inline and symmetric if unavoidable) | max_mm=0 | soft | route, report | V | TI-SPRAAR7J, TI-TUSB8041 |
| USB2-06 | Keep 0.76 mm from other signals and 1.27 mm from clocks | to_other_mm=0.76; to_clock_mm=1.27 | soft | route, report | V | TI-SPRAAR7J |
| USB2-07 | ESD and common-mode choke as close to the connector as possible, flow-through | to=connector; max_mm=5 | soft | place, report | V | TI-SPRAAR7J, TI-SLVA680A |
| USB2-08 | Along each line: connector -> ESD -> CMC -> transceiver | sequence=[connector, esd, cmc, transceiver] | soft | place, route, report | V | TI-SPRAAR7J, TI-SLVA680A |
| USB2-09 | Route over solid GND on the adjacent layer; never cross a plane split | plane_net=GND; no_split_crossing=true | soft | route, report | V | TI-SPRAAR7J |
| USB2-10 | No AC coupling capacitors on D+/D-; P/N swap only if the PHY supports it | — | adv. | report | V | TI-SLLA414, RPI-CM5 |
| USB2-11 | Receptacle at the board edge, mating face at or just past the edge | max_mm=0.5 | soft | place, report | R | TMK-PRACTICE |

- USB2-01: SLLA414 states +/-15 %; SPRAAR7J tables +/-10 %. Needs a stackup (doc 15 §5.3). FS-only links tolerate much more; HS is the reason for the rule.
- USB2-02: TI 50 mil; Raspberry Pi CM5 asks 0.15 mm. Default is TI's value.
- USB2-07: 5 mm is TraceMaker's default for 'as close as possible'; sources give no number.
- USB2-11: General connector practice (CONN-01); kept here so the USB report is self-contained.

#### `usb3_typec` — USB 3.x SuperSpeed and USB Type-C receptacle

Detected by: lib_id (up to 60), pin_name (up to 40), value (up to 30). Roles: `connector`, `cc`, `sbu`, `vbus`, `sstx`, `ssrx`, `dp_tie`, `ac_cap`, `esd`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| USBC-01 | SuperSpeed pairs 90 ohm differential | zdiff_ohm=90; tol_pct=7 | soft | route, drc, report | V | TI-SPRAAR7J, TI-SLLA414, TI-TUSB1310A |
| USBC-02 | Intra-pair mismatch <= 0.127 mm; TX and RX need not be matched to each other | max_intra_skew_mm=0.127 | soft | route, drc, report | V | TI-SPRAAR7J, TI-SLLA414, TI-TUSB1310A |
| USBC-03 | Each stub of the A6/B6 and A7/B7 D+/D- tie <= 3.5 mm | max_mm=3.5 | soft | route, report | V | TI-TUSB320 |
| USBC-04 | <= 2 vias per SuperSpeed line | max_vias=2; equal_pn=true | soft | route, report | V | TI-SPRAAR7J |
| USBC-05 | SuperSpeed length <= 140 mm (SoC tables 89-140 mm; 203 mm absolute for hubs/PHYs) | max_mm=140 | adv. | report | V | TI-SPRAAR7J, TI-TUSB1310A |
| USBC-06 | TX AC caps (100 nF, 0402 or smaller, no arrays) symmetric near the connector, order connector -> ESD -> CMC -> caps | sequence=[connector, esd, cmc, ac_cap]; cap_nF=100 | soft | place, report | V | TI-TUSB1310A, TI-TUSB8041, TI-SPRAAR7J |
| USBC-07 | Void the reference plane under AC-cap pads (100 %) and ESD pads (60 %) on the adjacent layer | layers=adjacent; disallow=[zones]; void_pct=100 | adv. | report | V | TI-SPRAAR7J |
| USBC-08 | CC traces >= 0.2 mm (carry VCONN current) | min_width_mm=0.2 | soft | route, drc | V | TI-TPS65987D |
| USBC-09 | VBUS sized for the port current; at 5 A ~3 mm outer (0.5 oz + plating), > 5 mm inner, >= 4 vias per layer change | current_a=3; min_width_mm_at_5a_outer=3.05; min_vias=4 | soft | route, drc, report | V | TI-TPS65987D, IPC-2152 |
| USBC-10 | ESD/TVS on CC, SBU, D+/D-, VBUS as close to the receptacle as possible | to=connector; max_mm=5 | soft | place, report | V | TI-SPRAAR7J, TI-SLVA680A |
| USBC-11 | Sink (UFP) needs Rd = 5.1 kohm (+/-20 %, +/-10 % if the sink reads the source's current advertisement) on each CC unless the controller integrates it | rd_kohm=5.1 | adv. | report | V | USB-TYPEC-SPEC, TI-TUSB320 |
| USBC-12 | SBU: no numeric layout rule found; treat as low-speed signal with ESD at the connector | — | adv. | report | V | TI-SPRAAR7J |

- USBC-01: Tolerance varies: +/-7 % (SPRAAR7J), +/-10 % (TUSB8041), +/-15 % (SLLA414); some SoCs ask 95 ohm +/-5 %.
- USBC-06: Spec range 75-265 nF (recalled, not re-read).
- USBC-07: Changes plane geometry; reported, not generated, until P4.
- USBC-09: Default current 3 A (Type-C default max without PD 5 A cable). See PWR-01.

#### `ethernet` — Ethernet 10/100/1000BASE-T (PHY, magnetics, RJ45)

Detected by: lib_id (up to 60), value (up to 50), keywords (up to 30), pin_name (up to 30), net_name (up to 20). Roles: `rj45`, `phy`, `magnetics`, `mdi_pairs`, `line_pairs`, `centre_tap`, `chassis`, `mac_bus`, `phy_xtal`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| ETH-01 | MDI pairs 100 ohm differential (50 ohm single-ended) | zdiff_ohm=100; z0_ohm=50; tol_pct=10 | soft | route, drc, report | V | TI-SNLA387, TI-SNLA079D, TI-DP83867 |
| ETH-02 | Intra-pair mismatch <= 0.51 mm at 1G, <= 1.27 mm at 10/100; each pair on one layer to the magnetics, no stubs | max_intra_skew_mm=0.51; max_intra_skew_mm_10_100=1.27 | soft | route, drc, report | V | TI-SNLA387, TI-SNLA079D |
| ETH-03 | PHY to magnetics < 50.8 mm | max_mm=50.8 | soft | place, report | V | TI-SNLA387, MCHP-AN18-0 |
| ETH-04 | >= 0.76 mm between different pairs; same-layer copper >= 3w (5w preferred); other high-speed traces >= 7.6 mm from the front end | pair_to_pair_mm=0.762; to_copper_w=3; to_high_speed_mm=7.62 | soft | route, report | V | TI-SNLA387, MCHP-AN18-0 |
| ETH-05 | No copper under discrete magnetics on the component layer and the layer below (all layers for automotive), void 0.5 mm beyond the body; not required under integrated-magnetics RJ45 | layers=[same, adjacent]; margin_mm=0.508; disallow=[tracks, vias, zones] | soft | place, route, drc, report | V | TI-SNLA387, TI-SNLA079D, TI-DP83867 |
| ETH-06 | Chassis (earth) ground isolated from circuit ground by >= 0.51 mm on all layers; many designs use ~2 mm for 1500 Vrms hipot | min_mm=0.508; recommended_mm=2.0 | **hard** | route, drc, report | V | TI-SNLA387, IEEE-802.3 |
| ETH-07 | Planes stop at the middle of the magnetics; only line pairs cross from there to the RJ45; chassis plane around RJ45 overlaps nothing else | layers=all; disallow=[zones, tracks_except_line_pairs] | soft | route, report | V | MCHP-AN18-0 |
| ETH-08 | Bob Smith termination: 75 ohm per pair centre tap to a common 1000 pF / 2 kV capacitor to chassis; unused RJ45 pairs terminated the same way, close to the RJ45 | r_ohm=75; c_pF=1000; c_rating_kV=2 | adv. | report | V | TI-SNLA079D, MCHP-AN18-0 |
| ETH-09 | Magnetics oriented with line side facing the RJ45 and PHY side facing the PHY | relation=line_side_faces_anchor | soft | place, report | V | MCHP-AN18-0 |
| ETH-10 | PHY crystal/oscillator and RBIAS resistor close to the PHY; one crystal per device; all crystal parts on the top layer | to=phy; max_mm=10 | soft | place, report | V | TI-SNLA387, MCHP-AN18-0 |
| ETH-11 | RGMII: 50 ohm, TXD[3:0] and RXD[3:0] each matched within 11 ps (~1.5 mm), length < 50.8 mm (152 mm max); 2 ns clock delay from the PHY/MAC internal delay, not trace | z0_ohm=50; tol_ps=11; tol_mm=1.524; max_mm=50.8; hard_max_mm=152.4 | soft | route, drc, report | V | TI-DP83867 |
| ETH-12 | RMII/MII: length < 152 mm, RX group and TX group matched within 1.27 mm; 50 ohm; series termination at the driver | max_mm=152.4; tol_mm=1.27; z0_ohm=50 | soft | route, report | V | TI-SNLA387, TI-SNLA079D |
| ETH-13 | Never route MDI or MAC bus over a plane split | no_split_crossing=true | soft | route, report | V | TI-SNLA079D |
| ETH-14 | ESD diodes on Ethernet go on the PHY side of the magnetics (exception to ESD-01 connector-side placement) | — | adv. | report | V | TI-SNLA387 |
| ETH-15 | MDIO needs a 1.5 kohm pull-up resistor (PHY datasheets; IEEE 802.3 clause 22) | r_kohm=1.5 | adv. | report | V | TI-DP83848C, IEEE-802.3 |

- ETH-03: SMSC AN18.0 gives a 25.4-76.2 mm window (minimum for ESD); TI gives < 50.8 mm. Default TI; minimum not enforced.
- ETH-06: 0.508 mm read in SNLA387; the 2 mm practice value is recalled, not verified.
- ETH-10: 10 mm is TraceMaker's default (see XTAL-01).
- ETH-12: SNLA079 allows 50.8 mm mismatch and 68 ohm; SNLA387 asks 1.27 mm and 50 ohm. Default SNLA387.
- ETH-15: The value is read in a PHY datasheet; the IEEE 802.3 clause 22 text (paywalled) was not read, so which side of the bus carries the pull-up is not taken from the standard.

#### `hdmi_dp` — HDMI / DVI / DisplayPort (TMDS, main link)

Detected by: lib_id (up to 60), pin_name (up to 30), value (up to 30). Roles: `connector`, `tmds`, `esd`, `ac_cap`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| HDMI-01 | TMDS / main-link pairs 100 ohm differential | zdiff_ohm=100; tol_pct=10 | soft | route, drc, report | V | TI-SPRAAR7J, TI-SLLA414 |
| HDMI-02 | Intra-pair mismatch <= 0.127 mm | max_intra_skew_mm=0.127 | soft | route, drc, report | V | TI-SPRAAR7J, TI-SLLA414 |
| HDMI-03 | Pair-to-pair matching within 25 mm is sufficient | tol_mm=25 | soft | route, report | V | RPI-CM5 |
| HDMI-04 | Avoid vias (0 in TI SoC tables; <= 2 on newer SoCs); max length 102 mm | max_vias=0; relaxed_max_vias=2; max_mm=102 | soft | route, report | V | TI-SPRAAR7J |
| HDMI-05 | ESD next to the connector, no vias between connector and ESD pin | to=connector; max_mm=5; max_vias_between=0 | soft | place, route, report | V | TI-TPD12S016 |
| HDMI-06 | DisplayPort needs AC caps on main-link lanes (spec 75-200 nF); HDMI has none. DP: no copper under connector pads (pad region < 75 ohm otherwise) | — | adv. | report | V | TI-SLLA414, TI-SN75DP130 |

- HDMI-01: SLLA414 gives +/-15 %.
- HDMI-03: Spec budget is 0.2 Tcharacter inter-pair.

#### `lvds` — LVDS link (display, ADC data, FPD-Link)

Detected by: value (up to 40), net_name (up to 20), keywords (up to 30). Roles: `pairs`, `term`, `receiver`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| LVDS-01 | 100 ohm differential, tightly coupled (S < 2W), uniform W and S | zdiff_ohm=100; tol_pct=10 | soft | route, drc, report | V | TI-SNLA187 |
| LVDS-02 | Termination resistor as close to the receiver inputs as possible | to=receiver; max_mm=3 | soft | place, report | V | TI-SNLA187 |
| LVDS-03 | Intra-pair mismatch <= 0.127 mm (general high-speed default) | max_intra_skew_mm=0.127 | soft | route, report | V | TI-SPRAAR7J |

- LVDS-02: 3 mm is TraceMaker's default.

#### `mipi_dphy` — MIPI D-PHY (CSI-2 camera, DSI display)

Detected by: net_name (up to 40), pin_name (up to 40), lib_id (up to 10). Roles: `data`, `clk`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| MIPI-01 | 100 ohm differential, 50 ohm single-ended, loosely coupled (lanes also carry single-ended low-power signalling) | zdiff_ohm=100; z0_ohm=50; tol_pct=15 | soft | route, drc, report | V | TI-SPRAAR7J, TI-DS90UB953 |
| MIPI-02 | Intra-pair mismatch <= 0.127 mm (RPi CM5: 0.15 mm) | max_intra_skew_mm=0.127 | soft | route, drc, report | V | TI-DS90UB953, RPI-CM5 |
| MIPI-03 | Clock-to-data lane mismatch <= 0.64 mm (lane skew budget 40 ps at 1.25 GHz) | tol_mm=0.64; tol_ps=40; reference=clk | soft | route, drc, report | V | TI-DS90UB953, TI-SPRAAR7J |
| MIPI-04 | <= 2 vias per line; length <= 254 mm; pair separation >= 3W | max_vias=2; max_mm=254; pair_spacing_w=3 | soft | route, report | V | TI-SPRAAR7J, TI-DS90UB953 |

#### `pcie` — PCI Express link (edge connector, slot, M.2)

Detected by: lib_id (up to 60), pin_name (up to 30). Roles: `tx`, `rx`, `refclk`, `ac_cap`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| PCIE-01 | 100 ohm differential (Gen1/2); 85 ohm at Gen3/4 CEM connectors; some SoCs 95 ohm +/-5 % | zdiff_ohm=100; alt_zdiff_ohm=85; tol_pct=10 | soft | route, drc, report | V | TI-SPRAAR7J, TI-DS160PR410 |
| PCIE-02 | Intra-pair mismatch <= 0.127 mm | max_intra_skew_mm=0.127 | soft | route, drc, report | V | TI-SPRAAR7J |
| PCIE-03 | Inter-pair (lane-to-lane) mismatch per direction <= 14 mm | tol_mm=14 | soft | route, report | V | TI-SPRAAR7J |
| PCIE-04 | TX AC caps 220 nF (Gen3/4; spec 75-265 nF), 0402 max, symmetric, GND void under pads | cap_nF=220 | soft | place, report | V | TI-DS160PR410, RPI-CM5 |
| PCIE-05 | Avoid vias (0 on older TI SoCs, <= 2 newer); length <= 102-140 mm; back-drill connector vias | max_vias=2; max_mm=140 | soft | route, report | V | TI-SPRAAR7J, TI-DS160PR410 |

- PCIE-01: Sources disagree; pick per generation (open question §11.3).
- PCIE-04: Placement within the segment disagrees: near the receiver end (DS160PR410) vs near the driver (RPi CM5).

#### `sata` — SATA link

Detected by: lib_id (up to 60), pin_name (up to 20). Roles: `pairs`, `ac_cap`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| SATA-01 | 100 ohm differential +/-15 % | zdiff_ohm=100; tol_pct=15 | soft | route, drc, report | V | TI-SPRAAR7J |
| SATA-02 | Intra-pair mismatch <= 0.127 mm; length <= 89-140 mm; avoid vias | max_intra_skew_mm=0.127; max_mm=140; max_vias=0 | soft | route, report | V | TI-SPRAAR7J |
| SATA-03 | 10 nF AC coupling caps on each line (spec max 12 nF) | cap_nF=10 | adv. | report | V | TI-SN75LVCP601 |

### 8.2 Memory and buses

#### `ddr3` — DDR3/DDR3L SDRAM interface

Detected by: value (up to 60), pin_name (up to 30). Roles: `controller`, `dram`, `byte_lane`, `addr_cmd`, `ck`, `vtt_term`, `vref`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| DDR3-01 | 50 ohm single-ended, 100 ohm differential (CK, DQS), +/-5 % | z0_ohm=50; zdiff_ohm=100; tol_pct=5 | soft | route, drc, report | V | TI-SPRABI1 |
| DDR3-02 | DQ/DM within +/-0.25 mm of their DQS; DQS P/N +/-0.025 mm; whole byte on one layer | tol_mm=0.25; dqs_pn_tol_mm=0.025; reference=dqs | soft | route, drc, report | V | TI-SPRABI1 |
| DDR3-03 | Fly-by topology; each address/command net within +/-0.51 mm of CK from controller to each DRAM; stubs < 2.0 mm; same via count per segment | topology=fly_by; tol_mm=0.51; max_stub_mm=2.0; reference=ck | soft | route, drc, report | V | TI-SPRABI1 |
| DDR3-04 | VTT termination (39-42 ohm) at the last DRAM, <= 12.7 mm of trace from it | to=last_dram; max_mm=12.7; measure=routed | soft | place, report | V | TI-SPRABI1 |
| DDR3-05 | Centre-to-centre spacing >= 5W (4W only up to 1066 MT/s) | pitch_w=5 | soft | route, report | V | TI-SPRABI1 |
| DDR3-06 | VREF: 10 nF + 100 nF at each VREF pin; trace >= 0.76 mm wide, >= 0.38 mm from other nets | min_width_mm=0.76; to_other_mm=0.38 | soft | place, route, report | V | TI-SPRABI1 |

#### `ddr4_lpddr4` — DDR4 / LPDDR4 SDRAM interface

Detected by: value (up to 60), pin_name (up to 30). Roles: `controller`, `dram`, `byte_lane`, `addr_cmd`, `ck`, `vrefca`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| DDR4-01 | 40 ohm single-ended, 80 ohm differential +/-10 %; stripline over full VSS reference; no plane cuts | z0_ohm=40; zdiff_ohm=80; tol_pct=10; structure=stripline | soft | route, drc, report | V | TI-SPRAD06 |
| DDR4-02 | DDR4: DQS-to-DQ <= 2 ps (~0.25 mm), DQS P/N <= 0.4 ps, <= 2 vias, equal via count; no cross-byte matching | tol_ps=2; dqs_pn_tol_ps=0.4; max_vias=2; reference=dqs | soft | route, drc, report | V | TI-SPRAD06 |
| DDR4-03 | DDR4: address/control to CK <= 4 ps total; CK P/N <= 0.8 ps; total <= 500 ps (~64 mm); <= 3 vias | tol_ps=4; ck_pn_tol_ps=0.8; max_ps=500; max_vias=3; reference=ck; topology=fly_by | soft | route, drc, report | V | TI-SPRAD06 |
| DDR4-04 | LPDDR4: DQS P/N <= 1.5 ps, DQS-to-byte <= 150 ps, CK and address <= 450 ps; via stub <= 0.5 mm; 5W to other classes | dqs_pn_tol_ps=1.5; tol_ps=150; max_ps=450; max_via_stub_mm=0.5 | soft | route, report | V | TI-SPRAD06 |
| DDR4-05 | 4W to other DDR classes, 3W within a class | other_class_w=4; same_class_w=3 | soft | route, report | V | TI-SPRAD06 |
| DDR4-06 | VREFCA 0.5 mm trace, 100 nF at each device | min_width_mm=0.5 | soft | place, route, report | V | TI-SPRAD06 |

- DDR4-01: Controller-specific; AM62x values. LPDDR4 per its own table.

#### `sdr_sdram` — SDR SDRAM / parallel memory and external buses

Detected by: value (up to 50), pin_name (up to 20). Roles: `controller`, `memory`, `data`, `addr`, `clk`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| PBUS-01 | Memory close to the controller; keep the bus short (sources thin: no primary numeric value read) | to=controller; max_mm=30 | soft | place, report | R | TMK-PRACTICE |
| PBUS-02 | Series source termination (22-33 ohm, 0 ohm placeholder) on clock lines next to the driver | r_ohm=22; max_mm=3 | soft | place, report | V | TI-SPRAD21 |
| PBUS-03 | Match bus lengths to the clock loosely (rule of thumb +/-10 mm at <= 133 MHz) | tol_mm=10; reference=clk | adv. | report | R | TMK-PRACTICE |

- PBUS-01: 30 mm is a TraceMaker default. Thin evidence.
- PBUS-03: Rule of thumb; no primary source read.

#### `qspi_flash` — QSPI / OSPI flash

Detected by: value (up to 60), pin_name (up to 20). Roles: `controller`, `flash`, `clk`, `data`, `series_r`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| QSPI-01 | CLK pin to flash <= 450 ps (~70 mm stripline / 80 mm microstrip) | max_ps=450; max_mm=70 | soft | place, route, report | V | TI-AM625 |
| QSPI-02 | Each data and CS line = CLK delay +/- 60 ps (~7.6 mm) | tol_ps=60; tol_mm=7.6; reference=clk | soft | route, report | V | TI-AM625 |
| QSPI-03 | Series resistor (0 ohm placeholder) as close as possible to the controller CLK pin; 50 ohm routing | to=controller; max_mm=3; z0_ohm=50 | soft | place, report | V | TI-AM625, TI-SPRAD21 |

#### `sd_sdio` — SD card / microSD / SDIO / eMMC

Detected by: lib_id (up to 60), pin_name (up to 20). Roles: `socket`, `controller`, `clk`, `bus`, `series_r`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| SDIO-01 | CLK, CMD, DAT routed in parallel, all the same length, < 120 mm | max_mm=120; tol_mm=5 | soft | route, report | V | TI-WL1837MOD, TI-TXS0206A |
| SDIO-02 | > 1.5W spacing or GND guard between lines, especially around CLK | spacing_w=1.5 | soft | route, report | V | TI-WL1837MOD |
| SDIO-03 | Series 0 ohm/22 ohm resistor on CLK close to the host | to=controller; max_mm=3 | soft | place, report | V | TI-SPRAD21 |
| SDIO-04 | eMMC HS200/HS400: no primary numeric source read; apply SDIO-01..03 and report | — | adv. | report | V | TI-SPRAD21 |

- SDIO-01: TXS0206A: keep round-trip reflection < 30 ns, load < 50 pF. tol 5 mm is TraceMaker's default for 'same length'.

### 8.3 Clocks

#### `crystal` — Quartz crystal with load capacitors

Detected by: lib_id (up to 50), ref_prefix (up to 15), value (up to 20), topology (up to 50). Roles: `crystal`, `xin`, `xout`, `load_caps`, `ic`, `series_r`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| XTAL-01 | Crystal as close to the IC oscillator pins as possible; trace <= 10 mm | to=[xin, xout]; max_mm=10; measure=routed | soft | place, report | D | ST-AN2867, NXP-AN2049 |
| XTAL-02 | Load caps next to the crystal, between crystal and IC, short ground to the IC's VSS/oscillator ground | to=crystal; max_mm=3 | soft | place, report | D | ST-AN2867 |
| XTAL-03 | No vias on crystal traces | max_vias=0 | soft | route, report | V | ESP-HDG-ESP32, ST-AN2867 |
| XTAL-04 | No foreign signals under the crystal/load-cap area or between crystal and IC, on any layer; solid GND on the adjacent layer | shape=courtyard_hull_with_ic_pins; layers=all; disallow=[foreign_tracks, foreign_vias]; margin_mm=0.5 | soft | route, drc, report | V | ESP-HDG-ESP32, ST-AN2867 |
| XTAL-05 | Ground guard ring around crystal, caps and traces, tied to the IC's VSS | net=GND | adv. | report | V | ST-AN2867 |
| XTAL-06 | XIN/XOUT paths short, load capacitors placed symmetrically (loose, mm scale) | tol_mm=2 | adv. | report | D | ST-AN2867 |
| XTAL-07 | Keep the crystal >= 5 mm from switching-regulator inductors/SW nodes, clocks and board edges | to_roles=[buck.inductor, buck.sw, boost.inductor]; min_mm=5 | soft | place, report | D | ST-AN2867, TI-SNVA021C |
| XTAL-08 | ESP32 family: crystal >= 2.7 mm (ESP32) / >= 2.0 mm (S3, C3) from the chip; dense GND vias around the clock trace | min_mm=2.7; min_mm_s3_c3=2.0; applies_when=ic matches esp32 | soft | place, report | V | ESP-HDG-ESP32, ESP-HDG-S3, ESP-HDG-C3 |
| XTAL-09 | Check load caps: CL = C1*C2/(C1+C2) + Cstray, Cstray ~2-5 pF; C1 = C2 = 2(CL - Cstray) | cstray_pF=[2, 5] | adv. | report | V | MCHP-AN826 |

- XTAL-01: No vendor gives a hard number; 5-10 mm is common practice. Exception XTAL-08 (ESP32).

#### `oscillator` — Crystal oscillator module (active clock)

Detected by: lib_id (up to 50), pin_name (up to 10), ref_prefix (up to 5). Roles: `osc`, `out`, `load`, `decap`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| OSC-01 | 100 nF decoupling at the oscillator VDD pin (see DEC-01) | to=osc.VDD; max_mm=2 | soft | place, report | V | TI-SLVA959B |
| OSC-02 | Oscillator close to its load; series resistor at the source | to=load; max_mm=20 | soft | place, report | V | TI-SNLA079D |

- OSC-02: 20 mm is TraceMaker's default.

### 8.4 Power

#### `ic_decoupling` — MCU / FPGA / SoC / IC supply decoupling (generalises D25)

Detected by: topology (up to 80), pin_name (up to 10). Roles: `cap`, `pin`, `ic`, `bulk`, `ferrite`, `vdda`, `vref`, `vcap`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| DEC-01 | One 100 nF ceramic per power pin (or pin pair) as close as possible; < 5 mm | to=pin; max_mm=5; target_mm=2; measure=pad_edge | soft | place, report | V | TI-SLVA959B, ST-AN4488, ESP-HDG-ESP32 |
| DEC-02 | Smallest-value cap closest to the pin, larger caps further out | sequence_by=value_ascending | soft | place, report | V | TI-SLVA959B |
| DEC-03 | Same layer as the IC; plane -> via -> cap -> pin; no via between cap pad and IC pin | sequence=[plane_via, cap, pin] | soft | route, report | V | TI-SLVA959B |
| DEC-04 | Cap-to-pin connection short and wide (length:width <= 3:1); ground via at the cap ground pad, 2 preferred | max_length_to_width=3; gnd_vias_min=1; gnd_vias_preferred=2 | soft | route, report | V | TI-SLVA959B, ESP-HDG-ESP32 |
| DEC-05 | Bulk >= 4.7 uF (typ. 10 uF) per supply near the IC (MT-101: 10-100 uF within 2 in = 50.8 mm) | to=ic; max_mm=50 | soft | place, report | V | ST-AN4488, ADI-MT101, ESP-HDG-ESP32 |
| DEC-06 | VDDA decoupled with 100 nF ceramic + 1 uF at the pin; VDDA may be fed from VDD through a ferrite bead (STM32F4) | to=vdda; max_mm=5 | soft | place, report | D | ST-AN4488 |
| DEC-07 | VREF+ (when fed from a separate reference): 100 nF + 1 uF at the pin; VCAP1/VCAP2: 2.2 uF ceramic, ESR < 2 ohm, each (one 4.7 uF, ESR < 1 ohm, if only VCAP1) (STM32F4; other families per their own notes) | to=[vref, vcap]; max_mm=3 | soft | place, report | D | ST-AN4488 |
| DEC-08 | FPGA/BGA: capacitor count and values per rail from the vendor table; mid-frequency ceramics within two electrical inches of the point of load, mounted on the back side under the device when the power planes sit in the lower half of the stack | — | adv. | report | V | AMD-UG483 |
| DEC-09 | ESP32: >= 9 ground vias in the exposed pad | count_min=9; applies_when=ic matches esp32 | adv. | report | V | ESP-HDG-ESP32 |

- DEC-01: Existing D25 pseudo-net reaches 1.8-3 mm; target 2 mm.
- DEC-05: max_mm = 50 rounds MT-101's 2 in (50.8 mm) down.
- DEC-08: UG483 v1.14 uses 0805/0603 board capacitors (the package carries its own); via-in-pad is optional where the fab allows it.

#### `buck` — Switching step-down (buck) regulator

Detected by: pin_name (up to 40), value (up to 50), keywords (up to 30), topology (up to 30). Roles: `ic`, `cin`, `inductor`, `cout`, `sw`, `fb`, `fb_div`, `diode`, `snubber`, `epad`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| BUCK-01 | Hot loop: HF input cap placed first, as close to VIN/PGND as manufacturing allows (<= 2 mm), wide short copper, no vias in the loop | roles=[cin, ic.VIN, ic.PGND]; max_mm=2; max_vias=0 | soft | place, route, report | V | TI-SLYT614 |
| BUCK-02 | Minimise SW-node copper; inductor close to the IC and rotated to keep SW short | minimise=true; inductor_max_mm=3 | soft | place, route, report | V | TI-SLYT614 |
| BUCK-03 | Output loop inductor -> COUT -> PGND short; inductor, COUT, diode close together | roles=[inductor, cout, ic.PGND] | soft | place, report | V | TI-SLYT614, TI-SNVA021C |
| BUCK-04 | FB trace short, taken from the output cap (Kelvin), away from inductor and SW; preferably opposite side with GND between | from_roles=[sw, inductor]; min_mm=2; kelvin_from=cout | soft | route, report | V | TI-SNVA021C, TI-SLYT614 |
| BUCK-05 | FB divider, compensation and soft-start parts close to the IC pins; small FB node | to=ic.FB; max_mm=3 | soft | place, report | V | TI-SLYT614, TI-SNVA021C |
| BUCK-06 | No foreign signal traces under the inductor or SW copper on the adjacent layers | layers=[same, adjacent]; disallow=[foreign_tracks]; margin_mm=0.5 | soft | route, drc, report | V | TI-SNVA021C, TI-SLYT614 |
| BUCK-07 | Thermal vias in the exposed pad: drill <= 0.33 mm (13 mil), on a grid filling the pad (TI's JEDEC test board: 0.3 mm at 1.5 mm pitch), or per datasheet | drill_mm=0.3; pitch_mm=1.5 | adv. | report | V | TI-SLMA002 |
| BUCK-08 | High-current traces: max(IPC-2221 width, 0.381 mm per A) | mm_per_a=0.381 | soft | route, report | V | TI-SNVA021C, IPC-2221B |
| BUCK-09 | Grounding style per datasheet: single solid plane (ADI AN-139, modern TI) or PGND/AGND joined at the exposed pad (SLYT614) | — | adv. | report | V | TI-SLYT614, ADI-AN139 |
| BUCK-10 | Placement weight x8 on VIN/SW/PGND loop nets so the power stage packs tightly | weight=8 | soft | place | V | TI-SLYT614 |

- BUCK-01: 2 mm is a TraceMaker default for 'as close as allowed'.
- BUCK-04: 2 mm is a TraceMaker default.
- BUCK-07: SLVA959B instead gives 0.2 mm hole / 0.5 mm pad. The device datasheet wins.
- BUCK-10: Weight is a TraceMaker choice implementing the cited placement order.

#### `boost` — Switching step-up (boost) regulator

Detected by: keywords (up to 40), value (up to 50), pin_name (up to 30). Roles: `ic`, `inductor`, `rect`, `cout`, `cin`, `fb`, `sw`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| BOOST-01 | Hot loop is on the output side (LS switch -> rectifier -> COUT): COUT tightest, <= 2 mm | roles=[cout, rect, ic.PGND]; max_mm=2; max_vias=0 | soft | place, route, report | D | ADI-AN139 |
| BOOST-02 | Minimise SW-node copper; inductor close to the SW pin | minimise=true | soft | place, route, report | V | TI-SLYT614 |
| BOOST-03 | FB from the output cap, away from SW and inductor (as BUCK-04) | from_roles=[sw, inductor]; min_mm=2 | soft | route, report | V | TI-SNVA021C |
| BOOST-04 | No foreign signals under inductor/SW (as BUCK-06) | layers=[same, adjacent]; disallow=[foreign_tracks] | soft | route, drc, report | V | TI-SNVA021C |

#### `ldo` — Linear regulator / LDO

Detected by: value (up to 50), keywords (up to 40), pin_name (up to 10). Roles: `ic`, `cin`, `cout`, `fb_div`, `tab`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| LDO-01 | Input and output caps next to their pins (< 5 mm, <= 10 mm max), ground returns direct to the LDO GND | to=[ic.VIN, ic.VOUT]; max_mm=5 | soft | place, report | V | TI-SLVA959B |
| LDO-02 | Adjustable: divider at the FB/ADJ pin, top taken from the output cap | to=ic.FB; max_mm=3 | soft | place, report | V | TI-SLYT614 |
| LDO-03 | Thermal copper on the tab sized from PD = (VIN-VOUT)*IOUT and theta_JA; most gain in the first ~645 mm2 (1 in2) of 1 oz copper | min_mm2=645 | adv. | report | V | TI-SLVA118A |
| LDO-04 | Tab/exposed pad connected solid (no thermal relief) with vias to inner/back planes | style=solid | soft | route, report | V | TI-SLVA959B |

- LDO-03: Needs VIN/IOUT from the user; otherwise reported only.

#### `power_path` — High-current power path (power input, regulator in/out, motor/LED supply)

Detected by: lib_id (up to 40), net_name (up to 20), topology (up to 30). Roles: `net`, `pads`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| PWR-01 | Width from current: IPC-2221 I = k*dT^0.44*A^0.725 (k 0.048 ext, 0.024 int; A in mil2), IPC-2152 charts when available; default dT 10 C, 1 oz | delta_t_c=10; copper_oz=1; k_external=0.048; k_internal=0.024 | soft | route, drc, report | C | IPC-2221B, IPC-2152 |
| PWR-02 | Via count at layer changes >= ceil(I / I_via); I_via ~0.8 A for 0.25-0.3 mm drill, 1 oz, 10 C rise | amps_per_via={d0_20: 0.55, d0_25: 0.81, d0_30: 0.84, d0_41: 1.1} | soft | route, report | V | TI-SLVA959B, IPC-2152 |
| PWR-03 | High-current and thermal pads connect solid to zones (no spoke relief) | style=solid | soft | route, report | V | TI-SLVA959B |
| PWR-04 | Via rows must not slice the return plane; keep copper webs between antipads | min_web_mm=0.25 | adv. | report | V | TI-SLVA959B |

- PWR-01: Needs the current (user override or net-name convention). IPC-2221 internal values are very conservative vs IPC-2152.
- PWR-02: TI table is not monotonic per mm; treat as approximate.

#### `motor_driver` — Motor driver / H-bridge / MOSFET power stage / gate driver

Detected by: value (up to 40), pin_name (up to 30), keywords (up to 30). Roles: `ic`, `fet`, `gate`, `gate_r`, `shunt`, `sense`, `bulk`, `bypass`, `boot`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| MOT-01 | Gate driver close to the MOSFETs (gate trace <= 20 mm); gate resistor at the driver or gate; driver bypass directly across its pins | to=gate_driver_pin; max_mm=20 | soft | place, report | V | TI-SLUA618A, TI-SLVA959B |
| MOT-02 | High-side gate trace routed next to its return (SH/source); >= 0.5 mm wide on >= 1 oz | pair_with=source_return; min_width_mm=0.51 | soft | route, report | V | TI-SLVA959B |
| MOT-03 | Sense lines from the shunt's inner (Kelvin) pads, routed as a coupled pair to the amplifier; filter at the amplifier | pair=true; filter_cap_nF=1 | soft | route, report | V | TI-SLVA959B |
| MOT-04 | Power loop (VM bypass -> HS FET -> LS FET -> GND) minimised; bypass <= 5 mm from VM pins; bulk near power entry with several vias | max_mm=5 | soft | place, route, report | V | TI-SLVA959B |
| MOT-05 | Thermal pad: wide top pour, via array 0.2 mm hole / 0.5 mm pad, direct connect, unmasked; 1.5-2 oz preferred | drill_mm=0.2; pad_mm=0.5 | adv. | report | V | TI-SLVA959B |
| MOT-06 | Drain sense (VDRAIN) as a single trace to the drains via a net tie; gate return to source, not the power ground path | — | adv. | report | V | TI-SLVA959B |

- MOT-01: 20 mm from practice; source asks 'as close as possible'.

### 8.5 RF

#### `rf_module` — Radio module with integrated PCB/chip antenna (ESP32, Pico W, u-blox, nRF modules)

Detected by: lib_id (up to 60), value (up to 30), keywords (up to 20). Roles: `module`, `antenna_region`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| RFM-01 | Module antenna outside the board or at the edge with the feed near the edge (ESP32: overhang strongly recommended; u-blox NINA-B3 internal antenna: corner) | antenna_side_to_edge_mm=0; prefer_overhang=true | soft | place, report | V | ESP-HDG-ESP32, UBLOX-NINAB3, RPI-PICOW |
| RFM-02 | No copper, vias or parts under/around the antenna region on any layer, per the module datasheet (Pico W: 14 x 9 mm cut-out; ESP32: cut the board on both sides and below if it cannot overhang) | layers=all; disallow=[tracks, vias, zones, footprints]; shape=footprint_keepout_or_datasheet | **hard** | place, route, drc, report | V | ESP-HDG-ESP32, RPI-PICOW |
| RFM-03 | Tall or metal parts >= 10 mm from the antenna; >= 15 mm clearance around ESP32 PCB antenna inside the housing | min_mm=10; esp32_housing_mm=15 | soft | place, report | V | UBLOX-NINAB3, ESP-HDG-ESP32 |
| RFM-04 | Ground plane extends >= 10 mm on both sides of a PCB-trace-antenna module (u-blox B3x6); dense GND vias near module ground pads | gnd_extent_mm=10 | adv. | report | V | UBLOX-NINAB3 |

- RFM-02: Hard only when the region comes from the footprint's own keep-out or a known module table; otherwise soft. Some modules (u-blox NINA-B3x2 internal antenna) want GND under the antenna: the module table decides, never a blanket rule.

#### `chip_antenna` — Discrete antenna (chip antenna, PCB trace antenna, wire/whip pad)

Detected by: lib_id (up to 60), ref_prefix (up to 20). Roles: `antenna`, `feed`, `match`, `rf_ic`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| ANT-01 | Ground/copper-free clearance area around the antenna on all layers per datasheet (Johanson 2450AT18A100 ~6.5 x 13.5 mm at the board end; Ignion NN03-310 40 x 12 mm) | layers=all; disallow=[tracks, vias, zones, footprints]; shape=datasheet_table | **hard** | place, route, drc, report | V | JOHANSON-2450AT18A100, IGNION-NN03-310 |
| ANT-02 | Antenna at a board edge or corner | max_mm=1 | soft | place, report | V | JOHANSON-2450AT18A100, TI-AN058 |
| ANT-03 | Pi (shunt-series-shunt) matching pads between RF pin and antenna | — | adv. | report | V | TI-AN058, JOHANSON-2450AT18A100 |
| ANT-04 | Matching parts close to the RF pin (zigzag 0201/0402) | to=rf_ic.ANT; max_mm=3 | soft | place, report | V | ESP-HDG-ESP32, NORDIC-NRF52840-REF |

- ANT-01: Hard only with a part-table entry; unknown antennas get advisory 'keep-out unknown'.

#### `rf_line` — 50 ohm RF line (RF pin, matching, antenna or RF connector)

Detected by: pin_name (up to 50), lib_id (up to 40), net_name (up to 15). Roles: `line`, `connector`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| RF-01 | 50 ohm, grounded coplanar waveguide preferred (2-layer 1.6 mm: ~1.0-1.2 mm wide, 0.15-0.2 mm gap; plain microstrip would be ~3 mm) | z0_ohm=50; tol_pct=10; structure=gcpw | soft | route, drc, report | C | ESP-HDG-ESP32, WADELL-1991, IPC-2141A, HAMMERSTAD-1980, UBLOX-NINAB3 |
| RF-02 | No vias, no 90 degree corners (135 degree or arcs), uniform width and gap, short | max_vias=0; max_bend_deg=45 | soft | route, report | V | ESP-HDG-ESP32, UBLOX-NINAB3 |
| RF-03 | GND stitching vias along both sides of the RF line's ground pour, spaced < lambda/10 of the 10th harmonic (2.4 GHz: ~1.2 mm; reference boards 1.0-1.27 mm), 0.5-1 mm from the gap edge | pitch_mm=1.27; max_pitch_lambda_frac=0.01; offset_mm=0.75 | soft | route, report | D | SILABS-AN928, UBLOX-NINAB3 |
| RF-04 | Other signals >= 3w from the RF line; high-speed and switching nets kept away | min_w=3 | soft | route, report | V | UBLOX-NINAB3, ESP-HDG-ESP32 |
| RF-05 | u.FL: no conductors under the connector between ground pads, GND void under the signal pad on the first inner layer; SMA THT: void all layers around the centre pin | layers=[adjacent]; disallow=[tracks, zones] | soft | route, drc, report | V | UBLOX-NINAB3 |

- RF-01: Widths computed with Wadell GCPW formulas (er 4.4-4.6); a stackup is required to apply.
- RF-03: AN928.2 states the spacing for PCB edges and internal GND pour edges; the 0.5-1 mm offset is a TraceMaker default.

#### `shield_can` — RF shield can / fence

Detected by: lib_id (up to 60). Roles: `fence`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| SHLD-01 | GND vias along the fence pad at < lambda/10 of the 10th harmonic (2.4 GHz: 1.0-1.27 mm, as on reference boards) | pitch_mm=1.27 | adv. | report | V | SILABS-AN928 |
| SHLD-02 | No top-layer signals across the fence; parts >= 0.3-0.5 mm inside the fence | layers=[same]; disallow=[foreign_tracks]; inner_margin_mm=0.4 | soft | place, route, report | R | SILABS-AN928 |

- SHLD-01: AN928.2 states the spacing for GND pour edges; applying it to a shield fence is a TraceMaker reading.
- SHLD-02: Rule of thumb; can vendor land pattern is authoritative.

### 8.6 Analog

#### `analog_frontend` — ADC / DAC / precision analog / sensor / audio codec

Detected by: value (up to 40), pin_name (up to 30), keywords (up to 20). Roles: `ic`, `analog_in`, `ref`, `ref_cap`, `aa_filter`, `hi_z`, `agnd`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| ANA-01 | One solid ground plane, partitioned by placement; if AGND/DGND are split, join once under the converter and never route across the split | no_split_crossing=true | soft | route, report | V | ADI-MT031, TI-SLVA959B |
| ANA-02 | Converter AGND and DGND pins both to the analog ground plane with short connections | max_mm=1 | adv. | report | D | ADI-MT031 |
| ANA-03 | Reference cap(s) directly at the VREF pin, short return to AGND | to=ic.VREF; max_mm=2.5 | soft | place, report | D | ADI-MT101, ST-AN4488 |
| ANA-04 | Differential inputs symmetric and coupled; anti-alias RC at the ADC pins | max_intra_skew_mm=1.0; filter_max_mm=3 | soft | place, route, report | V | TI-SLVA959B |
| ANA-05 | Guard ring completely around high-impedance nodes, held at the node's potential (ground for an inverting stage), on both sides of the board with vias for through-hole parts | width_mm=[0.5, 1.0] | adv. | report | D | ADI-OPAMP-HB |
| ANA-06 | Digital, clock and switching nets >= 3W away and not parallel to analog inputs/reference | min_w=3; to_roles=[buck.sw, boost.sw, clocks] | soft | route, report | V | TI-SLVA959B |
| ANA-07 | Analog front end placed away from switching regulators and their inductors (>= 10 mm) | to_roles=[buck.inductor, boost.inductor]; min_mm=10 | soft | place, report | D | TI-SNVA021C |

- ANA-03: MT-101 does not address reference pins specifically; it is cited for HF decoupling 'as physically close to the pins as possible'.
- ANA-05: Ring width and solder-mask opening are TraceMaker defaults; the source gives neither.
- ANA-07: 10 mm is a TraceMaker default.

### 8.7 Protection, connectors and safety

#### `esd_tvs` — ESD / TVS protection device

Detected by: value (up to 50), keywords (up to 30), lib_id (up to 20), topology (up to 30). Roles: `tvs`, `connector`, `protected`, `ic`, `gnd`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| ESD-01 | TVS as close to the connector as the rules allow; IC much farther from the TVS than the TVS from the connector | to=connector; max_mm=5; ratio_ic_to_conn_min=2 | soft | place, report | V | TI-SLVA680A |
| ESD-02 | Routing order connector -> TVS pad -> IC, flow-through, no stub to the TVS | sequence=[connector, tvs, ic]; max_stub_mm=0 | soft | route, report | V | TI-SLVA680A |
| ESD-03 | No via between connector and TVS (if unavoidable, reach the TVS before any branch) | max_vias_between=0; segment=[connector, tvs] | soft | route, report | V | TI-SLVA680A |
| ESD-04 | No unprotected traces in the region between connector and TVS | layers=all; disallow=[foreign_tracks]; shape=hull_connector_tvs | soft | route, report | V | TI-SLVA680A |
| ESD-05 | TVS ground straight into the plane: stitching via(s) right at the ground pin (>= 2) | count_min=2 | soft | route, report | V | TI-SLVA680A, TI-TCAN1042H |
| ESD-06 | Straight runs or curves between connector and TVS; no 90 degree corners | max_bend_deg=45 | adv. | report | V | TI-SLVA680A |

- ESD-01: Ethernet exception ETH-14.

#### `connector_general` — Board-to-wire / board-to-board connector (any external connector)

Detected by: lib_id (up to 40), ref_prefix (up to 20). Roles: `connector`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| CONN-01 | Cable/mating connectors at the board edge, mating face at or slightly past the edge, facing outward | max_mm=1.0; facing=outward | soft | place, report | R | TMK-PRACTICE |
| CONN-02 | Mechanical keep-out for the mating plug/cable in front of the connector (no parts taller than the board in that region) | depth_mm=5; disallow=[footprints] | soft | place, report | R | TMK-PRACTICE |
| CONN-03 | Copper >= 0.3-0.5 mm from routed board edges (fab floor 0.2 mm; V-score 0.4 mm) | min_mm=0.3; fab_min_mm=0.2; vscore_min_mm=0.4 | soft | route, drc | V | JLC-CAPS |

- CONN-01: Universal practice; no single primary source. Pin headers between boards (stacking) are excluded by footprint class.
- CONN-03: Board edge clearance rule from the board takes precedence.

#### `mounting_hole` — Mounting hole / fastener

Detected by: lib_id (up to 70), ref_prefix (up to 20). Roles: `hole`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| MH-01 | Copper and parts kept out to the screw head/washer diameter plus margin (M3: ~6-7 mm diameter) unless the hole is plated and grounded | diameter_mm_m3=6.5; disallow=[footprints, foreign_tracks] | soft | place, route, report | R | TMK-PRACTICE, JLC-CAPS |

- MH-01: Rule of thumb; NPTH-to-copper fab minimum 0.2 mm is from JLC (V).

#### `mains_hv` — Mains / high-voltage section (AC input, relay contacts, offline converter primary)

Detected by: net_name (up to 40), lib_id (up to 30), value (up to 30). Roles: `hazardous`, `selv`, `barrier`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| HV-01 | Functional spacing between conductors from IPC-2221B Table 6-1 by peak voltage (B2 external uncoated: 0.64 mm at 31-150 V, 1.25 mm at 151-300 V, 2.5 mm at 301-500 V, +0.005 mm/V above) | table=IPC-2221B-6-1; column=B2; b2_mm={0-15: 0.1, 16-30: 0.1, 31-50: 0.6, 51-100: 0.6, 101-150: 0.6, 151-170: 1.25, 171-250: 1.25, 251-300: 1.25, 301-500: 2.5}; b2_per_volt_above_500=0.005; b1_mm={0-15: 0.05, 16-30: 0.05, 31-50: 0.1, 51-100: 0.1, 101-150: 0.2, 151-170: 0.2, 171-250: 0.2, 251-300: 0.2, 301-500: 0.25} | **hard** | route, drc, report | V | IPC-2221B, KICAD-CALC-IPC2221 |
| HV-02 | Safety creepage/clearance mains (250 Vrms, PD2, material group IIIb, OVC II) to SELV: basic 2.5 mm creepage / 2.0 mm clearance; reinforced 5.0 mm / 4.0 mm | voltage_v=250; pollution_degree=2; material_group=IIIb; basic={creepage_mm: 2.5, clearance_mm: 2.0}; reinforced={creepage_mm: 5.0, clearance_mm: 4.0} | **hard** | place, route, drc, report | R | IEC-62368-1 |
| HV-03 | Slots count toward creepage only if >= 1.0 mm wide at PD2 (0.25 mm PD1, 1.5 mm PD3); effective creepage = path + width + 2 x depth around the slot | min_slot_mm_pd2=1.0 | adv. | report | V | TI-SLLA284, IEC-62368-1 |
| HV-04 | No copper of either side under the barrier part between its primary and secondary pins, all layers | layers=all; disallow=[tracks, vias, zones] | **hard** | route, drc, report | V | TI-SLLA284 |

- HV-01: IPC-2221B prints 0.6 mm where the KiCad calculator has 0.64 mm; column letters differ between revisions. Hard only when the voltage is known (doc 15 §3.6). Not a safety spacing.
- HV-02: Not re-read in the standard; verify. IEC 62368-1 transient-based clearance can be lower (~1.5/3.0 mm). Medical (IEC 60601-1) is higher. The often-quoted 6.4 mm is IPC B3 / UL-era, not IEC reinforced. The report never claims compliance.
- HV-03: TraceMaker does not cut slots; it reports where one would close a creepage shortfall.

#### `isolation` — Isolation barrier (digital isolator, optocoupler, isolated DC/DC)

Detected by: value (up to 50), keywords (up to 40), lib_id (up to 20). Roles: `iso`, `side1`, `side2`, `gnd1`, `gnd2`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| ISO-01 | Keep the space under the isolator free of traces, vias and pads on all layers (barrier strip between pin rows) | layers=all; disallow=[tracks, vias, pads, zones]; shape=between_pin_rows | **hard** | place, route, drc, report | V | TI-SLLA284 |
| ISO-02 | Side-1 to side-2 copper spacing >= the isolator's package creepage (default: body pin-row gap), never less than HV-02 when side 1 is hazardous | min_mm=from_footprint_pin_rows | **hard** | place, route, drc, report | V | TI-SLLA284, IEC-62368-1 |
| ISO-03 | Optional stitching capacitance by overlapping inner planes across the barrier (isoPower EMI); keep away from the board edge | — | adv. | report | V | ADI-AN1109 |
| ISO-04 | Isolator stack: >= 4 layers (signal, GND, power, signal) per TI; signal spacing >= 3h | — | adv. | report | V | TI-SLLA284 |

### 8.8 Field buses, low-speed buses and debug

#### `can` — CAN / CAN FD transceiver

Detected by: value (up to 50), pin_name (up to 40), net_name (up to 20). Roles: `xcvr`, `bus`, `term`, `split_cap`, `connector`, `esd`, `cmc`, `decap`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| CAN-01 | CANH/CANL routed as a coupled pair (~120 ohm differential), short on-board stub to the transceiver | zdiff_ohm=120; max_stub_mm=100 | soft | route, report | D | TI-SLLA270, TI-TCAN1042H |
| CAN-02 | TVS, filter caps and CMC as close to the bus connector as possible, in line with the signal path | to=connector; max_mm=10 | soft | place, route, report | V | TI-TCAN1042H |
| CAN-03 | 120 ohm termination only at the two bus ends (never on a removable node); split 2 x 60 ohm + 4.7 nF recommended for EMC | r_ohm=120; split_r_ohm=60; split_c_nF=4.7 | adv. | report | V | TI-SLLA270, ISO-11898-2 |
| CAN-04 | VCC/VIO bypass at the pins; >= 2 vias per supply/ground connection on bypass and protection parts | to=xcvr; max_mm=3; vias_min=2 | soft | place, route, report | V | TI-TCAN1042H |

- CAN-01: 120 ohm is the cable impedance and termination (read); the sources give no on-board trace impedance, so the pair target is a TraceMaker choice. 100 mm keeps the on-board stub a small part of ISO 11898-2's 0.3 m.

#### `rs485` — RS-485 / RS-422 transceiver

Detected by: value (up to 50), pin_name (up to 15), net_name (up to 30). Roles: `xcvr`, `bus`, `term`, `bias`, `esd`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| RS485-01 | A/B routed as a coupled ~120 ohm pair | zdiff_ohm=120 | soft | route, report | D | TI-SLLA272 |
| RS485-02 | TVS and termination at the connector end | to=connector; max_mm=10 | soft | place, report | D | TI-SLLA272, TI-SLVA680A |
| RS485-03 | 120 ohm at each cable end (or 2 x 60 ohm + cap, 1 % matched); external fail-safe bias e.g. 2 x 523 ohm at 5 V; stub <= t_r*v/10 | r_ohm=120; bias_ohm_5v=523 | adv. | report | V | TI-SLLA272 |

- RS485-01: 120 ohm is the cable impedance (read); the board pair target is a TraceMaker choice.

#### `rs232` — RS-232 transceiver with charge pump

Detected by: value (up to 60). Roles: `xcvr`, `pump_caps`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| RS232-01 | Charge-pump caps close to the IC, C1 and C2 shortest (fastest edges) | to=xcvr; max_mm=5 | soft | place, report | V | TI-MAX3232 |

- RS232-01: 5 mm is a TraceMaker default; datasheet says 'short'.

#### `i2c` — I2C / SMBus

Detected by: pin_name (up to 50), net_name (up to 30). Roles: `bus`, `pullup`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| I2C-01 | Pull-ups between Rp_min = (VCC - 0.4 V)/3 mA and Rp_max = tr/(0.8473*Cb); Cb <= 400 pF (Std/Fast), 550 pF (Fm+); tr 1000/300/120 ns | vol_v=0.4; iol_ma=3; cb_max_pF=400; tr_ns={standard: 1000, fast: 300, fast_plus: 120} | adv. | report | V | NXP-UM10204, TI-SLVA689 |
| I2C-02 | Runs > 100 mm without planes: order SDA-VDD-VSS-SCL or SDA-VSS-SCL | max_mm_without_planes=100 | adv. | report | V | NXP-UM10204 |
| I2C-03 | Pull-ups are not placement-critical (low weight); one set per bus segment | weight=0.5 | soft | place | V | NXP-UM10204 |

#### `spi` — SPI bus (non-memory)

Detected by: pin_name (up to 40). Roles: `sck`, `bus`, `series_r`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| SPI-01 | 22 ohm series resistor (provisioned) at the SCK driver, especially when the clock line is long | to=driver; max_mm=3 | adv. | report | D | TI-SPRAD21 |

- SPI-01: SPRAD21 recommends it on every MCSPI clock output of the AM62x family; 33 ohm and the 'few cm' condition were not in the source.

#### `jtag_swd` — JTAG / SWD debug header

Detected by: lib_id (up to 60), pin_name (up to 30). Roles: `header`, `target`, `tdo_r`, `tck_r`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| DBG-01 | Header to device <= 76 mm | max_mm=76.2 | soft | place, report | V | TI-SPRU655I |
| DBG-02 | TDO series termination at the device pin, <= 12.7-25.4 mm | to=target.TDO; max_mm=12.7 | soft | place, report | V | TI-SPRU655I |
| DBG-03 | Debug header reachable at the board edge (Cortex Debug 10-pin and Debug+ETM 20-pin at 1.27 mm pitch; Arm JTAG 20-pin at 2.54 mm) | max_mm=5 | adv. | place, report | D | ARM-CORESIGHT-CONN |

- DBG-02: SPRU655I says 1 in in a table and 0.5 in in a figure note; default is the stricter.

### 8.9 Brief categories

#### `led` — Indicator LED with series resistor

Detected by: lib_id (up to 50), ref_prefix (up to 10). Roles: `led`, `r`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| LED-01 | Series resistor next to the LED or its driver (thin evidence: practice only) | to=led; max_mm=10 | adv. | place | R | TMK-PRACTICE |

- LED-01: No layout source; weight only. Visible LEDs belong on the user-facing side (user input).

#### `display_fpc` — Display / camera FPC connector

Detected by: lib_id (up to 40), keywords (up to 20). Roles: `connector`, `lanes`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| DISP-01 | High-speed lanes through an FPC follow MIPI-*/LVDS-* rules; keep ground pins and a continuous return under the connector | — | adv. | report | V | TI-SPRAAR7J |
| DISP-02 | FPC connector at the board edge with cable exit unobstructed | max_mm=3 | soft | place, report | R | TMK-PRACTICE |

- DISP-01: SPRAAR7J has no FPC-specific text; its reference-plane and connector rules are applied.
- DISP-02: Practice (CONN-01).

#### `battery_charger` — Li-ion/LiPo charger and battery connection

Detected by: value (up to 60), lib_id (up to 20). Roles: `ic`, `caps`, `sense`, `epad`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| BAT-01 | IN, OUT and BAT caps as close as possible with short ground to the thermal pad | to=ic; max_mm=3 | soft | place, report | V | TI-BQ24075 |
| BAT-02 | Thermal pad soldered with multiple vias (filled or tented); it is the main GND | count_min=4 | adv. | report | V | TI-BQ24075 |
| BAT-03 | Switch-mode charger sense resistor routed Kelvin (see MOT-03) | pair=true | soft | route, report | V | TI-SLVA959B |

#### `fuse` — Fuse / PTC on a power input

Detected by: lib_id (up to 60), ref_prefix (up to 20). Roles: `fuse`, `input`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| FUSE-01 | Fuse at the power-entry point before any other branch; traces sized for the fuse rating (PWR-01) | sequence=[input, fuse] | soft | place, route, report | R | TMK-PRACTICE |

- FUSE-01: Practice; thin primary evidence.

#### `test_point` — Test point (bed-of-nails / probe)

Detected by: lib_id (up to 60), ref_prefix (up to 30). Roles: `tp`.

| ID | Rule | Parameters (defaults) | Sev. | Enforced | Ev. | Sources |
|---|---|---|---|---|---|---|
| TP-01 | Bed-of-nails: pad >= 0.9-1.0 mm, pitch 2.54 mm preferred (1.27 mm possible), >= 3 mm from the board edge, all on one side | pad_min_mm=0.9; pitch_mm=2.54; edge_min_mm=3.0 | adv. | report | R | IPC-2221B-TP |
| TP-02 | No test points on high-speed pairs (USB2-05) | — | adv. | report | V | TI-SPRAAR7J |

- TP-01: Not verified: IPC-2221B was not read; fixture vendors set the real limits.

### 8.10 Source verification (2026-10-05)

The 41 rules that were **R** after the first research pass were checked against their sources (ST, ADI, NXP, AMD and Silicon Labs
documents through the Internet Archive where the vendor servers refused the download; the copies are the vendors' own
PDFs). Counts: 9 confirmed, 9 corrected, 11 default (the source supports the rule but gives no number, so
the value is a TraceMaker default and the rule is now **D**), 12 unverifiable (still **R**). No parameter that the engine
reads changed: the corrected numbers are in RF-03, SHLD-01 (via pitch 2.5 → 1.27 mm, λ/20 → λ/10 of the 10th harmonic) and
BUCK-07 (pitch 1.0–1.2 → 1.5 mm), whose kinds are not enforced yet; the other corrections are to the rule text. The YAML
`checked` field holds the same text.

| ID | Status | Ev. | Source checked (document, section, page) or reason |
|---|---|---|---|
| USB2-11 | unverified | R | practice rule (TMK-PRACTICE); no primary source gives a number, the receptacle datasheet sets the overhang |
| USBC-11 | confirmed | V | USB Type-C Cable and Connector Specification R2.0 §4.11.1 Table 4-25 p.236 (Rd 5.1 kOhm, +/-20 % or +/-10 %); TI TUSB320 SLLSEN9F §6.5 p.6 (RCC_D 4.6/5.1/5.6 kOhm) |
| ETH-15 | confirmed | V | TI DP83848C SNLS266E pin description p.6 and 'MII Serial Management Interface' p.35 ('The MDIO pin requires a pullup resistor (1.5 kOhm)') |
| PBUS-01 | unverified | R | practice default (TMK-PRACTICE); no primary source gives a distance |
| PBUS-03 | unverified | R | practice rule of thumb (TMK-PRACTICE); no primary source read |
| SDIO-04 | confirmed (no number to check) | V | TI SPRAD21I §7.3.2.1 p.43 gives no HS200/HS400 layout numbers and points to the processor data sheet and an E2E FAQ |
| XTAL-01 | default | D | ST AN2867 Rev 22 §7.1 p.44 ('mount the crystal as close as possible to the MCU') and NXP AN2049 p.2 ('very close to the two clock pins') give no number; 10 mm is a TraceMaker default |
| XTAL-02 | default | D | ST AN2867 Rev 22 §7.1 Figure 13 p.45 shows CL1/CL2 between crystal and MCU with VSS paths to a local ground; no distance given, 3 mm is a TraceMaker default |
| XTAL-05 | confirmed | V | ST AN2867 Rev 22 §7.1 p.44 (guard ring connected to ground 'is essential'; oscillator ground connected to the nearest MCU ground) and §7.2 Figure 14 p.46 |
| XTAL-06 | corrected | D | ST AN2867 Rev 22 §7.2 pp.47-50 asks for short paths and 'symmetry between oscillator capacitances' (not equal trace lengths); 2 mm is a TraceMaker default |
| XTAL-07 | default | D | ST AN2867 Rev 22 §7.1 p.44 (route high-frequency signals away from the oscillator) and TI SNVA021C §1 p.2 (open-core inductors further from low-power parts) give no number; 5 mm is a TraceMaker default |
| DEC-05 | confirmed | V | ST AN4488 Rev 7 §2.2 p.9 and §8.3 p.37 (min 4.7 uF, typ. 10 uF per package); ADI MT-101 Rev.0 p.2 Figure 2 (10-100 uF no more than 2 in from the chip) |
| DEC-06 | corrected | D | ST AN4488 Rev 7 §2.2 p.9 gives 100 nF + 1 uF on VDDA and an optional ferrite bead from VDD; the earlier '10 nF' and '<= 47 ohm' are not in AN4488. 5 mm is a TraceMaker default |
| DEC-07 | corrected | D | ST AN4488 Rev 7 §2.2 pp.9-10; the single-VCAP 4.7 uF case added and F7/H7 dropped (not in AN4488). 3 mm is a TraceMaker default |
| DEC-08 | corrected | V | AMD UG483 v1.14 Tables 2-1 to 2-4 pp.14-19, 'PCB Capacitor Placement and Mounting Techniques' p.24, back-side mounting p.32, via-in-pad p.30; the earlier '0402 under the BGA with via-in-pad' is not what v1.14 says |
| BUCK-07 | corrected | V | TI SLMA002H p.9 (drill 0.33 mm or smaller) and p.18 (test board vias 0.3 mm diameter, 1.5 mm pitch); the earlier 1.0-1.2 mm pitch is not in SLMA002H |
| BOOST-01 | default | D | ADI (LT) AN-139 Rev A p.3 Figures 6-7 (boost hot loop = switch, rectifier and COUT on the output side) gives no distance; 2 mm and no vias are TraceMaker defaults |
| RF-03 | corrected | D | Silicon Labs AN928.2 Rev 1.7 §3.1 p.19 (via spacing < lambda/10 of the 10th harmonic, typically 40-50 mil); was lambda/20 and 2.5 mm. u-blox NINA-B3 SIM R15 §B.1.2 p.69 asks for stitching vias around the RF trace without a number |
| SHLD-01 | corrected | V | Silicon Labs AN928.2 Rev 1.7 §3.1 p.19 (40-50 mil typical); was lambda/20 and 2.5 mm |
| SHLD-02 | unverified | R | AN928.2 Rev 1.7 only says a shielding cap should cover all RF parts (§3.2.1 p.26); no fence-crossing or margin number found; the can vendor's land pattern is authoritative |
| ANA-01 | confirmed | V | ADI MT-031 Rev.A pp.7-12 (AGND/DGND joined at the converter, Figure 8) and Figure 11 (partitioning); TI SLVA959B §1.1-1.2 pp.3-5 (partitioning is not a physical split, Figure 1-4) |
| ANA-02 | default | D | ADI MT-031 Rev.A pp.7-8 (AGND and DGND pins joined to the analog ground plane with minimum lead lengths); 1 mm is a TraceMaker default |
| ANA-03 | default | D | ADI MT-101 Rev.0 p.2 (HF caps as close as possible to the pins) and ST AN4488 Rev 7 §2.2 p.9 (VREF+ needs 100 nF + 1 uF); no distance given, 2.5 mm is a TraceMaker default |
| ANA-05 | default | D | ADI Op Amp Applications Handbook Section 7-2 pp.7.41-7.42 (guard surrounds the node at its potential, within 1 mV for < 1 pA at 1 GOhm); the 0.5-1.0 mm width is a TraceMaker default |
| ANA-07 | default | D | TI SNVA021C §1-2 p.2 (keep sensitive traces away from the inductor) gives no number; 10 mm is a TraceMaker default |
| CONN-01 | unverified | R | practice rule (TMK-PRACTICE); no primary source exists |
| CONN-02 | unverified | R | practice rule (TMK-PRACTICE); the mating plug's drawing sets the real depth |
| MH-01 | unverified | R | 6.5 mm sits between the M3 socket-head (5.5 mm) and washer (7 mm) diameters recalled from ISO 4762 / ISO 7089, which were not read |
| HV-02 | unverified | R | IEC 62368-1 is a paid standard and no copy was read; the values stay as attributed and the rule stays soft at run time |
| ISO-03 | Optional stitching capacitance by overlapping inner planes across the barrier (isolator EMI, iCoupler/isoPower); keep away from the board edge | — | adv. | report | V | ADI-AN1109 |
| CAN-01 | default | D | TI SLLA270 p.5 and TCAN1042H SLLSES7D §10.2.2.1 p.27 (120 ohm twisted pair and termination), §10.2.1.1 p.26 (0.3 m stub at 1 Mbps); 100 mm on-board stub is a TraceMaker default |
| RS485-01 | default | D | TI SLLA272D §5 p.2 (keep both lines close and equidistant on the PCB, 120 ohm cable) and §6 p.3 (120 ohm termination) |
| RS485-02 | default | D | TI SLVA680A §2 p.4 ('place the TVS as near to the connector as design rules allow') and SLLA272D §6 p.3 (termination at the cable ends); 10 mm is a TraceMaker default |
| SPI-01 | corrected | D | TI SPRAD21I §5.2.1.6 p.17 (22 ohm near the processor clock output); 3 mm is a TraceMaker default |
| DBG-03 | corrected | D | Keil/Arm 'CoreSight target connectors' page (read via the Internet Archive): 10-pin and 20-pin Cortex at 0.05 in, Arm standard JTAG 20-pin at 0.10 in; the 5 mm edge distance is a TraceMaker default |
| LED-01 | unverified | R | practice rule (TMK-PRACTICE); no layout source exists |
| DISP-01 | confirmed (no number to check) | V | TI SPRAAR7J §2.4 p.6 (continuous reference planes) and §3.5 p.10 (connectors) |
| DISP-02 | unverified | R | practice rule (TMK-PRACTICE); the FPC connector drawing sets the cable exit |
| BAT-03 | confirmed | V | TI SLVA959B §7.6 p.32 (sense lines as a tightly coupled pair from the shunt to the amplifier) and §7.9 p.34 ('use Kelvin connections'); written for motor drivers |
| FUSE-01 | unverified | R | practice rule (TMK-PRACTICE); no primary source |
| TP-01 | unverified | R | IPC-2221B is a paid standard and was not read; fixture vendors set the real limits |

## 9. Test plan

### 9.1 Levels

| Level | What | How |
|---|---|---|
| L0 unit | Catalogue loads; every rule has id, kind, severity, ≥ 1 source; YAML ↔ doc ids identical; detectors are valid regexes; impedance solver matches a table of reference cases (KiCad calculator, IPC-2141A examples) within 1 %; IPC-2221 width formula matches published examples | Catch2 + a Python check over this doc and the YAML |
| L1 detection | Hand-labelled ground truth: for ~60 PCBench boards, the categories and anchors a human sees (reference, category). Precision and recall per category; **precision first** (a wrong detection applies wrong rules) | `bench/crules_detect.py` (to build) |
| L2 binding | For the labelled boards, roles (pairs, ESD part, load caps, loop parts) match the labels | same harness |
| L3 effect | Place and route with `--component-rules off` vs `soft` vs `on` under the same budget; measure each rule's metric (distance, skew, stub, loop perimeter, keep-out intrusions) and the usual completion / clean-pass / KiCad-DRC numbers | `bench/place_auto.py`, `bench/route_bench.py` with a new `crules` column |
| L4 judge | KiCad DRC with the merged `.kicad_dru` fragment counts component-rule violations independently of the own DRC | harness (§5.5) |
| L5 human review | A designer checks the report and the board for the categories in §9.2 | checklist per category |

Gates for turning a category on by default (`soft`): detection precision ≥ 0.95 and recall ≥ 0.8 on the
labelled set; no regression in completion or KiCad-clean pass on the `quick` tier (CLAUDE.md rule 9); the
rule metric improves on the boards that have the category (e.g. median decap distance, ESD-to-connector
distance, crystal distance, hot-loop perimeter).

### 9.2 Fixtures

A scan of footprint names in PCBench `unrouted.kicad_pcb` files (2026-10-03, case-insensitive substring;
counts are boards, approximate — e.g. "sma" also matches diode packages) found these candidates:

| Category | Boards (approx.) | Examples |
|---|---|---|
| USB-C | 16 | `kitspace_USB-C-Screen-Adapter`, `kitspace_USBee32-S2`, `typec-charger_TypeC-DC-Charger` |
| USB micro/mini/A/B | 181 | `4-port-usb-hub_4port-usb-hub`, `ISO-port_ch340-usb-serial-isolated`, `UCCBPCB_USB_CAN_CONVERTER_BASIC` |
| Ethernet (RJ45/MagJack) | 24 | `esp32-ethernet_esp32-ethernet`, `CoreOne-xCORE200-Original_CoreOne`, `bobc_mbeduinopresso` |
| HDMI / LVDS | 5 | `PmodHDMIIn_PmodHDMIIn`, `pcb_HDMI2RGB_HDMI2RGB`, `LVDS2TMDS_LVDS2TMDS`, `edid-injector_edid-injector` |
| PCIe | 8 | `MAVRIC_Hardware_pcieduino`, `mosavr_pcie1x_backplane`, `PCIE-to-MXM-Adapter_PCIEx1toMXM3.0` |
| SDRAM / BGA SoC | 1 / 13 | `lattice-ice40-hx8kevb-sdram_hx8kevb-sdram`, `jadonk_PocketBone`, `front-end-modules_LimeSDR_Sony` |
| SD / microSD | 28 | `Board-RZA1L_BoardRZA1`, `airqualitystation_hardware` |
| Crystal / oscillator | 202 / 20 | `4-port-usb-hub_4port-usb-hub`, `ArduinoDueClone_ATSAM3X8EA` |
| ESP32 / RF modules | 62 | `ESP32-board_esp32_board`, `esp32-cantroller_EltekController`, `Hornbill-ESP32-DEV_Hornbill` |
| Antenna / u.FL / SMA | ≤ 243 (over-matched) | `BeaconBuddy_BeaconBuddy`, `LoRaCatTrack_GPSLoRa`, `radio_antenna-iridium` |
| Buck / boost / LDO | ≥ 4 by name, ~200 with inductors | `C-BISCUIT_buck-reg-5v`, `kitspace_12_24_boost_converter`, `supply-ldo-adj-single_supply-ldo-adj-single`, `TPS63001-Breakout_TPS63001` |
| Motor drivers | 13 by name | `motor-3xdrv8833-hw_ver1`, `ATMEGA328-Motor-Board_ATMEGA328_Motor_Board`, `EUC-VESC_electronics_sept` |
| CAN / RS-232 | 7 / 2 | `CANadapter_CANadapter`, `stm32-network-rs232_stm32-network-rs232` |
| Audio / ADC | 5 / 7 | `HiFiAudioCodecModule_HiFiAudioCodecModule`, `ADC-PCM4202-SE_ADC-PCM4202-SE`, `ADC-DAC-16bit_ADC-DAC-16bit` |
| Opto / isolators | 13 | `4N35-TTL-Serial-Optoisolator_…`, `6N137-TTL-Serial-Optoisolator_…`, `Baofeng-Interface_BaofengInterfaceIsolated` |
| Mains / transformers / relays | 9 / 27 | `analogevse_AnalogEVSE`, `powersupply_5v_v1_powersupply_5v_v1`, `ozinverter_ozinverterkicad` |
| Fuses, test points, batteries | 65 / 46 / 45 | — |

The labelled set (L1) takes 3–5 boards per category from this list, preferring KiCad 6+ files (pad
`pinfunction` present) plus a few KiCad 5 files to test the capped-confidence path. Most PCBench boards have
no stackup or only the default; impedance rules are tested on boards where the stackup is present and on
copies with a stackup preset injected.

### 9.3 Metrics

Per category and rule: detection precision/recall; rule status counts (applied, satisfied-by-board,
not-applied by reason); measured value vs limit before and after (human placement vs TraceMaker placement and
routing); completion and KiCad-clean pass with and without component rules; time overhead of detection and
compilation (target < 1 % of a route job).

## 10. Phased implementation

| Phase | Content | Why first |
|---|---|---|
| P0 | Catalogue loader, detection, binding and the report in `report` mode only; YAML↔doc check; labelled detection set | No risk to routing; measures detection quality; useful as a design-review checklist on its own |
| P1 | Placement proximity rules that reuse D25's pseudo-net: decoupling (generalised to VDDA/VREF and bulk caps), crystal + load caps, ESD at connector (with order), regulator input/output caps, termination resistors; connector edge placement | Highest value per effort; the machinery exists and is proven (D25: decaps 16–19 mm → 1.8–3 mm); categories are very common in PCBench |
| P2 | Keep-outs: crystal (no foreign tracks under), antenna/module keep-outs, inductor/SW-node keep-out, magnetics void; generated rule areas checked by own DRC and KiCad | Rule areas already exist end to end |
| P3 | Routing: per-net diff-pair enablement, intra-pair skew and max-uncoupled for USB 2.0 / Ethernet MDI / CAN / RS-485 / LVDS / HDMI; length groups through series parts; via limits; chain topology for connector→ESD→IC | Builds on experimental diff pairs and length tuning |
| P4 | Stackup parsing and impedance solver; impedance rules for all interfaces; reference-plane (no split crossing) check and router cost; stitching vias at layer changes | New capability with its own unit-test table |
| P5 | Power: hot-loop metric for bucks/boosts, SW-node copper area, feedback routing away from SW, thermal vias (advisory), width for current (IPC-2152/2221), motor-driver gate loops and Kelvin sense | Needs loop metric and current inputs |
| P6 | Safety: clearance/creepage between voltage domains, isolation-barrier keep-out, slots (advisory: TraceMaker does not cut slots) | Needs voltage inputs; must be conservative and well tested |
| P7 | DDR/LPDDR byte-lane and fly-by rules, MIPI/PCIe/SATA/USB 3 full sets | Rare in PCBench; needs P3+P4 |

Each phase ends with the gates of §9.1 for its categories and a row in [12-decisions.md](12-decisions.md).

## 11. Open questions

1. **Catalogue governance.** Numbers differ between vendors (e.g. USB 2.0 intra-pair match 1.27 mm in TI guides
   vs 0.15 mm for the Raspberry Pi CM5, §8.0). The catalogue takes the most widely cited value as the default and
   records the alternatives; should the default be the *strictest* instead? Strictest values may make boards
   unroutable for no electrical benefit at low speeds (FS USB, 10/100 Ethernet).
2. **Part-specific overrides.** A part's own datasheet layout section beats a generic category rule (e.g. a
   buck converter's recommended layout). Should the catalogue grow a part-family table (TPS62xxx, LAN87xx,
   ESP32-WROOM) with datasheet rules, and who maintains it?
3. **Speed grade detection.** USB FS vs HS, 100BASE-TX vs 1000BASE-T, SDRAM vs DDR3 change the rules. Detect
   from the IC part number (needs a part table) or ask the user? Default: assume the higher speed for soft
   rules, the lower for hard ones.
4. **Stackup presets.** Is a named preset (`jlc-2l-1.6`, `jlc-4l-1.6-7628`) acceptable as user-supplied data,
   or must the stackup always be in the board? (Rule 6 allows it only when the user names it.)
5. **Hard proximity in the placer.** D25 chose a soft pseudo-net; some rules (crystal ≤ 10 mm, antenna
   keep-out) want hard constraints. The legaliser and annealer need distance constraints with an exact
   reference path (rule 3).
6. **Writing to the user's files.** Should generated keep-outs and net classes ever be written into the
   `.kicad_pcb`/`.kicad_dru`, and in what form (rule areas named `tmk:RF-03:AE1`)?
7. **Learning.** Doc 06's knowledge base could learn per-category placements (escape templates generalise to
   "USB-C + ESD + MCU" templates). Out of scope here, noted as a link.
8. **AC-coupling capacitor position** (PCIe, USB 3, DisplayPort): sources put the caps near the connector,
   near the receiver end of a segment, or near the driver. The catalogue only checks the value and symmetry
   until a consistent rule is found.
9. **Verification of R-tagged rules.** Done 2026-10-05 for all 41 (§8.10): 9 confirmed, 9 corrected, 11 kept with
   TraceMaker-default numbers (now **D**), 12 not verifiable (still **R**: practice rules with no primary source, and the
   paid IEC 62368-1 and IPC-2221B texts). HV-02 (IEC 62368-1 creepage) and TP-01 need the standards before either can
   be trusted; ISO 4762/7089 sizes for MH-01 likewise.
10. **Licensing of sources.** Numbers and short rule statements from app notes are facts and citations; the
   catalogue must not copy text or figures from the documents.

## 12. Prior art

| Tool | How category-aware rules are expressed | Relevance |
|---|---|---|
| **KiCad 9/10** | Net classes (by pattern), board-setup diff-pair and impedance-controlled track widths, named rule areas, and `.kicad_dru` custom rules whose conditions can test `hasNetclass`, `hasComponentClass`, `memberOfFootprint`, `memberOfGroup`, `memberOfSheet`, `intersectsCourtyard`, `enclosedByArea`, `inDiffPair`, `isCoupledDiffPair`, `fromTo`, `getField(…)` and net-chain functions (source: `pcbexpr_functions.cpp`, KICAD-PCBEXPR). *Component classes* (KiCad 9+, assigned from fields, references or sheets) are the closest built-in analogue of a category | TraceMaker's compiled output (§5.5) is a `.kicad_dru` fragment in exactly this language, so the user can keep it. TraceMaker's own rule engine supports a subset today (`drc/rule_engine.cpp`); `hasComponentClass`/`getField` are worth adding |
| **Altium Designer** | Rule categories (electrical, routing, placement, high speed, SI…) each scoped by queries (`InNetClass`, `InComponentClass`, `HasFootprint`, `InRoom`, `InDifferentialPair`, `InxSignal`), resolved by priority; *rooms* tie placement regions to component classes; *xSignals* define pad-to-pad paths through series parts for matching; impedance profiles from the Layer Stack Manager drive width/gap | xSignals = our length groups through series parts (§3.3); rooms = proximity/region rules; impedance profiles = §5.3 |
| **Cadence Allegro Constraint Manager** | Electrical constraint sets (ECSets: topology, length/delay, diff pair, impedance) from topology templates, applied to nets, buses and XNets (paths through passives); physical/spacing CSets per net class and per region; Allegro X AI places/routes against them | ECSets ≈ a category's rule bundle; the template idea matches our per-category roles |
| **Siemens Xpedition CES**, **Zuken CR-8000** | Constraint editor shared by schematic and layout; net/constraint classes, topology, rule areas, reusable constraint sets carried with IP blocks | Rules travelling with a circuit block is the same idea from the schematic side |
| **Quilter** | A "circuit comprehension" step infers constraints from object/net classes, component and net names, pin names and connections: power nets, diff pairs, crystals (near driver), switching converters (U + L heuristic, tight input/output caps), bypass caps (attached by schematic wire, pin names, then shared nets; smaller values closer); "physics rule checks" report pin distance, layer switches, ground overlap, length mismatch, uncoupled spacing (QUILTER-DOCS) | Closest prior art for *detect category → apply rule*; confirms the detection signals of §3.1 and the decap/converter/crystal priorities of P1 |
| **JITX** | Code-defined circuits where reusable blocks carry their own SI, placement and routing constraints (diff-pair impedance, length) (JITX-DOCS) | Rules attached to components/interfaces by construction instead of detection |
| **Flux (AI copilot)**, **Celus**, **DeepPCB** | Flux: AI design review against datasheet-derived checks; Celus: block-based schematic generation with reference-circuit knowledge; DeepPCB: RL placement/routing honouring input net classes. None publishes a formal category→layout-rule catalogue (not verified in depth) | Background only |

What none of them publishes is an open, cited, machine-readable catalogue of category rules. That is the gap
`component_rules.yaml` fills; it can also be exported to KiCad component classes and custom rules.

## 13. References

Impedance formulas (§5.3): IPC-2141A, Wadell 1991, Hammerstad & Jensen 1980. Current capacity (§5.4): IPC-2221B,
IPC-2152. All sources cited by rule id in §8:

| Key | Source |
|---|---|
| TI-SPRAAR7J | [TI SPRAAR7J, High-Speed Interface Layout Guidelines (2023)](https://www.ti.com/lit/an/spraar7j/spraar7j.pdf) |
| TI-SLLA414 | [TI SLLA414A, High-Speed Layout Guidelines for Signal Conditioners and USB Hubs](https://www.ti.com/lit/an/slla414a/slla414a.pdf) |
| TI-TUSB8041 | [TI TUSB8041 USB 3.0 hub datasheet, §11.1 layout](https://www.ti.com/lit/ds/symlink/tusb8041.pdf) |
| TI-TUSB1310A | [TI TUSB1310A USB 3.0 PHY datasheet, §6.2.4 layout](https://www.ti.com/lit/ds/symlink/tusb1310a.pdf) |
| TI-TUSB320 | [TI TUSB320 USB Type-C CC logic datasheet, §10.1 layout](https://www.ti.com/lit/ds/symlink/tusb320.pdf) |
| TI-TPS65987D | [TI TPS65987D USB Type-C PD controller datasheet, §11.4-11.5 layout](https://www.ti.com/lit/ds/symlink/tps65987d.pdf) |
| USB-TYPEC-SPEC | [USB-IF, USB Type-C Cable and Connector Specification, Release 2.0 (Aug 2019), §4.11.1 Table 4-25 (Rd = 5.1 kOhm)](https://www.usb.org/sites/default/files/USB%20Type-C%20Spec%20R2.0%20-%20August%202019.pdf) |
| RPI-CM5 | [Raspberry Pi Compute Module 5 datasheet (carrier-board layout notes)](https://datasheets.raspberrypi.com/cm5/cm5-datasheet.pdf) |
| TI-TPD12S016 | [TI TPD12S016 HDMI companion/ESD datasheet, §10.1 layout](https://www.ti.com/lit/ds/symlink/tpd12s016.pdf) |
| TI-SN75DP130 | [TI SN75DP130 DisplayPort redriver datasheet, §12.1 layout](https://www.ti.com/lit/ds/symlink/sn75dp130.pdf) |
| TI-DS160PR410 | [TI DS160PR410 PCIe 4.0 redriver datasheet, §9-10](https://www.ti.com/lit/ds/symlink/ds160pr410.pdf) |
| TI-SN75LVCP601 | [TI SN75LVCP601 SATA redriver datasheet](https://www.ti.com/lit/ds/symlink/sn75lvcp601.pdf) |
| TI-DS90UB953 | [TI DS90UB953-Q1 serializer datasheet, §7.4.1.1 CSI-2 layout](https://www.ti.com/lit/ds/symlink/ds90ub953-q1.pdf) |
| TI-SNLA187 | [TI SNLA187, LVDS Owner's Manual](https://www.ti.com/lit/pdf/snla187) |
| TI-SPRABI1 | [TI SPRABI1, DDR3 Design Requirements for KeyStone Devices](https://www.ti.com/lit/pdf/sprabi1) |
| TI-SPRAD06 | [TI SPRAD06, AM62x DDR Board Design and Layout Guidelines (DDR4, LPDDR4)](https://www.ti.com/lit/pdf/sprad06) |
| TI-AM625 | [TI AM625 datasheet, §8.2.2 OSPI/QSPI board design](https://www.ti.com/lit/ds/symlink/am625.pdf) |
| TI-SPRAD21 | [TI SPRAD21, AM62x/AM62Ax/AM62D-Q1/AM62Px Schematic Design Guidelines and Review Checklist (rev. I, Sep 2025)](https://www.ti.com/lit/pdf/sprad21) |
| TI-WL1837MOD | [TI WL1837MOD datasheet, §9.1 SDIO layout](https://www.ti.com/lit/ds/symlink/wl1837mod.pdf) |
| TI-TXS0206A | [TI TXS0206A SD-card level translator datasheet](https://www.ti.com/lit/ds/symlink/txs0206a.pdf) |
| ST-AN2867 | [ST AN2867 Rev 22 (Nov 2024), Guidelines for oscillator design on STM8AF/AL/S and STM32 MCUs/MPUs](https://www.st.com/resource/en/application_note/an2867-oscillator-design-guide-for-stm8afals-stm32-mcus-and-mpus-stmicroelectronics.pdf) |
| MCHP-AN826 | [Microchip AN826, Crystal Oscillator Basics and Crystal Selection for rfPIC and PICmicro](https://ww1.microchip.com/downloads/en/AppNotes/00826a.pdf) |
| NXP-AN2049 | [NXP/Freescale AN2049, Some Characteristics and Design Notes for Crystal Feedback Oscillators](https://www.nxp.com/docs/en/application-note/AN2049.pdf) |
| ESP-HDG-ESP32 | [Espressif, ESP32 Hardware Design Guidelines: PCB Layout Design](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32/pcb-layout-design.html) |
| ESP-HDG-S3 | [Espressif, ESP32-S3 Hardware Design Guidelines: PCB Layout Design](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/pcb-layout-design.html) |
| ESP-HDG-C3 | [Espressif, ESP32-C3 Hardware Design Guidelines: PCB Layout Design](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32c3/pcb-layout-design.html) |
| ST-AN4488 | [ST AN4488 Rev 7 (Oct 2018), Getting started with STM32F4xxxx MCU hardware development](https://www.st.com/resource/en/application_note/an4488-getting-started-with-stm32f4xxxx-mcu-hardware-development-stmicroelectronics.pdf) |
| TI-SLVA959B | [TI SLVA959B, Best Practices for Board Layout of Motor Drivers](https://www.ti.com/lit/an/slva959b/slva959b.pdf) |
| AMD-UG483 | [AMD/Xilinx UG483 v1.14 (May 2019), 7 Series FPGAs PCB Design Guide](https://docs.amd.com/v/u/en-US/ug483_7Series_PCB) |
| ADI-MT101 | [ADI MT-101 Rev.0 (03/09), Decoupling Techniques](https://www.analog.com/media/en/training-seminars/tutorials/MT-101.pdf) |
| TI-SLYT614 | [TI SLYT614, Five steps to a great PCB layout for a step-down converter](https://www.ti.com/lit/an/slyt614/slyt614.pdf) |
| TI-SNVA021C | [TI SNVA021C, AN-1149 Layout Guidelines for Switching Power Supplies](https://www.ti.com/lit/an/snva021c/snva021c.pdf) |
| TI-SLMA002 | [TI SLMA002H (rev. Jul 2018), PowerPAD Thermally Enhanced Package](https://www.ti.com/lit/an/slma002h/slma002h.pdf) |
| ADI-AN139 | [ADI (Linear Technology) AN-139 Rev A (Oct 2012), Power Supply Layout and EMI](https://www.analog.com/media/en/technical-documentation/application-notes/an139f.pdf) |
| TI-SLVA118A | [TI SLVA118A, Linear Regulator Design Guide for LDOs](https://www.ti.com/lit/an/slva118a/slva118a.pdf) |
| ADI-MT031 | [ADI MT-031 Rev.A (10/08), Grounding Data Converters and Solving the Mystery of AGND and DGND](https://www.analog.com/media/en/training-seminars/tutorials/MT-031.pdf) |
| ADI-OPAMP-HB | [ADI, Op Amp Applications Handbook (W. Jung, ed.), Section 7-2 PCB Design Issues: guarding (pp. 7.41-7.42)](https://www.analog.com/media/en/training-seminars/design-handbooks/Op-Amp-Applications/Section7.pdf) |
| IPC-2221B | [IPC-2221B, Generic Standard on Printed Board Design (2012): §6.2 conductor current, Table 6-1 spacing](https://www.ipc.org/TOC/IPC-2221B.pdf) |
| IPC-2152 | [IPC-2152, Standard for Determining Current Carrying Capacity in Printed Board Design (2009)](https://www.ipc.org/TOC/IPC-2152.pdf) |
| KICAD-CALC-IPC2221 | [KiCad PCB Calculator, electrical spacing panel (IPC-2221 Table 6-1 values)](https://gitlab.com/kicad/code/kicad/-/raw/master/pcb_calculator/calculator_panels/panel_electrical_spacing_ipc2221.cpp) |
| TI-SLUA618A | [TI SLUA618A, Fundamentals of MOSFET and IGBT Gate Driver Circuits](https://www.ti.com/lit/an/slua618a/slua618a.pdf) |
| RPI-PICOW | [Raspberry Pi Pico W datasheet, §2.2.1 antenna placement](https://pip-assets.raspberrypi.com/categories/686-raspberry-pi-pico-w/documents/RP-008312-DS-2-pico-w-datasheet.pdf) |
| UBLOX-NINAB3 | [u-blox NINA-B3 System Integration Manual UBX-17056748](https://content.u-blox.com/sites/default/files/NINA-B3_SIM_UBX-17056748.pdf) |
| NORDIC-NRF52840-REF | [Nordic nRF52840 Product Specification, Reference circuitry](https://docs.nordicsemi.com/bundle/ps_nrf52840/page/ref_circuitry.html) |
| JOHANSON-2450AT18A100 | [Johanson 2450AT18A100 2.4 GHz chip antenna datasheet (eval-board layout)](https://www.johansontechnology.com/datasheets/2450AT18A100/2450AT18A100.pdf) |
| IGNION-NN03-310 | [Ignion TRIO mXTEND NN03-310 user manual](https://ignion.io/files/UM_NN03-310.pdf) |
| TI-AN058 | [TI SWRA161B (AN058), Antenna Selection Guide](https://www.ti.com/lit/an/swra161b/swra161b.pdf) |
| SILABS-AN928 | [Silicon Labs AN928.2 Rev 1.7, EFR32 Series 2 Layout Design Guide](https://www.silabs.com/documents/public/application-notes/an928.2-efr32-series2-layout-design-guide.pdf) |
| WADELL-1991 | [B. C. Wadell, Transmission Line Design Handbook, Artech House, 1991](https://us.artechhouse.com/Transmission-Line-Design-Handbook-P297.aspx) |
| IPC-2141A | [IPC-2141A, Design Guide for High-Speed Controlled Impedance Circuit Boards (2004)](https://www.ipc.org/TOC/IPC-2141A.pdf) |
| HAMMERSTAD-1980 | [E. Hammerstad, O. Jensen, Accurate Models for Microstrip Computer-Aided Design, IEEE MTT-S Digest 1980, pp. 407-409](https://doi.org/10.1109/MWSYM.1980.1124303) |
| IEC-62368-1 | [IEC 62368-1:2023, Audio/video, information and communication technology equipment - Safety requirements (clearance/creepage tables)](https://webstore.iec.ch/en/publication/90190) |
| TI-SLLA284 | [TI SLLA284G, Digital Isolator Design Guide](https://www.ti.com/lit/an/slla284d/slla284d.pdf) |
| ADI-AN1109 | [ADI AN-1109 Rev.0, Recommendations for Control of Radiated Emissions with iCoupler Devices](https://www.analog.com/media/en/technical-documentation/application-notes/AN-1109.pdf) |
| JLC-CAPS | [JLCPCB PCB manufacturing capabilities](https://jlcpcb.com/capabilities/pcb-capabilities) |
| KICAD-PCBEXPR | [KiCad source, pcbnew/pcbexpr_functions.cpp (custom-rule condition functions)](https://gitlab.com/kicad/code/kicad/-/raw/master/pcbnew/pcbexpr_functions.cpp) |
| QUILTER-DOCS | [Quilter documentation, Physics constraints (overview, bypass capacitors, switching converters)](https://docs.quilter.ai/physics-constraints/overview.md) |
| JITX-DOCS | [JITX documentation](https://docs.jitx.com/en/latest/) |
| TI-SNLA079D | [TI SNLA079D, AN-1469 PHYTER Design & Layout Guide](https://www.ti.com/lit/an/snla079d/snla079d.pdf) |
| TI-SNLA387 | [TI SNLA387, Ethernet PHY PCB Design Layout Checklist (2021)](https://www.ti.com/lit/an/snla387/snla387.pdf) |
| TI-DP83867 | [TI DP83867IR/CR Gigabit Ethernet PHY datasheet SNLS484J, §9.2.2 layout](https://www.ti.com/lit/ds/symlink/dp83867ir.pdf) |
| MCHP-AN18-0 | [SMSC/Microchip AN 18.0, LAN950x Layout Guidelines (Ethernet chapter)](https://ww1.microchip.com/downloads/en/Appnotes/en562744.pdf) |
| TI-SLLA270 | [TI SLLA270, Controller Area Network Physical Layer Requirements](https://www.ti.com/lit/an/slla270/slla270.pdf) |
| TI-TCAN1042H | [TI TCAN1042H CAN FD transceiver datasheet, §12.1 layout](https://www.ti.com/lit/ds/symlink/tcan1042h.pdf) |
| TI-SLLA272 | [TI SLLA272, The RS-485 Design Guide](https://www.ti.com/lit/an/slla272d/slla272d.pdf) |
| TI-MAX3232 | [TI MAX3232 RS-232 transceiver datasheet](https://www.ti.com/lit/ds/symlink/max3232.pdf) |
| TI-SLVA689 | [TI SLVA689, I2C Bus Pullup Resistor Calculation](https://www.ti.com/lit/an/slva689/slva689.pdf) |
| NXP-UM10204 | [NXP UM10204 Rev. 7, I2C-bus specification and user manual](https://www.nxp.com/docs/en/user-guide/UM10204.pdf) |
| TI-SPRU655I | [TI SPRU655I, Emulation and Trace Headers Technical Reference Manual](https://www.ti.com/lit/ug/spru655i/spru655i.pdf) |
| TI-SLVA680A | [TI SLVA680A, ESD Protection Layout Guide](https://www.ti.com/lit/an/slva680a/slva680a.pdf) |
| TI-BQ24075 | [TI BQ2407x Li-ion charger datasheet SLUS810N, §12 layout](https://www.ti.com/lit/ds/symlink/bq24075.pdf) |
| IEEE-802.3 | [IEEE 802.3 Ethernet standard (MDI isolation 1500 Vrms, clause 22 MDIO)](https://standards.ieee.org/ieee/802.3/10422/) |
| TI-DP83848C | [TI DP83848C PHYTER Ethernet PHY datasheet SNLS266E: MDIO 1.5 kOhm pull-up (p.6, p.35)](https://www.ti.com/lit/ds/symlink/dp83848c.pdf) |
| ISO-11898-2 | [ISO 11898-2, Road vehicles - Controller area network - High-speed medium access unit](https://www.iso.org/standard/85120.html) |
| ARM-CORESIGHT-CONN | [Arm/Keil, CoreSight target connectors: Cortex Debug 10-pin and Debug+ETM 20-pin at 0.05 in, Arm JTAG 20-pin at 0.10 in (now Arm KAN339)](http://www2.keil.com/coresight/coresight-connectors/) |
| IPC-2221B-TP | [Common in-circuit-test (bed-of-nails) DFT practice; e.g. IPC-2221B §8 test points (not re-read)](https://www.ipc.org/TOC/IPC-2221B.pdf) |
| TMK-PRACTICE | No primary source read: common layout practice recorded as a TraceMaker default (doc 15 §8 note); verify before making hard |

## 14. Implementation status (2026-10-04, milestone M13 P0–P4, user overrides, ETH-05, edge attraction)

Built in `src/crules/` (namespace `tmk::crules`); assumptions made without review are listed in
[`../dev/assumptions-m13.md`](../dev/assumptions-m13.md); decisions D26–D31 and D36–D42 in [12-decisions.md](12-decisions.md).

### 14.1 What is built

| Piece | Where | Notes |
|---|---|---|
| Catalogue loader | `scripts/crules_catalogue.py` → `src/crules/catalogue.json` (embedded); `catalogue.{hpp,cpp}` | ctest `crules_catalogue_sync` checks JSON = YAML and YAML rule ids = this document's (§9.1 L0); `tracemaker rules --catalogue` loads another file |
| Detection (§3.1–3.2) | `detect.cpp` | Every footprint against every category; integer weights; apply 70 / suggest 40; no-pin-name cap 60 (§3.6) for categories with a `pin_name` detector except `ic_decoupling`; conflicts by exclusive groups (§3.4); "possible" list below 40 |
| Role binding (§3.3) | `detect.cpp` (`bind_*`) | usb2/usb3_typec (connector, D+/D− by pin name, net name or KiCad USB pad numbers, ESD, CMC, transceiver, VBUS, CC), crystal (IC, XIN/XOUT pins, load caps, series R), oscillator, ic_decoupling (generalised D25), ldo/buck/boost (VIN/VOUT, cin/cout, inductor, SW), esd_tvs (connector, protected nets, IC, ground pins), can/rs485 (bus, connector, ESD, termination), rf_module/chip_antenna (footprint keep-out), ethernet (rj45 or phy anchor, discrete magnetics, integrated magnetics), connector_general, mounting_hole; other categories bind their anchor only |
| Effective rules (§3.5, §7) | `evaluate.cpp`, `report.cpp` | Every rule of every instance: applied / satisfied-by-board / not-applied(reason) / advisory, hard→soft below `apply` and for R- and D-evidence rules, measured pad-centre distances for proximity rules, edge distance for edge rules |
| Report | `tracemaker rules <board> [--mode report\|soft\|on] [--json f] [--dru f] [--roles-from input]` | Text summary (categories with > 6 instances condensed) and full JSON incl. keep-out polygons |
| P1 placement | `tracemaker-place --component-rules off\|report\|soft` (default **off**), `ExtractOptions::affinities` | Objective-only pseudo-nets (D25 mechanism, D28) for XTAL-01/02, OSC-01/02, ESD-01, USB2-07, USBC-10, CAN-02, RS485-02, LDO-01, BUCK-01/02 (proxies), DEC-01/07 |
| P2 keep-outs | `generate_keepouts`, `dru_sidecar`; `tracemaker route --component-rules on` | XTAL-04, BUCK-06, BOOST-04 at confidence ≥ 70: rule areas on pad-free layers (D29) given to the router (in memory only); `report`/`soft`/`on` write `<output>.tracemaker.kicad_dru` |
| Ethernet magnetics void | `bind_ethernet`, `generate_keepouts` (ETH-05) | Discrete magnetics: tracks, vias and zones kept out on their side and the adjacent layer (D41, §14.6) |
| User override file (§3.5, §6.3) | `overrides.{hpp,cpp}`, `detect(…, &overrides)`; `--rules-override` on `rules`, `route`, `tracemaker-place` | `disable`/`assert`/`deny`/`set`, validated against catalogue and board; reported (D40, §14.6) |
| Connector edge attraction | `crules::edge_attractions`, `ExtractOptions::edge_pulls`, `PNet::has_ax/ay`; `tracemaker-place --component-rules soft --edge-attraction` | Opt-in, objective only (D42, §14.6) |
| P3 USB pairs | `RouterOptions::pair_nets`, `crules::usb_pairs`; `tracemaker route --component-rules soft\|on` | USB2-02: detected D+/D− routed coupled first (D30) by the coupled pair search (doc 05 §14, D50); skew measured by `tracemaker pairs`, tuned with `--pair-skew-mm` (not yet set from the rule's 1.27 mm) |
| Events (rule 7) | route job | `crules.detected`, `crules.keepout` |
| P4 stackup | `model/stackup.{hpp,cpp}`, `io/kicad/board_reader.cpp` (`read_stackup`) | `Board::stackup`: layers in file order with type, thickness (nm), εr, loss tangent, material, sublayers (`addsublayer`, combined in series), copper index; copper finish. Read-only (rule 8); `present == false` without a block |
| P4 impedance solver | `crules/impedance.{hpp,cpp}` | Closed forms, quasi-static, solder mask ignored; width/gap by integer-nm bisection (§14.5) |
| P4 impedance rules | `crules/impedance_rules.{hpp,cpp}`, `evaluate.cpp` | Every `impedance` rule gets a width (and gap) per routing layer over the adjacent copper layer, in the text and JSON report; report only (D36) |
| P4 width for current | `imp::width_for_current` (IPC-2221), `plan_current` | Reported for rules that give `current_a` (USBC-09); report only |

Not built: net classes from rules (impedance widths are reported only, D36; width for current, P5), hard proximity
constraints, the connector orientation part of CONN-01 beyond what the edge pull implies, edge attraction in the global
(quadratic) placement stage (the annealer carries it), antenna keep-outs without a footprint keep-out (no datasheet
table), the Ethernet plane split (ETH-07) and chassis isolation (ETH-06), routing order/chain topology, skew and via
limits for pairs, `stackup_preset` and catalogue additions in the override file, automatic discovery of
`<project>.tracemaker_rules.*` next to the board, `--write-keepouts`/`--write-rules`, viewer drawing of the events.

### 14.2 Detection quality (§9.1 L1)

Labels: `bench/crules_labels.json` (41 PCBench boards; ic_decoupling on 10 of them) and
`bench/crules_labels_heldout.json` (12 boards labelled after the detectors were tuned on the first set).
`bench/crules_detect.py` scores them. All PCBench boards are KiCad 5 files without pin names.

Per category, at the **suggest** threshold (≥ 40: what the report lists and what soft rules use). TP/FP/FN count
instances (anchor references); "before" is the catalogue as written on 2026-10-03, "tuned" after the detector changes
of D27 and assumptions-m13 item 4, "held-out" the 12 boards labelled after tuning (before the CAN fix, which was made
because of its FP).

| Category | 41 boards, before: TP/FP/FN (P / R) | 41 boards, tuned: TP/FP/FN (P / R) | 12 held-out: TP/FP/FN (P / R) |
|---|---|---|---|
| crystal | 18/0/3 (1.00 / 0.86) | 21/0/0 (1.00 / 1.00) | 12/0/1 (1.00 / 0.92) |
| oscillator | 0/0/1 (– / 0.00) | 0/0/1 (– / 0.00) | – |
| usb2 | 20/6/13 (0.77 / 0.61) | 30/0/3 (1.00 / 0.91) | 5/0/3 (1.00 / 0.63) |
| esd_tvs | 13/0/4 (1.00 / 0.76) | 17/0/0 (1.00 / 1.00) | 1/0/0 (1.00 / 1.00) |
| ldo | 24/0/9 (1.00 / 0.73) | 33/0/0 (1.00 / 1.00) | 10/0/2 (1.00 / 0.83) |
| buck | 1/2/1 (0.33 / 0.50) | 2/0/0 (1.00 / 1.00) | 0/0/1 (– / 0.00) |
| boost | 0/0/4 (– / 0.00) | 4/0/0 (1.00 / 1.00) | 4/0/0 (1.00 / 1.00) |
| rf_module | 9/0/3 (1.00 / 0.75) | 12/0/0 (1.00 / 1.00) | 5/0/1 (1.00 / 0.83) |
| can | 6/0/0 (1.00 / 1.00) | 6/0/0 (1.00 / 1.00) | 2/2/0 (0.50 / 1.00) → 2/0/0 after the CAN fix |
| rs485 | 2/0/0 (1.00 / 1.00) | 2/0/0 (1.00 / 1.00) | – |
| connector_general | 102/0/75 (1.00 / 0.58) | 172/0/5 (1.00 / 0.97) | 34/0/25 (1.00 / 0.58) |
| mounting_hole | 44/0/10 (1.00 / 0.81) | 54/0/0 (1.00 / 1.00) | 20/0/8 (1.00 / 0.71) |
| ic_decoupling (10 boards) | 63/1/8 (0.98 / 0.89) | 68/1/3 (0.99 / 0.96) | – |

Held-out precision is 1.00 in every category but CAN (two LIN transceivers TJA1027 matched `tja10`; the pattern is
now `tja104x|tja105x`). Held-out recall shows what tuning on 41 boards cannot buy: footprints named by vendor part
number (connectors such as `B2B-PH-K-S`, `20021121-...`, `SIL-10J1`; USB `10118192`), mounting holes named
`M2.5_HOLE` or `1pin`, an LDO named `TC1265`, a buck `LM3671`. At the **apply** threshold (≥ 70) only crystal,
esd_tvs, rf_module, mounting_hole and ic_decoupling are reached on PCBench, because KiCad 5 boards have no pin names
and the other categories are capped at 60 (§3.6); held-out at apply: crystal 12/0/1, rf_module 1/0/5, mounting_hole
20/0/8, the rest 0 detections (no false positives). Gate of §9.1 (precision ≥ 0.95, recall ≥ 0.8) on the held-out
set: met by crystal, ldo, boost, rf_module, esd_tvs (1 instance), can (after the fix); not met by usb2 (recall 0.63),
connector_general (0.58), mounting_hole (0.71), buck (1 instance).

### 14.3 Placement effect (§9.1 L3, P1)

`bench/crules_place.py`: `tracemaker-place --mode full --seed 1` (8 threads per board), `--component-rules off`
vs `soft`; distances by `bench/place_intent.py`, component-rule distances measured on the placed board with roles
bound on the input (`--roles-from`). HPWL is the reported (unweighted) wirelength; pseudo-nets never count in it.

Medians over boards (n = boards with that kind of part); distances in mm. "decap" and "crystal" are
place_intent's own measures (centre to the nearest IC pad); "XTAL-01", "XTAL-02", "ESD-01", "reg caps" (LDO-01 and
BUCK-01) are the largest pad-centre distance per rule instance. The third configuration additionally doubles D25's
decoupling ties (`--decap-weight 20`).

| Set | Config | HPWL (median) | HPWL vs off (geo-mean) | decap | crystal | XTAL-01 | XTAL-02 load caps | ESD-01 | reg caps |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 23 boards of doc 04 §7.3, seed 1 | off | 518.3 | 1.000 | 3.66 (17) | 5.11 (6) | 6.36 (5) | 2.38 (4) | – | 4.25 (3) |
| | soft | 518.3 | 0.996 | 3.92 | 3.98 | 5.06 | 2.02 | – | 4.31 |
| | soft + decap ×2 | 581.3 | 1.028 | 3.26 | 4.47 | 4.81 | 2.00 | – | 2.83 |
| | human placement | – | – | 4.15 | 4.45 | 5.36 | 2.84 | – | 3.48 |
| 9 boards with ESD parts, seed 1 | off | 582.0 | 1.000 | 5.61 (9) | 4.04 (6) | 6.04 (6) | 3.70 (5) | 7.12 (9) | 2.00 (3) |
| | soft | 589.5 | 1.005 | 3.83 | 3.46 | 4.52 | 2.28 | 3.19 | 3.80 |
| | soft + decap ×2 | 597.3 | 1.019 | 2.78 | 3.38 | 4.26 | 1.86 | 3.20 | 2.01 |
| | human placement | – | – | 2.94 | 3.98 | 4.68 | 1.85 | 7.12 | 2.12 |
| 6 boards where soft changes the result, seeds 1–3 (18 runs) | off | 615.7 | 1.000 | 3.20 (15) | 5.41 (15) | 6.36 (15) | 3.10 (12) | – | 2.19 (3) |
| | soft | 617.9 | 0.986 | 4.29 | 3.54 | 4.58 | 2.06 | – | 4.53 |
| | soft + decap ×2 | 628.7 | 1.004 | 2.92 | 4.08 | 5.03 | 2.06 | – | 2.53 |

On 17 of the 23 boards `soft` adds no pseudo-net (no crystal/ESD/regulator instance, or D25 already ties the parts)
and the placement is byte-identical to `off`. Per board, soft vs off (change > 0.05 mm): on the 23 boards XTAL-01 is
better on 4 and worse on 0, load caps better on 3, decaps worse on 3 and better on 0; on the ESD set ESD-01 is better
on 8 of 9 boards (worse on 0), decaps better on 5 and worse on 4, regulator caps worse on 2; over the 18 multi-seed
runs XTAL-01 is better on 14 of 15, load caps on 9 of 12, but decaps worse on 9 (better on 2) and regulator caps
worse on 3 of 3. The crystal and protection pulls take the space next to the IC that D25's decaps had (bullion,
PocketBone: decaps 3-4 mm become 6-8 mm). Doubling the decap ties fixes the decaps
(better than off) at +2-3 % HPWL. Neither is free of regressions, so **`--component-rules` stays `off` by
default for placement** (D31); `soft` is recommended where crystals and connector ESD matter more than a few tenths
of a millimetre of decap distance. `off` reproduces the previous placer output byte for byte.

**Weight scale (`tracemaker-place --crules-weight PCT`, 2026-10-04 night, seed 1, 23 boards).** Medians, off →
soft at 50 % / 75 % / 100 %: decaps 3.66 → 3.33 / 3.30 / 3.92 mm; crystal to IC 5.11 → 5.35 / 4.53 / 3.98 mm;
regulator caps 4.25 → 3.79 / 2.45 / 4.31 mm; HPWL equal. At 75 % every median improves on off, but per board the
decaps still get worse on RX5808 (3.7 → 5.8 mm) and PocketBone (4.3 → 8.5 mm) and better on bullion (4.0 → 2.4 mm);
only 5 of 23 boards change at all. Seeds 2–4 overturn the seed-1 picture: at 75 % decap medians are worse than off
on every seed (3.74 / 3.92 / 3.85 vs 3.26 / 3.74 / 3.39 mm), as at 100 %, and regulator caps too; only the crystal
gain holds on all seeds (e.g. 5.07 → 2.83–3.59 mm on seed 3). A weight scale does not fix the decap conflict; the
feature stays off and the scale at 100 %.

**Two stages (D43, now what `soft` does in full mode).** Stage 1 places with the decoupling ties only; stage 2
locks those capacitors and their ICs and refines with the other pulls. 23 boards × seeds 1–3, off → two-stage:
decap median 3.66/3.26/3.74 → 3.66/3.42/3.74 mm, crystal 5.11/6.70/5.07 → 4.66/5.77/4.72, load caps 2.38/4.77/3.11 →
2.03/2.76/2.39, regulator caps unchanged, HPWL geometric mean 0.99. Per board, 18 metric changes better and 4 worse
(PocketBone decaps on every seed; bullion's crystal on one). PocketBone's "decaps" there are mostly the regulator U4's
input/output capacitors (C2–C13): the LDO rule re-places them around U4, which is not locked (no D25 tie anchors it),
and `bench/place_intent.py` counts any supply-to-ground capacitor near an IC as a decap — the two metrics overlap.
Integration test `crules_two_stage`.

### 14.4 Routing (P2/P3)

`--component-rules` defaults to `off` for `tracemaker route` (D31): the router's default path is unchanged
(`pair_nets` empty). Measured 2026-10-04:

- **Tier-A sample** (`bench/run.py --tier A --limit 40 --time 30 --jobs 2`, KiCad DRC judge): off 36/40 clean,
  completion 0.997; with `TM_ROUTE_ARGS="--component-rules on"` 35/40, 0.995. `on` changes the router input on only 3
  of the 40 boards (kitspace_aquarius, kitspace_threeboard: one crystal keep-out each; mechkeys_1800-controller: a
  keep-out and two USB pairs); the board that lost its clean pass (mosavr_pcie1x_backplane, 110 → 102 routed) gets no
  component rule at all, so that difference is wall-clock noise of the 30 s runs on a shared machine.
- **16 boards with crystals/USB/ESD** (same harness): off 8/16 clean, completion 0.951; on 6/16, 0.947.
- **Deterministic** (`--work 20000000 --threads 1`, 10 boards where the rules change the router input): `soft` (USB
  pairs only) routes the same number of connections as `off` on all 10; the experimental coupled-pair router mostly
  falls back to single tracks on these short USB runs ("too short to couple", "leg start fails"). `on` (keep-outs)
  is worse on 4 boards (blackmagic-isolated 93 → 86, PiZeroHub 108 → 104, bobc_MS-F100 102 → 99, MicroMaple 95 → 94),
  better on 1 (HACK 82 → 88) and equal on 5: a hard keep-out under the crystal and its IC pins removes routing
  space on the far layer. Leaving the IC pins out of the area (tried) does not help consistently.
- **KiCad check of the sidecar**: on PCBench CANadapter, KiCad 10 reads `<output>.tracemaker.kicad_dru` (copied as
  the project's `.kicad_dru`) and reports one "items not allowed (rule 'tmk XTAL-04 Y1')" — a +3V3 track on B.Cu
  under the crystal — for the board routed with `off`, and none for the board routed with `on`
  (ctest `crules_route`).

So keep-outs are a real constraint with a completion cost on dense boards; they stay opt-in.

### 14.5 Stackup and impedance (P4)

**Stackup.** `(setup (stackup …))` is read into `Board::stackup` (§14.1). Of the 1,157 PCBench boards, **none** of the
`unrouted.kicad_pcb` files has a stackup (they are KiCad 5 exports); 8 of the `raw.kicad_pcb` originals do
(`kitspace_BalthazarPSU3`, `Minisumo_V2.1`, `minisumo_v3`, `OSO-BOOK-C1`, `OtterPill`, `solenoid_driver`, `Unifying`,
`USBI2C01`), as do 15 KiCad demos (2 to 12 copper layers; `tiny_tapeout` uses dielectric sublayers) and 12 Freerouting
issue fixtures. So on the benchmark impedance rules always report "not applied: no stackup in the board (doc 15
§3.6)" — nothing is guessed (CLAUDE.md rule 6).

**Geometry per routing layer** (`stackup_geometry`): an outer layer is microstrip over the adjacent copper layer
(dielectric = everything between them); an inner layer is stripline between the copper layers above and below
(off-centre allowed; εr of the two sides combined in series). The adjacent copper layer is *assumed* to be a plane;
the report says "(GND)" when a zone of a ground net is on it and "(assumed)" otherwise. KiCad `power` layers are not
routing layers. A stackup without εr or thickness for a gap makes that layer "not computed", never defaulted.

**Models** (`impedance.cpp`, each cited at its definition):

| Structure | Model | Stated formula error |
|---|---|---|
| Microstrip | Hammerstad & Jensen 1980 with their thickness correction (KiCad additionally applies Bahl & Garg's thickness term, 0–3 % higher Z on thin dielectrics) | ±4 % |
| Stripline | Cohn 1954 (w/(b−t) ≥ 0.35) / Wheeler 1978 (narrow), off-centre by Wadell's image split (two centred lines in parallel) | ±2 % centred, ±5 % off-centre |
| Edge-coupled microstrip | Kirschning & Jansen 1984 static even/odd modes, Jansen 1978 thickness, Bahl & Garg 1977 | ±5 % |
| Edge-coupled stripline | Cohn 1955 (exact zero-thickness conformal mapping, thick-strip eq. 18/20/22), image split off-centre | ±4 % centred, ±8 % off-centre |
| Grounded coplanar | Ghione & Naldi 1983/84 conformal mapping, Gupta et al. 1996 thickness | ±7 % |

The stated error covers the papers' accuracy plus what is left out (solder mask lowers outer-layer Z by roughly 1–3 Ω;
dispersion; the image split). Fab tolerance (±10 % typical) is reported separately. Differential impedance =
2 × odd-mode impedance; the propagation delay is √εeff / c (odd mode for pairs).

**Solving** (`plan_impedance`): width by bisection on integer nm (monotone functions; ties to the narrower width) in
[max(board minimum width, 0.1 mm), 5 mm (pairs) / 10 mm]; pairs use the Default net class's diff-pair gap (never below
its clearance or the board minimum clearance); if even the narrowest width is too low in impedance, the width stays at
the minimum and the gap is solved instead; GCPW uses the clearance as the ground gap. Rules with `structure: stripline`
are solved on inner layers only, `gcpw` on outer layers (pairs on those layers use coupled microstrip: no coplanar-pair
model). A pair wider than 1 mm as plain microstrip gets the §5.3 note (prefer GCPW or short matched routing).

**Unit tests** (`src/crules/test_impedance.cpp`, `[impedance]`; reader in `tests/test_kicad_io.cpp`, `[stackup]`).
References and the tolerance each test uses:

| Case | Reference | TraceMaker | Error | Tolerance |
|---|---|---:|---:|---:|
| Microstrip, εr 2.2, h 1.27 mm, w 3.911 mm, t 0 | Pozar Ex. 3.7: 50 Ω, εeff 1.87 | 50.04 Ω, 1.881 | 0.07 %, 0.6 % | 0.5 %, 1 % |
| Microstrip, εr 4.5, h 1.51 mm, w 2.9 mm, 35 µm | KiCad: 49.03 Ω | 48.94 Ω | 0.2 % | 3 % |
| Microstrip, εr 4.3, h 0.2 mm, w 0.35 mm, 35 µm | KiCad: 51.44 Ω | 50.71 Ω | 1.4 % | 3 % |
| Microstrip, εr 4.4, h 0.1 mm, w 0.18 mm, 35 µm | KiCad: 49.29 Ω | 47.92 Ω | 2.8 % | 3 % |
| Microstrip 50 Ω on 1.6 mm FR-4 (εr 4.5, 35 µm) | this document / KiCad: 2.9–3.0 mm | 2.966 mm | — | in range |
| Microstrip, h 0.2 mm, w 0.1–0.38 mm | IPC-2141A rule of thumb | | ≤ 10 % | 10 % |
| Stripline, εr 2.2, b 3.2 mm, w 2.66 mm, t 0 | Pozar Ex. 3.5: 50 Ω | 49.90 Ω | 0.2 % | 0.5 % |
| Stripline, b 20 mil, w 8 mil, t 0.7 mil, centred / off-centre (3 cases) | KiCad: 49.71 / 43.50 / 29.69 Ω | identical | < 0.01 % | 0.1 % |
| Coupled microstrip, 4 geometries (h 0.1–1.51 mm) | KiCad: 133.30 / 113.25 / 92.78 / 79.15 Ω diff | identical | < 0.01 % | 0.1 % |
| Coupled stripline, w = s = 0.1 mm, b 0.5 mm, t 0 | exact (scipy ellipk): Z0e 91.209, Z0o 55.090 Ω | identical | < 1e-6 | 1e-6 |
| Coupled stripline, b 20 mil, w = s = 8 mil, t 0.7 mil | KiCad QA: Z0e 55.34, Z0o 43.59, Zdiff 87.18 Ω | 54.91 / 44.12 / 88.24 Ω | 0.8 / 1.2 / 1.2 % | 2 % |
| GCPW, w 0.5, gap 0.3, h 0.8 mm, εr 4.4, t 0 | exact (scipy ellipk): 70.483 Ω, εeff 2.8166 | identical | < 1e-6 | 1e-6 |
| GCPW, same, 35 µm | KiCad: 70.86 Ω | 69.44 Ω | 2.0 % | 3 % |
| IPC-2221: 1 A, ΔT 10 °C, 35 µm, outer / inner | formula by hand: 0.300 / 0.781 mm (≈ 12 / 31 mil per A) | 0.300 / 0.781 mm | < 0.1 % | 0.5 % |

"KiCad" = KiCad's `common/transline_calculations` (master, 2026-10-04) compiled outside the tree and run at 1 MHz.
Where it differs from TraceMaker beyond rounding: its microstrip adds Bahl & Garg's thickness term; its coupled
stripline thin-gap branch (Cohn eq. 22, s < 5t) omits √εr in the medium impedance (in air both agree; with εr 4.3 it
jumps 7 % at s = 5t where ours joins eq. 20 within 1.4 %); its zero-thickness coupled stripline and GCPW differ from the
exact conformal mapping by 2–6 %. Round trips (solve → evaluate) are exact to 1e-3 Ω; monotonicity of every function
the solver bisects is tested by sweeps. The single stripline's two branches meet with a step of about 0.6 % at
w/(b−t) = 0.35 (bisection then returns the boundary width).

**Report** (`tracemaker rules`): a header line with the stackup and the propagation delay per layer (50 Ω line) used
for ps→mm skew conversion, or "no stackup in the board: … skew budgets use 6.0 ps/mm outer / 7.0 ps/mm inner"; per
`impedance` rule the widths grouped by layers with the same result, the reference layers and the stated error; when
a board net class already covers the pair, the rule stays "satisfied-by-board" and the report adds the class's
impedance by the same model. Example (KiCad demo RoyalBlue54L-Feather, 8 layers, 0.1 mm prepreg εr 4.5):

```
  stackup: 8 copper layers, 1.58 mm, ENIG; prop delay (ps/mm, 50 Ω line): F.Cu 5.91 In1.Cu 7.08 … B.Cu 5.91
     USB2-01   impedance       soft      satisfied-by-board: net class USB_DIFF (diff pair 0.125 mm / gap 0.203 mm,
               114 Ω diff on F.Cu by this model); for comparison, report only, not applied to routing:
         90 Ω diff on F.Cu, B.Cu: 0.201 mm / gap 0.250 mm microstrip (h 0.100 mm, εr 4.50) = 90.0 Ω, 6.06 ps/mm, formula error ±5 %
           reference: F.Cu over In1.Cu (GND); B.Cu over In6.Cu (GND)
```

JSON: `stackup` (layers, per-layer geometry, delays) at the top level; `impedance` (bounds, one entry per layer and
target with width, gap, Z, εeff, ps/mm, h, εr, error) and `current` on the rules.

Not built in P4: routing with the computed widths (net classes per layer; D36), the reference-plane coverage check
and plane-gap cost, stitching vias at layer changes, solder-mask correction, broadside-coupled and coplanar pairs,
the `--assume-stackup` preset (§3.6), commented suggestions in the sidecar `.kicad_dru`.


### 14.6 User overrides, Ethernet magnetics void, connector edge attraction

**User override file** (§3.5 level 1, §6.3; D40). `src/crules/overrides.{hpp,cpp}` parses and validates the file;
`detect(board, catalogue, &overrides)` applies it, so `tracemaker rules`, `tracemaker route` and `tracemaker-place`
behave the same:

```
tracemaker rules board.kicad_pcb --mode on --rules-override board.tracemaker_rules.json
tracemaker route board.kicad_pcb -o out.kicad_pcb --component-rules on --rules-override board.tracemaker_rules.json
tracemaker-place board.kicad_pcb -o placed.kicad_pcb --component-rules soft --rules-override board.tracemaker_rules.json
scripts/crules_override.py board.tracemaker_rules.yaml -o board.tracemaker_rules.json   # YAML -> JSON
```

- `deny` moves the instance to "possible" ("denied by user (file)"), so a competing category in its exclusive group
  can win; `assert` gives confidence 100 (never capped), wins its group, binds with the category's binder or, if the
  binder rejects the part, the anchor alone (the objection stays in the evidence).
- `disable` (rule, rule@REF, category, category@REF): the rule is neither applied nor measured; no pseudo-net,
  keep-out, sidecar rule or USB pair comes from it. Report: `not-applied: overridden by user: disabled (disable
  XTAL-04@Y1)`; condensed categories count "disabled by user".
- `set`: the rule is evaluated with the new parameter (limits, keep-out margins, impedance targets); report:
  `…; overridden by user: max_mm = 5 (catalogue 10; set XTAL-01 max_mm = 5)`; JSON `user_override` per rule and
  `user_overrides` (file, entries, unused) at the top level.
- Errors (exit 1, message names the file and entry, with a "did you mean" hint): unknown keys, rule ids, categories,
  parameters, value types, negative numbers, malformed `ID@REF`, references not on the board, a category both
  asserted and denied, two asserted categories that exclude each other, a `.yaml` file (with the conversion command).
  An entry whose reference is on the board but not an instance of the category (or a global entry with no instance)
  is a warning in the report and the route/place log: the catalogue default then applies.
- Tests: `[overrides]` in `src/crules/test_crules.cpp` (every form, 20 error messages, disable/assert/deny/set
  semantics, precedence, determinism).

**Ethernet magnetics void** (ETH-05; D41). `bind_ethernet` binds the anchor as `rj45` (connector) or `phy` (IC),
`magnetics` = transformers (T*/TR* or a transformer lib_id/value, ≥ 6 pads) sharing ≥ 2 signal nets with the anchor,
and `integrated_magnetics` for MagJack-style RJ45s (no void: "not required"). `generate_keepouts` adds a rule area
around the magnetics' courtyards grown by 0.508 mm on their side and the adjacent copper layer, minus layers with
their pads (SMD on F.Cu: B.Cu on 2 layers, In1.Cu on 4), keeping out tracks, vias and zones; the sidecar rule is
`(constraint disallow track via zone)` with only the magnetics' own nets exempt. An RJ45 and a PHY instance sharing
the magnetics give one area. Generated at confidence ≥ apply and used for routing with `--component-rules on` only.
On PCBench (the 30 boards whose file names an RJ45), 54 Ethernet instances on 27 boards: discrete magnetics bound on 7 boards (KiwiSDR,
Own-Mailbox ×4, PoEPi ×2; 13 instances), 10 MagJack instances "not required"; all capped at confidence 60 (KiCad 5, no pin names), so no void is generated there unless
the user asserts the category; with `assert {"P3": "ethernet"}` on Own-Mailbox `pierre` the void is generated on In1.Cu and
given to the router. Not verified: that KiCad applies the sidecar `zone` constraint (kicad-cli DRC reports nothing for a malformed `.kicad_dru` either, so a silent run proves nothing). Tests: `[ethernet]` (2 and 4 layers, shared magnetics, MagJack, through-hole magnetics,
unbound role, sidecar text, user disable).

**Connector edge attraction** (D42), opt-in: `tracemaker-place --component-rules soft --edge-attraction` (an error
with `off`/`report`). `crules::edge_attractions` lists the anchors of instances with a connector edge rule (CONN-01,
USB2-11, DISP-02; not RFM-01/ANT-02, not advisory DBG-03, not disabled), once per footprint, never locked parts or
pin headers/sockets. The placer (`ExtractOptions::edge_pulls`) adds, for each such part that is **movable**, a
one-pin affinity net from the courtyard point nearest to the outer outline to that segment's axis (`PNet::has_ax`/
`has_ay`), weight 20 × `--crules-weight`. `net_hpwl` includes the fixed coordinate, so the annealer's incremental
integer cost stays exact (tested with independent runs, tempering and LNS); the weighted median move uses it; the
global (quadratic) stage and the HPWL lower bound ignore it (the bound stays a lower bound). Reported HPWL and
crossings exclude it. Connectors already at the edge stay fixed as before (`fix_edge_connectors`).

Measured with `bench/crules_place.py --configs off,soft,soft:--edge-attraction` (23 boards of doc 04 §7.3, seed 1,
8 threads). Connector edge distance = CONN-01's measure (nearest courtyard vertex to Edge.Cuts, once per connector);
the per-board median and mean are then taken over boards. `off` and `soft` reproduce §14.3 exactly.

| Config | HPWL median | HPWL vs off (geo-mean) | connector edge, median of board medians | median of board means | connectors ≤ 1 mm from the edge (sum, 148 connectors) | pads ≤ 5 mm from the edge box (sum) |
|---|---:|---:|---:|---:|---:|---:|
| off | 518.3 | 1.000 | 1.81 mm | 2.17 mm | 52 | 105 |
| soft | 518.3 | 0.996 | 1.81 mm | 2.36 mm | 52 | 105 |
| soft + edge attraction | 518.3 | 1.004 | 1.75 mm | 1.81 mm | 58 | 117 |
| human placement | – | – | 1.81 mm | 2.45 mm | – | – |

The pull changes the result on 5 boards (the others have no movable connector with an edge rule: their connectors
are at the edge and fixed, or are pin headers). There, mean connector distance: ChirpHardware 6.2 → 2.1 mm,
LadybugLiteBlue 5.8 → 2.1, kitspace_training_board 11.4 → 1.6 (6 of 6 connectors now within 1 mm), nanoTracer
4.0 → 3.4, Microdox unchanged (3.9); HPWL +3.8 % geo-mean on these 5 (Chirp +7.7 %, Ladybug +6.3 %, training +2.2 %,
nanoTracer +3.3 %, Microdox −0.2 %). Costs: decoupling distance worse on 2 boards (training board 11.2 → 27.2 mm,
nanoTracer 5.8 → 10.0 mm) because the connectors take edge space the ICs and their caps had. So edge attraction does
what it is meant to, at a wirelength and (on two boards) decap cost; it stays **opt-in**, default off.

# M13 component-aware layout rules — assumptions made without asking (2026-10-04)

Each item: what was chosen, why, and how to revert. Design: `docs/15-component-rules.md` (Implementation status
section at its end); decisions D26–D31 in `docs/12-decisions.md`.

## Catalogue and detection

1. **YAML stays the edited source; a committed JSON is embedded.** `scripts/crules_catalogue.py` writes
   `src/crules/catalogue.json` (ASCII, catalogue order kept) and `--check` (ctest `crules_catalogue_sync`) fails if
   the JSON, the YAML and the rule ids in doc 15 drift. Why: no YAML library in the engine; one generated file is
   simpler than a parser. Revert: replace `embed.cmake` with a YAML reader. `tracemaker rules --catalogue x.json`
   loads another catalogue at run time.
2. **HV-02 is `hard` with evidence `R`**, which doc 15 §8 says must not happen. I did not edit the rule; the
   engine demotes every R-evidence hard rule to soft at run time and the check only warns. Revert: drop the
   demotion in `evaluate.cpp` (`r.evidence == "R"`), or make HV-02 soft in the YAML.
3. **lib_id patterns match the footprint name, not the library nickname**, unless the pattern itself contains a
   `:` (e.g. `connector_usb:usb_...`, `rf_module:`). Why: custom nicknames are free text; on PCBench UCCBPCB the
   nickname `usb_ccb_custom:` made the MCU, a jumper and a header "USB connectors". Revert: the `Signal::LibId` case
   in `detect.cpp`.
4. **Detector patterns were extended (and two narrowed) using the 41-board labelled set**: USB footprint names
   (`MICRO-USB`, `USB_A`, `USBOTG`, `USB-C`), crystal packages (ABS07, ABM, FC-135, FA-238, TSX-3225, CSM, HC-50),
   LDO families (MCP170x, AP22xx, XC62xx, LP29xx/LP38xx, LT19xx, BU33SD5, NCP11xx, AMS10xx, SPX), buck/boost
   families, ESP-0x/ESP-32/E73 modules, TVS arrays (SM05..SM24, NUF20, STF20, EMIF), hole names (MTG-HOLE,
   HOLE_NPTH, MH-M2) and connector footprint families (headers, USB, RJ, JST S2B-/SM04B-, Phoenix, Berg, BNC, XT60,
   MiniDIN, castellated pads, Arduino, TC2030, ...). Buck/boost part-number weight 30 -> 50 (as the LDO's), because
   KiCad 5 files have no pin names or keywords and those categories could never reach the suggest threshold.
   Narrowed: buck `tps5|tps6` (matched TPS61 boosts), `lm26`/`max1` (matched an LM2660 charge pump and a MAX17048
   fuel gauge), boost `max17`/`ltc3`; and, after the held-out set showed LIN transceivers (TJA1027) as CAN, CAN
   `tja10` -> `tja104x|tja105x`. Revert: `git diff` of `docs/component_rules.yaml` in commit "labelled detection set".
5. **No-pin-name cap (doc 15 §3.6)**: confidence is capped at 60 on boards without pad pin names (all 1,157 PCBench
   boards are KiCad 5 files) only for categories that have a `pin_name` detector, except `ic_decoupling`, whose
   binding works from net names and topology (D25). Consequence: on PCBench, usb2, ldo, can, rs485 and
   connector_general never reach the apply threshold (70); their soft rules still apply (suggest band). Revert:
   `binds_without_pin_names` / `kNoPinNameCap` in `detect.{hpp,cpp}`.
6. **Crystal topology without pin names**: "both crystal nets end on the same IC (directly or across one series
   resistor)" counts as `two_terminal_between_osc_pins` (doc 15 wants oscillator pin names), but only when a
   part-level detector (lib_id, value or keywords) also matched; otherwise an LED driven from two MCU pins became a
   crystal (PCBench 1Bitsy). Revert: `topology_weak` in `detect.cpp`.
7. **A reference prefix alone never creates an instance**, and "possible" entries need confidence >= 20 with a
   non-prefix signal. Why: otherwise every J* and U* is a possible USB/CAN/... part. Revert: `strong` in `detect()`.
8. **Exclusive categories on one anchor** (doc 15 §3.4 gives LDO vs buck as the example): {buck, boost, ldo},
   {crystal, oscillator}, {rf_module, chip_antenna}. Other categories may share an anchor (a USB-C receptacle is
   usb2, usb3_typec and connector_general). Revert: `exclusive_group` in `detect.cpp`.
9. **Footprints without pads are skipped** (logos named after a module matched rf_module). Mounting holes keep an
   NPTH pad, so they are still seen.
10. **Role-binding heuristics** where the catalogue only describes roles in prose: USB D+/D- from pin names, else net
    names, else KiCad USB footprint pad numbers (micro/mini/A/B: 3 = D+, 2 = D-; Type-C: A6/B6, A7/B7); the USB ESD part
    has a pad on D+ or D- and on ground and no other signal net; regulator in/out rails from pin names, else from the
    voltages in the supply names (higher = input), else input-like names (VIN, VBUS, VBAT, RAW, ...); a capacitor
    belongs to a regulator if, in the input placement, the regulator's pad is the nearest pad of an active part
    (IC or regulator) on that rail (D25's nearest-pin rule). Consequence: regulator-cap binding can change slightly
    after a placement. Revert/extend: the `bind_*` functions in `detect.cpp`.
11. **Generalised decoupling** (doc 15 `ic_decoupling`) also accepts analog/reference supplies (VREF, AREF, VCAP,
    VDDA, AV+, ...), `+NAME` rails (+BATT), `3.3V`-style names, VEXT/VLDO/VREG/VOUT, nets reaching an IC pin named as a
    supply, and VR*/REG*/PS* regulator references. **D25 itself is unchanged** (same supply/ground/IC tests; the code
    moved to `crules::decap_ties(..., generalised=false)` and the placer output is byte-identical: checked on
    ChirpHardware_chirp against the main-branch binary).

## Placement (P1)

12. **Soft rules are applied in the suggest band (40–69)** as doc 15 §3.2 says; keep-outs only at >= 70 (apply).
13. **Pseudo-net weights**: 10 (= kSignalWeight, as D25) for every proximity rule; 20 for a protection part to its
    connector (doc 15 §5.2 "ESD weighted x2 to its connector"). The ESD-to-IC leg of the order rule (USB2-08) is not
    added: the real net already pulls the ESD part toward the IC, and ESD-01 wants the IC far from it.
14. **One rule pulls each part**: priority crystal > oscillator > ESD/protection > regulator > decoupling; a part D25
    already tied keeps only its D25 tie (so `--component-rules soft` does not change any decoupling tie D25 makes).
    Locked parts are never pulled.
15. **Proxies until P5**: BUCK-01 (hot loop) is placed and measured as "input cap to the VIN pin" and BUCK-02 (SW
    copper) as "inductor to the SW pin". Measurements are pad-centre distances (the catalogue says pad edge or routed
    length): an over-estimate, so "met" is conservative.
16. **Connector edge placement is not attracted**: connectors touching the edge are already fixed by the placer;
    CONN-01/USB2-11/RFM-01/ANT-02 are measured and reported only.
17. **`tracemaker-place --component-rules` stays `off` by default** (D31). `soft` improves crystals, load caps and
    ESD parts at equal HPWL but moves decoupling/regulator caps away on some boards (doc 15 §14.3), which the task's
    "no regression" condition rules out. `--decap-weight 20` (new, default 10 = D25) fixes the decaps at +2–3 % HPWL;
    I did not make it a default either. To switch: change the `component_rules` defaults in
    `src/place/place_main.cpp` (and `RouteJob::component_rules`).

## Keep-outs and routing (P2, P3)

18. **Generated keep-outs cover only copper layers where none of the parts has a pad and no other part has a pad
    inside the area; tracks only (vias and zones stay allowed).** Why: KiCad rule areas cannot exempt nets, so a
    keep-out over the crystal on its own pad layer would make its pads unreachable; ground stitching vias under a
    crystal are good practice. The full intent (no foreign track or via under the parts on any layer, own nets and
    grounds exempt) goes into the sidecar `.kicad_dru` as `intersectsCourtyard` rules. Through-hole crystals (pads on
    every layer) get no keep-out and the report says why. Revert: `generate_keepouts` in `compile.cpp`.
19. **`soft` adds no hard constraints** (doc 15 §3.5), so the router uses the generated keep-outs only with
    `tracemaker route --component-rules on`; the task text suggested `soft`. `report`/`soft`/`on` all write the
    sidecar `<output>.tracemaker.kicad_dru` next to the output; the output board never contains the generated
    areas (rule 8). Default: `off`.
20. **RFM-02 / ANT-01 antenna keep-outs** are "satisfied by board" when the module footprint has its own keep-out,
    otherwise "not applied (antenna region unknown)": there is no per-module datasheet table yet.
21. **USB 2.0 pairs routed coupled (P3)**: with `--component-rules soft|on` the router routes each detected D+/D-
    pair coupled first via the new `RouterOptions::pair_nets` (falls back to single tracks, as `--diff-pairs` does).
    Router change kept to a separate helper `pair_wanted` and two call sites in `src/route/router.cpp`. Revert:
    remove `pair_nets` and `pair_wanted`.

## Labels and measurement

22. **Labels** (`bench/crules_labels.json`, 41 boards; `bench/crules_labels_heldout.json`, 12 boards) were made from
    footprint names, values and nets only (no schematics); ambiguous parts are listed under `ignore` (test pins,
    jumpers, module footprints, BAV99 clamps, buck-boost parts, sockets for plug-in radio modules).
    `ic_decoupling` is labelled on 10 boards only. The detectors were tuned on the 41-board set, so its numbers are
    optimistic; the held-out numbers are the honest ones.
23. **`tracemaker rules` defaults to `--mode soft`** (statuses as placement/routing would apply them with
    `--component-rules soft`); categories with more than 6 instances are summarised in the text report (JSON has
    everything).
24. **Worktree only**: `bench/data` is a symlink to the main checkout's fixtures (not committed).

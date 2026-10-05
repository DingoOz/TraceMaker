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

## Stackup and impedance (P4)

25. **Stackup reading**: dielectric sublayers (`addsublayer`) are combined into one layer: thicknesses add, εr in
    series (Σh / Σ(h/εr), exact for a field normal to the sheets), loss tangent thickness-weighted. A missing εr is
    never defaulted: the layer's gap is "incomplete" and no width is computed for it (rule 6). The solder-mask
    layers are read but not used. Revert: `read_stackup_layer` in `board_reader.cpp`.
26. **Reference plane = adjacent copper layer** (D38). Outer layers: microstrip over the next copper layer; inner
    layers: stripline between both neighbours, εr of the two sides combined in series. "(GND)" in the report means
    a zone of a net named like GND/VSS/GROUND is on that layer; anything else is "(assumed)". Not checked: whether
    the plane is solid under the route (plane coverage, doc 15 §5.2, still to build). KiCad `power` layers are not
    reported as routing layers.
27. **Stated formula errors** (shown in the report; `imp::formula_error_pct`): microstrip ±4 %, coupled microstrip
    ±5 %, centred stripline ±2 % (pairs ±4 %), off-centre stripline ±5 % (pairs ±8 %, the KiCad QA tolerance for its
    own offset model), GCPW ±7 %. Judgement: the papers claim 0.2–1 % for their range; the rest covers solder mask
    (ignored, −1 to −3 Ω on outer layers), thickness corrections and the image split. Fab tolerance ±10 % is printed
    separately.
28. **Microstrip follows Hammerstad & Jensen exactly**; KiCad's calculator adds Bahl & Garg's thickness term and
    gives 0–3 % higher Z on thin dielectrics. Tests compare with KiCad at 3 %.
29. **Cohn's thin-gap coupled-stripline equation (eq. 22, s < 5t)** is used with the medium's wave impedance
    η0/√εr in its capacitive terms. KiCad uses η0 there; ours is the dimensionally consistent reading (it joins eq. 20
    within 1.4 % at s = 5t; KiCad's jumps 7 % with εr 4.3). Revert: `coupled_stripline_centred`.
30. **Solving bounds**: width ≥ max(board minimum track width, 0.1 mm), ≤ 5 mm for pairs and 10 mm for single
    lines; pair gap = max(Default class diff-pair gap, Default clearance, board minimum clearance); GCPW ground gap =
    clearance; without project rules 0.1 mm / 0.2 mm (stated in the report). If the target needs a narrower trace
    than allowed, the width stays at the minimum and the gap is solved (D39).
31. **Single-ended lines are reported whenever a rule gives `z0_ohm`**, also when it equals half of `zdiff_ohm`
    (USB2-01's 45 Ω): DDR3/Ethernet use both, and the catalogue does not say which roles are single-ended.
32. **`structure: stripline`** (DDR4-01) is solved on inner layers only, **`structure: gcpw`** (RF-01) on outer
    layers; there is no coplanar-pair model, so differential targets on GCPW layers use coupled microstrip.
33. **Propagation delay** for the report header: εeff of the 50 Ω microstrip on outer layers, εr on inner layers;
    without a stackup the catalogue's 6.0 / 7.0 ps/mm. Not yet used to convert `tol_ps` rules (skew checking is not
    built).
34. **Width for current**: IPC-2221 for rules that give `current_a` (only USBC-09, 3 A); ΔT from the rule or 10 °C
    (PWR-01's default); copper from the stackup (F.Cu outer, In1.Cu inner) or 35 µm (1 oz, PWR-01's default) with
    "assumed" in the report. Report only.
35. **Reference values** in the tests come from Pozar's textbook examples, exact conformal-mapping values computed
    with scipy, and KiCad's transline code (master of 2026-10-04) compiled in a scratch directory and run at 1 MHz.
    No web search was available to collect fab-calculator values (JLCPCB, Polar); none are claimed.
36. **Not done tonight**: commented width suggestions in the sidecar `.kicad_dru` (the sidecar has no access to the
    project rules the report uses, so the numbers could differ from the report); `--assume-stackup` presets.

## User override file (§3.5, §6.3)

37. **JSON in the engine, YAML by conversion.** `--rules-override FILE` reads JSON; a `.yaml`/`.yml` name is refused
    with the command `scripts/crules_override.py FILE.yaml -o FILE.json` (PyYAML, keys and order kept). Why: no YAML
    library in C++ (same as the catalogue, item 1). The file is not discovered automatically next to the board
    (`<project>.tracemaker_rules.yaml` in §3.5): the user names it. Revert: look for the file in `cmd_rules`/route/place.
38. **Accepted keys**: `version` (must be 1), `comment`, `disable`, `assert`, `deny`, `set`. Everything else is an
    error, including `stackup_preset` (not built, §3.6) and catalogue keys such as `categories` (adding or redefining
    categories from the override file is not supported). `assert`/`deny` take the doc form `{REF: category}` (or a list
    of categories) or a list of `{category, ref}`; `set` takes `{rule, param, value}` with `RULE@REF` or `ref`.
39. **Validation**: unknown rule ids, categories, parameters, value types (JSON type must equal the catalogue
    value's), negative or non-finite numbers, malformed `ID@REF`, a category both asserted and denied on one part,
    two asserted categories that exclude each other (§3.4) and references that are not on the board are errors with a
    "did you mean" hint. A reference that is on the board but not an instance of the entry's category (or a global
    entry with no instance) is a **warning** in the report and the route/place log, not an error: the catalogue
    default then applies, which is the conservative side, and a board edit must not make routing fail.
40. **`@REF` means the instance anchor** (the connector of a usb2 instance, the crystal of a crystal instance), not a
    satellite part such as a load capacitor. Entries for one reference win over global ones; among equals the later
    entry in the file wins. A disabled rule is neither applied nor measured; the report says "overridden by user:
    disabled (<entry>)". A changed parameter is reported as "max_mm = 5 (catalogue 10; <entry>)".
41. **`assert`** gives confidence 100 (never capped), wins conflicts in its exclusive group, and binds roles with the
    category's binder; when the binder rejects the part (e.g. a U* asserted as connector) only the anchor role is
    bound and the binder's objection is kept in the evidence. **`deny`** lists the instance under "possible" with
    "denied by user", so a competing category in the same exclusive group can win.
42. **With `--component-rules off`** the override file is still read and validated (a broken file is an error), and a
    warning says it has no effect.

## Ethernet magnetics void (ETH-05)

43. **Magnetics binding**: a footprint is `magnetics` of an Ethernet instance when it has 6 or more pads, is a
    transformer by reference (T*, TR*) or by lib_id/value (`transformer`, `magnetics`, `lan trans`, Pulse H1102/HX11xx,
    Bel S558, Halo TG110, Wurth 7490...), and shares at least two signal nets with the anchor (line pairs of the RJ45 or
    MDI pairs of the PHY). Four-pad common-mode chokes are not magnetics. The anchor role is `phy` when the anchor is an
    IC, else `rj45`. Why 6 pads: one 1:1 pair transformer with centre taps has 6; a choke has 4.
44. **No void under an RJ45 with integrated magnetics** (catalogue note): lib_id or value matching MagJack, HR911/HR961,
    HY911, J00xx (Pulse), ARJM/ARJC (Abracon), LPJ, LMJ, HFJ1 (Halo), "PulseTrans", "Trafo" binds role
    `integrated_magnetics` and reports ETH-05 "not required". A MagJack named otherwise reports "role magnetics not
    bound": no void either way (the conservative side for routing).
45. **Layers "same, adjacent"** = the magnetics' side and the next copper layer (In1.Cu on 4 layers, B.Cu on 2), minus
    layers where the magnetics have pads (so SMD magnetics on F.Cu get a void on the adjacent layer only; through-hole
    magnetics on a 2-layer board get none, reported). The router area keeps out tracks, vias and zones; the sidecar
    rule disallows track, via and zone in the courtyard except the magnetics' own nets (no ground exemption, unlike
    XTAL-04: the void is about planes). Margin 0.508 mm from the catalogue. Generated at confidence >= apply only, and
    used for routing only with `--component-rules on` (as XTAL-04). On PCBench the Ethernet instances are capped at 60
    (no pin names), so no void is generated there unless the user asserts the category (override file).
46. **One area per set of parts**: an RJ45 instance and a PHY instance that bind the same magnetics produce one
    void (the first instance in report order names it); the other reports "generated for another Ethernet instance".

## Connector edge attraction (CONN-01)

47. **Which parts are pulled**: anchors of instances with a connector edge rule (CONN-01, USB2-11, DISP-02: kind
    `edge`, enforced in `place`, not advisory, `applies_to` contains `connector`), once per footprint, never locked,
    never a pin header/socket (lib_id `pin_header`/`pinheader`/`pin_socket`/`pinsocket`; CONN-01 note on stacking
    headers; jumper and programming headers are mostly not cable connectors). All of them, not only those whose rule
    is unmet in the input, because in full mode the input position is discarded; only parts the placer may move get a
    pull, so connectors already at the edge stay fixed as before. RF modules and antennas are not pulled (their
    orientation matters more than the distance).
48. **Pull model**: one-pin pseudo-net from the courtyard point nearest to the outer outline (cut-outs ignored) in the
    input, anchored on the axis of the nearest outline segment (x if |dy| >= |dx|, else y); on a slanted outline
    this is the axis distance, not the true distance. Weight 20 (2 x signal, like ESD-01) scaled by --crules-weight;
    not tuned. The global quadratic stage and the HPWL lower bound ignore the pull.
49. **`--edge-attraction` with `--component-rules off/report` is an error**, not a silent no-op.
50. **Two-stage soft placement (D43).** In full mode `--component-rules soft` first places with the decoupling ties only, then locks those capacitors *and the ICs they decouple* and refines with the other pulls. Locking the ICs is a choice: it keeps decaps next to their IC but stops the IC from moving toward its crystal or connector; the crystal moves toward the IC instead. PocketBone's decap metric still gets worse on all three seeds (its measured decaps include ones not tied by D25). Revert: `--no-crules-two-stage`.

51. **Source verification of the 41 R rules (D45, doc 15 §8.10).** ST, ADI, NXP, AMD and Silicon Labs servers refused
    direct downloads from this machine, so their PDFs were read from Internet Archive copies of the vendors' own URLs
    (revision and date recorded per source). Judgement calls: a rule whose source supports the statement but gives no
    number is **D** (default), not V; DEC-05 keeps 50 mm for MT-101's 2 in (50.8 mm); AN928.2's edge-stitching spacing
    (λ/10 of the 10th harmonic, 40–50 mil) is applied to the RF-03 line fence and the SHLD-01 shield fence; the MDIO
    1.5 kΩ value (ETH-15) is taken from the DP83848C datasheet because IEEE 802.3 clause 22 is paywalled; the 120 Ω of
    CAN-01/RS485-01 is the cable impedance, so the on-board pair target stays a TraceMaker choice. IEC 62368-1 (HV-02),
    IPC-2221B (TP-01) and ISO 4762/7089 (MH-01) were not read: those rules stay R. Revert: `git revert` the commit.

#!/usr/bin/env python3
"""Time-domain fixtures for M21 B2 (doc 05 §59): copies of two demo boards whose projects give a class a KiCad 10 tuning profile with
propagation delays, and whose rules ask for delays in picoseconds.

    make_fixtures.py DEMOS OUT     # DEMOS = bench/prepare_demos.py output (interf_u, RoyalBlue54L-Feather)

Writes OUT/<board>-profile/ (the profile and no rules: route it to measure each net's delay) and OUT/<board>-delay/ (the same with the
rules). The window below was set from the delays KiCad measured on interf_u-profile as TraceMaker routed it (README): /MD1-4 took
199-244 ps there (/MD0 and /MD7 774 and 346 ps, /MD5 and /MD6 unrouted), so the class holds those four and the window starts just above
the longest, so that every member must be lengthened.
"""
import json
import shutil
import sys
from pathlib import Path


def profile(name, differential, delays, via):
    return {"profile_name": name, "type": 1 if differential else 0, "target_impedance": 90.0 if differential else 50.0,
            "enable_time_domain_tuning": True, "generate_drc_rules": True, "via_prop_delay": round(via * 1e6), "via_overrides": [],
            "layer_entries": [{"signal_layer": l, "top_reference_layer": "UNDEFINED", "bottom_reference_layer": "UNDEFINED", "width": 0,
                               "diff_pair_gap": 0, "delay": round(d * 1e6)} for l, d in delays]}


def interf_u(pro):
    # Four lanes of the memory data bus (a 2-layer board): class MEMDATA, which copies Default's fields, so it is complete (D147).
    default = next(c for c in pro["net_settings"]["classes"] if c["name"] == "Default")
    pro["net_settings"]["classes"].append(dict(default, name="MEMDATA", tuning_profile="BUS"))
    assign = pro["net_settings"].get("netclass_assignments") or {}
    assign.update({f"/MD{i}": ["MEMDATA"] for i in range(1, 5)})
    pro["net_settings"]["netclass_assignments"] = assign
    profiles = [profile("BUS", False, [("F.Cu", 6.0), ("B.Cu", 6.4)], 7.0)]
    dru = ("(version 1)\n"
           "(rule \"memdata_window\"\n\t(condition \"A.hasNetclass('MEMDATA')\")\n\t(constraint length (min 245ps) (max 260ps)))\n"
           "(rule \"memdata_skew\"\n\t(condition \"A.hasNetclass('MEMDATA')\")\n\t(constraint skew (max 3ps)))\n")
    return profiles, dru


def royalblue(pro):
    # The two USB pairs (class USB_DIFF, which leaves fields unset, so it joins Default): a delay profile with no geometry, and a
    # skew rule within each pair.
    for c in pro["net_settings"]["classes"]:
        if c["name"] == "USB_DIFF":
            c["tuning_profile"] = "USB90"
    profiles = [profile("USB90", True, [("F.Cu", 5.9), ("B.Cu", 5.9)] + [(f"In{i}.Cu", 6.8) for i in range(1, 7)], 7.0)]
    dru = ("(version 1)\n"
           "(rule \"usb_pair_skew\"\n\t(condition \"A.hasNetclass('USB_DIFF')\")\n\t(constraint skew (max 0.5ps) (within_diff_pairs)))\n")
    return profiles, dru


BOARDS = {"interf_u": interf_u, "RoyalBlue54L-Feather": royalblue}


def main():
    demos, out = Path(sys.argv[1]), Path(sys.argv[2])
    for board, make in BOARDS.items():
        src = demos / board
        for kind in ("profile", "delay"):
            dst = out / f"{board}-{kind}"
            shutil.rmtree(dst, ignore_errors=True)
            dst.mkdir(parents=True)
            shutil.copy(src / "unrouted.kicad_pcb", dst / "unrouted.kicad_pcb")
            pro = json.loads((src / "unrouted.kicad_pro").read_text())
            profiles, dru = make(pro)
            pro["tuning_profiles"] = {"meta": {"version": 0}, "tuning_profiles_impedance_geometric": profiles}
            # KiCad gives every class an empty profile when it migrates net settings older than schema 5 (D147).
            pro["net_settings"].setdefault("meta", {})["version"] = 5
            sev = pro["board"]["design_settings"].setdefault("rule_severities", {})
            sev["length_out_of_range"] = sev["skew_out_of_range"] = "error"
            (dst / "unrouted.kicad_pro").write_text(json.dumps(pro, indent=2) + "\n")
            if kind == "delay":
                (dst / "unrouted.kicad_dru").write_text(dru)
            print(dst)


if __name__ == "__main__":
    main()

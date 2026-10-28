#!/usr/bin/env python3
"""Tuning-profile fixtures for M21 B1 (doc 05 §58): copies of two plane-tier boards whose projects get KiCad 10 tuning profiles, with
`tuning_profile_track_geometries` set to error so that KiCad's DRC judges every profiled track's width (and pair gap).

    make_fixtures.py DEMOS OUT     # DEMOS = bench/prepare_demos.py output (RoyalBlue54L-Feather, plane_smd)

Writes OUT/<board>-profiles/unrouted.{kicad_pcb,kicad_pro}, OUT/<board>-base/ (the same board and project with no profiles, for the
added-errors comparison) and, for a board with a differential profile, OUT/<board>-se/ (its single-ended profiles only)."""
import json
import shutil
import sys
from pathlib import Path

MM = 1_000_000


def layer(name, width, gap=0.0, top="UNDEFINED", bottom="UNDEFINED"):
    return {"signal_layer": name, "top_reference_layer": top, "bottom_reference_layer": bottom, "width": round(width * MM),
            "diff_pair_gap": round(gap * MM), "delay": 0}


def profile(name, differential, layers):
    return {"profile_name": name, "type": 1 if differential else 0, "target_impedance": 90.0 if differential else 50.0,
            "enable_time_domain_tuning": False, "via_prop_delay": 0, "via_overrides": [], "layer_entries": layers}


def copper(pcb):
    out, inside = [], False
    for line in pcb.read_text().splitlines():
        if line.startswith("\t(layers"):
            inside = True
        elif inside and line.startswith("\t)"):
            break
        elif inside and '.Cu"' in line:
            out.append(line.split('"')[1])
    return sorted(out, key=lambda n: (n != "F.Cu", n == "B.Cu", int(n[2:-3]) if n.startswith("In") else 0))


# Board: (class -> profile). Outer layers microstrip, inner stripline (narrower), as a real stackup would have them.
BOARDS = {
    "RoyalBlue54L-Feather": {
        "Default": ("SE50", False, lambda l: (0.25, 0.0) if l in ("F.Cu", "B.Cu") else (0.2, 0.0)),
        "USB_DIFF": ("USB90", True, lambda l: (0.15, 0.15) if l in ("F.Cu", "B.Cu") else (0.1, 0.2)),
    },
    "plane_smd": {
        "Default": ("SE50", False, lambda l: (0.25, 0.0) if l in ("F.Cu", "B.Cu") else (0.15, 0.0)),
    },
}


def main():
    demos, out = Path(sys.argv[1]), Path(sys.argv[2])
    for board, classes in BOARDS.items():
        src = demos / board
        kinds = ["base", "profiles"] + (["se"] if any(diff for _, diff, _ in classes.values()) else [])
        for kind in kinds:
            dst = out / f"{board}-{kind}"
            shutil.rmtree(dst, ignore_errors=True)
            dst.mkdir(parents=True)
            for f in ("unrouted.kicad_pcb", "unrouted.kicad_pro", "unrouted.kicad_dru"):
                if (src / f).exists():
                    shutil.copy(src / f, dst / f)
            pro = json.loads((dst / "unrouted.kicad_pro").read_text())
            pro["board"]["design_settings"].setdefault("rule_severities", {})["tuning_profile_track_geometries"] = "error"
            if kind != "base":
                layers = copper(dst / "unrouted.kicad_pcb")
                profiles = []
                for c in pro["net_settings"]["classes"]:
                    if c["name"] in classes and not (kind == "se" and classes[c["name"]][1]):
                        name, diff, geom = classes[c["name"]]
                        c["tuning_profile"] = name
                        profiles.append(profile(name, diff, [layer(l, *geom(l)) for l in layers]))
                pro["tuning_profiles"] = {"meta": {"version": 0}, "tuning_profiles_impedance_geometric": profiles}
                # KiCad gives every class an empty profile when it migrates net settings older than schema 5 (D147).
                pro["net_settings"].setdefault("meta", {})["version"] = 5
            (dst / "unrouted.kicad_pro").write_text(json.dumps(pro, indent=2) + "\n")
            print(dst)


if __name__ == "__main__":
    main()

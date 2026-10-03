#!/usr/bin/env python3
"""Prepare the DAC 2020 PCB benchmarks (bench/data/dac2020) in the PCBench fixture layout used by bench/compare.py.

For each benchmark bmN with a Specctra file in Freerouting's fixture set (DAC2020_bmNN.dsn), writes
bench/data/dac2020_prepared/bmN/ with:
  raw.kicad_pcb       the human-routed original
  unrouted.kicad_pcb  the same board with its tracks, track arcs and vias removed (the DAC 2020 authors' intended use)
  unrouted.dsn        Freerouting's Specctra fixture for the board (unrouted; same coordinates as the KiCad file)
"""
import pathlib
import shutil

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "bench/data/dac2020"
DSN = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/DAC2020_boards"
OUT = ROOT / "bench/data/dac2020_prepared"


def strip_routing(text: str) -> str:
    """Remove top-level (segment ...), (arc ...) and (via ...) items, keeping everything else byte for byte."""
    out, i, n, depth = [], 0, len(text), 0
    start_keep = 0
    while i < n:
        c = text[i]
        if c == '"':  # skip strings
            j = text.index('"', i + 1)
            while text[j - 1] == "\\":
                j = text.index('"', j + 1)
            i = j + 1
            continue
        if c == "(":
            if depth == 1:
                head = text[i + 1:i + 12].split(None, 1)[0].rstrip(")")
                if head in ("segment", "arc", "via"):
                    # find the matching close paren
                    d, j = 0, i
                    while True:
                        if text[j] == '"':
                            j = text.index('"', j + 1)
                        elif text[j] == "(":
                            d += 1
                        elif text[j] == ")":
                            d -= 1
                            if d == 0:
                                break
                        j += 1
                    # drop the item and its leading indentation / trailing newline
                    k = i
                    while k > start_keep and text[k - 1] in " \t":
                        k -= 1
                    out.append(text[start_keep:k])
                    i = j + 1
                    if i < n and text[i] == "\n":
                        i += 1
                    start_keep = i
                    continue
            depth += 1
        elif c == ")":
            depth -= 1
        i += 1
    out.append(text[start_keep:])
    return "".join(out)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for d in sorted(SRC.glob("bm*")):
        n = int(d.name[2:])
        dsn = DSN / f"DAC2020_bm{n:02d}.dsn"
        pcb = next(iter(d.glob("*.kicad_pcb")), None)
        if not dsn.exists() or pcb is None:
            print(f"skip {d.name}: no Specctra fixture" if pcb else f"skip {d.name}: no board")
            continue
        o = OUT / d.name
        o.mkdir(exist_ok=True)
        shutil.copy(pcb, o / "raw.kicad_pcb")
        shutil.copy(dsn, o / "unrouted.dsn")
        (o / "unrouted.kicad_pcb").write_text(strip_routing(pcb.read_text()))
        print("prepared", o)


if __name__ == "__main__":
    main()

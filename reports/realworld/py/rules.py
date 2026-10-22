# SPDX-License-Identifier: GPL-3.0-or-later
"""Rule tiers for the real-world report. A tier is a set of design-rule values written into a copy of the board (setup block and net classes)
so the router routes under them; the routed copper is then put back on a copy with the board's own rules and judged there: every violation is
a compromise to log."""
from __future__ import annotations

import re

# name: (copper clearance, minimum track width, via diameter, via drill, via minimum diameter, via minimum drill). mm.
TIERS = {
    "T0": dict(clr=0.20, tmin=0.20, via=0.80, drill=0.40, vmin=0.40, dmin=0.30),   # the boards' own rules (KiCad 5 defaults of the designs)
    "T1": dict(clr=0.15, tmin=0.15, via=0.60, drill=0.30, vmin=0.50, dmin=0.25),   # SMD pad-to-pad 0.15 mm is JLCPCB's stated limit; 2x the 2-layer track/space limit
    "T2": dict(clr=0.127, tmin=0.127, via=0.50, drill=0.25, vmin=0.45, dmin=0.20), # 5 mil: 27 % above the 2-layer 0.10 mm limit, 41 % above the 4-layer 0.09 mm
    "T3": dict(clr=0.127, tmin=0.127, via=0.40, drill=0.20, vmin=0.40, dmin=0.20), # experiment, not a fab-confirmed tier: 0.4/0.2 via (ring 0.1 mm); the designers of EtherCAT_shield used 0.4/0.3
}


def own_tier(text: str) -> dict:
    """The tier of the board's own rules, read from its setup block (the values `apply` writes). A value the file lacks falls back to T0's."""
    def get(name, default):
        m = re.search(rf"\({name} ([0-9.]+)\)", text)
        return float(m.group(1)) if m else default
    d = TIERS["T0"]
    return dict(clr=get("trace_clearance", d["clr"]), tmin=get("trace_min", d["tmin"]), via=get("via_size", d["via"]), drill=get("via_drill", d["drill"]),
                vmin=get("via_min_size", d["vmin"]), dmin=get("via_min_drill", d["dmin"]))


POWER_NAME = re.compile(r"(^|[^A-Za-z0-9])\+|^V(CC|DD|BUS|IN|BAT|B|S)|[0-9]V[0-9]|^[0-9]+[Vv][0-9]*|^A?D?GND|GND$|^VDD|^VCC", re.I)
ADD_NET = re.compile(r"^\s*\(add_net (.*)\)\s*$")
ADD_NET_ANY = re.compile(r'\(add_net ("(?:[^"\\]|\\.)*"|[^\s()]+)\)')   # anywhere in a block: the last one of a class may share its line with the block's closing parenthesis


def class_blocks(text: str) -> list[tuple[int, int]]:
    """(start, end) of every net class form, wherever its closing parenthesis is (the top-level forms of the file)."""
    import pcbtext
    return [(a, b) for h, a, b in pcbtext.toplevel(text) if h == "net_class"]


def class_nets(block: str) -> list[str]:
    return [_name(m.group(1)) for m in ADD_NET_ANY.finditer(block)]


def _name(s: str) -> str:
    s = s.strip()
    return s[1:-1] if len(s) > 1 and s[0] == s[-1] == '"' else s


def power_nets(text: str, ground: bool = True) -> list[str]:
    """Power and ground nets of a board file: by name (+rails, VCC/VDD/VBUS/VIN/VBAT/VB/VS, 3V3, 5V, GND...) and every net of a class the designer made wide
    (track width of 0.4 mm or more). They get the least margin to spare, so a relaxation spends nothing on them (`apply(..., keep=...)`)."""
    names = {_name(m.group(1)) for m in re.finditer(r'\n  \(net \d+ (.*)\)(?=\n)', text)}
    out = {n for n in names if n and POWER_NAME.search(n)}
    for a_, b_ in class_blocks(text):
        blk = text[a_:b_]
        w = re.search(r"\(trace_width ([0-9.]+)\)", blk)
        if w and float(w.group(1)) >= 0.4:
            out |= set(class_nets(blk))
    for z in zone_blocks(text):  # a net with a pour is a power or ground net, whatever it is called
        m = re.search(r'\(net_name ("[^"]*"|[^)\s]+)\)', z)
        if m and _name(m.group(1)):
            out.add(_name(m.group(1)))
    if not ground:  # supply rails only: ground nets (their pours carry the current) may take the relaxation
        out = {n for n in out if not re.search(r"GND", n, re.I)}
    return sorted(n for n in out if n in names)


def apply(text: str, t: dict, keep_class_width: bool = True, keep: tuple = (), keep_t: dict | None = None) -> str:
    """Return the board text with the tier's values: setup (trace_clearance, trace_min, via_size, via_drill, via_min_size, via_min_drill)
    and every net class (clearance, via_dia, via_drill). Class track widths stay (the router necks down to trace_min where it must).
    `keep`: nets that take none of it (power): they move to a copy of their class made from the UNTOUCHED text, so they keep the board's own values,
    or, with `keep_t`, take that (milder) tier's clearance and via values instead: power relaxes after the signals, never before."""
    keep = set(keep)
    clones: list[str] = []
    out = text
    if keep:
        def is_kept(l):
            a = ADD_NET.match(l)
            return bool(a) and _name(a.group(1)) in keep

        def split_class(m):
            b = m.group(0)
            lines = b.split("\n")
            if not any(is_kept(l) for l in lines):
                return b
            clone = re.sub(r"\(net_class (\S+)", lambda x: f"(net_class {x.group(1).strip(chr(34))}_keep", b, count=1)
            clones.append("\n".join(l for l in clone.split("\n") if not ADD_NET.match(l) or is_kept(l)))
            return "\n".join(l for l in lines if not is_kept(l))
        out = re.sub(r"\(net_class .*?\n  \)", split_class, out, flags=re.S)

    def setv(name, val, s):
        return re.sub(rf"(\({name} )[0-9.]+(\))", rf"\g<1>{val}\g<2>", s)
    out = setv("trace_clearance", t["clr"], out)
    out = setv("trace_min", t["tmin"], out)
    out = setv("via_size", t["via"], out)
    out = setv("via_drill", t["drill"], out)
    out = setv("via_min_size", t["vmin"], out)
    out = setv("via_min_drill", t["dmin"], out)

    def fix_class(m):
        b = m.group(0)
        b = re.sub(r"(\(clearance )[0-9.]+(\))", rf"\g<1>{t['clr']}\g<2>", b)
        b = re.sub(r"(\(via_dia )[0-9.]+(\))", rf"\g<1>{t['via']}\g<2>", b)
        b = re.sub(r"(\(via_drill )[0-9.]+(\))", rf"\g<1>{t['drill']}\g<2>", b)
        return b
    out = re.sub(r"\(net_class .*?\n  \)", fix_class, out, flags=re.S)
    if clones and keep_t:
        clones = [re.sub(r"\(via_drill [0-9.]+\)", f"(via_drill {keep_t['drill']})", re.sub(r"\(via_dia [0-9.]+\)", f"(via_dia {keep_t['via']})",
                  re.sub(r"\(clearance [0-9.]+\)", f"(clearance {keep_t['clr']})", c))) for c in clones]
    if clones:
        last = list(re.finditer(r"\(net_class .*?\n  \)", out, flags=re.S))[-1]
        out = out[:last.end()] + "".join("\n\n  " + c for c in clones) + out[last.end():]
    return out


def zone_blocks(text: str) -> list[str]:
    """The top-level (zone ...) blocks of a board file, as text."""
    out, i = [], 0
    while True:
        j = text.find("\n  (zone", i)
        if j < 0:
            return out
        depth, k = 0, j + 1
        while True:
            c = text[k]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        out.append(text[j + 1:k + 1].lstrip())
        i = k


def with_designer_zones(unrouted: str, raw: str) -> str:
    """The unrouted board plus the zones the designer drew (the fixture drops them), net numbers checked by name; zones of an unnamed net are left out."""
    import re
    by_name = {m.group(2): int(m.group(1)) for m in re.finditer(r'\n  \(net (\d+) "?([^")\s]*)"?\)', unrouted)}
    add = []
    for z in zone_blocks(raw):
        m = re.search(r'\(net (\d+)\)\s*\(net_name "?([^")\s]*)"?\)', z)
        if not m or not m.group(2) or m.group(2) not in by_name:
            continue  # a pour of an unnamed net, or of a net the unrouted board does not have
        n = by_name[m.group(2)]
        if int(m.group(1)) != n:  # the fixture renumbered its nets: the zone follows the name
            z = z[:m.start(1)] + str(n) + z[m.end(1):]
        add.append(z)
    end = unrouted.rstrip().rfind(")")
    return unrouted[:end].rstrip("\n") + "\n" + "\n".join("  " + z for z in add) + "\n)\n"


def all_nets(text: str) -> list[str]:
    return sorted({_name(m.group(1)) for m in re.finditer(r'\n  \(net \d+ (.*)\)(?=\n)', text)} - {""})


def apply_nets(text: str, assign: dict, tiers: dict, widths: dict | None = None) -> str:
    """Per-net relaxation: `assign` maps a net name to a tier name; those nets move to a copy of their class made from the UNTOUCHED text and take the
    tier's clearance and via values, every other net keeps the board's own rules. The setup block takes the loosest minimums in use (track, via) so the
    router may use them; the rest of the board is shielded from them by `--no-neck-nets`.
    `widths`: net name -> track width in mm, a width the net is routed at (the designer's, or one from a current): the net gets a class of its own with it."""
    widths = widths or {}
    by_tier: dict[tuple, set] = {}
    for n in set(assign) | set(widths):
        by_tier.setdefault((assign.get(n, "T0"), widths.get(n)), set()).add(n)
    if not by_tier:
        return text
    blocks = [(a, b, text[a:b]) for a, b in class_blocks(text)]
    if not blocks:
        raise ValueError("the board file has no net class to copy (rules in a project file?)")
    classname = lambda blk: re.match(r'\(net_class ("[^"]*"|\S+)', blk).group(1)
    in_class: dict[str, str] = {}
    for _, _, blk in blocks:
        for n in class_nets(blk):
            in_class[n] = classname(blk)
    default_name = next((classname(blk) for _, _, blk in blocks if classname(blk).strip('"') == "Default"), classname(blocks[0][2]))
    clones, moved = [], set()
    for (tier, width), nets in sorted(by_tier.items(), key=lambda kv: (kv[0][0], kv[0][1] or 0)):
        t = tiers[tier]
        for _, _, blk in blocks:
            cn = classname(blk)
            mine = [n for n in class_nets(blk) if n in nets]
            extra = [n for n in sorted(nets) if n not in in_class] if cn == default_name else []  # nets with no class of their own are in the default class
            if not mine and not extra:
                continue
            c = re.sub(r'\(net_class ("[^"]*"|\S+)', lambda x: f"(net_class {x.group(1).strip(chr(34))}_{tier}{'_w%d' % round(width * 1000) if width else ''}", blk, count=1)
            c = ADD_NET_ANY.sub("", c).rstrip()
            assert c.endswith(")")
            c = c[:-1].rstrip() + "".join(f'\n    (add_net "{n}")' for n in mine + extra) + ")"
            if tier != "T0":  # the board's own values stay as the class has them
                c = re.sub(r"(\(clearance )[0-9.]+(\))", rf"\g<1>{t['clr']}\g<2>", c)
                c = re.sub(r"(\(via_dia )[0-9.]+(\))", rf"\g<1>{t['via']}\g<2>", c)
                c = re.sub(r"(\(via_drill )[0-9.]+(\))", rf"\g<1>{t['drill']}\g<2>", c)
            if width:
                c = re.sub(r"(\(trace_width )[0-9.]+(\))", rf"\g<1>{width}\g<2>", c)
            clones.append(c)
            moved |= set(mine)
    out = text
    if moved:
        for a, b, blk in reversed(blocks):
            new_blk = ADD_NET_ANY.sub(lambda m: "" if _name(m.group(1)) in moved else m.group(0), blk)
            out = out[:a] + new_blk + out[b:]
    used = [tiers[t] for t, _ in by_tier if t != "T0"] or [tiers["T0"]]
    for name, field in (("trace_clearance", "clr"), ("trace_min", "tmin"), ("via_min_size", "vmin"), ("via_min_drill", "dmin")):
        out = re.sub(rf"(\({name} )[0-9.]+(\))", rf"\g<1>{min(u[field] for u in used)}\g<2>", out)
    last_end = class_blocks(out)[-1][1]
    return out[:last_end] + "".join("\n\n  " + c for c in clones) + out[last_end:]


def plane_layers(raw: str, board_area_mm2: float) -> list[str]:
    """Inner copper layers that are a plane: all pours of the layer are of ONE net and cover at least 60 % of the board's bounding box. A layer with several pours
    (split planes: VCC and VDD_RAM on one layer) or little copper still has room for signals, and the designer routes on it (Own-Mailbox: 114 tracks on In2)."""
    import re
    per: dict[str, list] = {}
    for z in zone_blocks(raw):
        m = re.search(r"\(layer (In\d+\.Cu)\)", z) or re.search(r"\(layers[^)]*?(In\d+\.Cu)", z)
        if not m:
            continue
        net = re.search(r'\(net_name ("[^"]*"|[^)\s]+)\)', z)
        area = 0.0
        for poly in re.finditer(r"\(polygon\s*\(pts\s+((?:\(xy [-0-9.]+ [-0-9.]+\)\s*)+)\)", z):
            pts = [(float(a), float(b)) for a, b in re.findall(r"\(xy ([-0-9.]+) ([-0-9.]+)\)", poly.group(1))]
            area += abs(sum(pts[i][0] * pts[(i + 1) % len(pts)][1] - pts[(i + 1) % len(pts)][0] * pts[i][1] for i in range(len(pts)))) / 2
        per.setdefault(m.group(1), []).append((net.group(1) if net else "", area))
    out = []
    for layer, items in sorted(per.items()):
        nets = {n for n, _ in items}
        if len(nets) == 1 and sum(a for _, a in items) >= 0.6 * board_area_mm2:
            out.append(layer)
    return out

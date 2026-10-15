#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read-only KiCad 10 routing preflight; all dimensions in the report are mm."""

import argparse
from collections import Counter, defaultdict
from decimal import Decimal, ROUND_HALF_UP
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile


def flat(text):
    """One printable line: names from board files must not be able to start a line of the report."""
    return ' '.join(''.join(c if c.isprintable() else ' ' for c in str(text)).split())


def sexprs(text):
    """Small iterative reader: quoted strings and comments cannot close a list."""
    root, stack = [], []
    current = root
    i = 0
    while i < len(text):
        c = text[i]
        if c.isspace():
            i += 1
        elif c in '#;':
            end = text.find('\n', i)
            i = len(text) if end < 0 else end + 1
        elif c == '(':
            node = []
            current.append(node)
            stack.append(current)
            current = node
            i += 1
        elif c == ')':
            if not stack:
                raise ValueError('Unexpected closing parenthesis')
            current = stack.pop()
            i += 1
        elif c == '"':
            i += 1
            value = []
            while i < len(text) and text[i] != '"':
                if text[i] == '\\':
                    i += 1
                    if i >= len(text):
                        raise ValueError('Unterminated string escape')
                    value.append({'n': '\n', 'r': '\r', 't': '\t'}.get(text[i], text[i]))
                else:
                    value.append(text[i])
                i += 1
            if i == len(text):
                raise ValueError('Unterminated string')
            current.append(''.join(value))
            i += 1
        else:
            end = i
            while end < len(text) and not text[end].isspace() and text[end] not in '()':
                end += 1
            current.append(text[i:end])
            i = end
    if stack:
        raise ValueError('Unclosed parenthesis')
    return root


def children(node, head):
    return [n for n in node if isinstance(n, list) and n and n[0] == head]


def child(node, head):
    return next(iter(children(node, head)), [])


def val(node, head, default=None):
    n = child(node, head)
    return n[1] if len(n) > 1 and not isinstance(n[1], list) else default


def number(node, head, default=0):
    return float(val(node, head, default))


def net_of(node, nets):
    """Net name of a pad or zone. KiCad 10 writes (net "NAME") and has no net table; older files write
    (net N "NAME") on pads and (net N) elsewhere, with a table of (net N "NAME") at the top."""
    n = child(node, 'net')
    if len(n) >= 3:
        return n[2]
    if len(n) == 2 and not isinstance(n[1], list):
        return nets.get(n[1], '' if nets else n[1])
    return ''


def locked(node):
    return 'locked' in node or bool(child(node, 'locked')) and val(node, 'locked') not in ('no', 'false')


def xy(node, head):
    n = child(node, head)
    return tuple(map(float, n[1:3])) if len(n) >= 3 else (0.0, 0.0)


def points(node):
    return [tuple(map(float, p[1:3])) for p in children(child(node, 'pts'), 'xy')]


def arc_bounds(a, mid, b):
    ax, ay = a
    mx, my = mid
    bx, by = b
    determinant = 2 * (ax * (my - by) + mx * (by - ay) + bx * (ay - my))
    if abs(determinant) < 1e-12:
        return [a, mid, b]
    aa, mm, bb = ax * ax + ay * ay, mx * mx + my * my, bx * bx + by * by
    cx = (aa * (my - by) + mm * (by - ay) + bb * (ay - my)) / determinant
    cy = (aa * (bx - mx) + mm * (ax - bx) + bb * (mx - ax)) / determinant
    start, middle, end = [math.atan2(p[1] - cy, p[0] - cx) for p in (a, mid, b)]
    span = (end - start) % math.tau
    ccw = (middle - start) % math.tau <= span
    radius = math.hypot(ax - cx, ay - cy)
    result = [a, mid, b]
    for angle in (0, math.pi / 2, math.pi, 3 * math.pi / 2):
        inside = ((angle - start) % math.tau <= span if ccw
                  else (start - angle) % math.tau <= (start - end) % math.tau)
        if inside:
            result.append((cx + radius * math.cos(angle), cy + radius * math.sin(angle)))
    return result


def curve_bounds(control):
    if len(control) != 4:
        return control
    times = {0.0, 1.0}
    for axis in (0, 1):
        p, q, r, s = [v[axis] for v in control]
        a, b, c = -p + 3 * q - 3 * r + s, 2 * (p - 2 * q + r), q - p
        if abs(a) < 1e-12:
            roots = [-c / b] if abs(b) >= 1e-12 else []
        elif b * b >= 4 * a * c:
            d = math.sqrt(b * b - 4 * a * c)
            roots = [(-b + d) / (2 * a), (-b - d) / (2 * a)]
        else:
            roots = []
        times.update(t for t in roots if 0 < t < 1)
    return [tuple((1 - t) ** 3 * control[0][axis] + 3 * (1 - t) ** 2 * t * control[1][axis]
                  + 3 * (1 - t) * t * t * control[2][axis] + t ** 3 * control[3][axis]
                  for axis in (0, 1)) for t in sorted(times)]


def pad_bounds(pad):
    width, height = xy(pad, 'size')
    bounds = [(-width / 2, -height / 2), (width / 2, height / 2)]
    for n in child(pad, 'primitives')[1:]:
        if not isinstance(n, list) or not n:
            continue
        kind = n[0]
        if kind == 'gr_circle':
            c, e = xy(n, 'center'), xy(n, 'end')
            radius = math.dist(c, e)
            vertices = [(c[0] - radius, c[1] - radius), (c[0] + radius, c[1] + radius)]
        elif kind == 'gr_arc':
            vertices = arc_bounds(xy(n, 'start'), xy(n, 'mid'), xy(n, 'end'))
        elif kind in ('gr_poly', 'gr_curve'):
            vertices = points(n)
            if kind == 'gr_curve':
                vertices = curve_bounds(vertices)
        elif kind in ('gr_line', 'gr_rect'):
            vertices = [xy(n, 'start'), xy(n, 'end')]
        else:
            raise ValueError('Unsupported custom pad primitive: ' + kind)
        stroke = number(n, 'width', number(child(n, 'stroke'), 'width')) / 2
        for x, y in vertices:
            bounds.extend(((x - stroke, y - stroke), (x + stroke, y + stroke)))
    return (min(p[0] for p in bounds), min(p[1] for p in bounds),
            max(p[0] for p in bounds), max(p[1] for p in bounds))


def area(poly):
    return abs(sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(poly, poly[1:] + poly[:1]))) / 2


def nm(mm):
    return int((Decimal(str(mm)) * 1000000).quantize(Decimal(1), rounding=ROUND_HALF_UP))


def pitch(width, clearance):
    # Integer division matches the engine before its 5 µm quantisation.
    return min(100000, max(25000, ((nm(width) + nm(clearance)) // 6 // 5000) * 5000)) / 1000000


class Condition:
    """Parse the engine's small condition grammar, not KiCad's full evaluator."""
    def __init__(self, text):
        self.text, self.i, self.refs = text, 0, set()

    def skip(self):
        while self.i < len(self.text) and self.text[self.i].isspace():
            self.i += 1

    def eat(self, token):
        self.skip()
        if self.text.startswith(token, self.i):
            self.i += len(token)
            return True
        return False

    def string(self):
        quote = self.text[self.i]
        self.i += 1
        end = self.text.find(quote, self.i)
        if end < 0:
            raise ValueError('unterminated condition string')
        self.i = end + 1

    def term(self):
        self.skip()
        if self.i == len(self.text):
            raise ValueError('unexpected end of condition')
        if self.eat('('):
            self.expression()
            if not self.eat(')'):
                raise ValueError("missing ')' in condition")
        elif self.text[self.i] in "'\"":
            self.string()
        elif self.text[self.i] in '+-.0123456789':
            m = re.match(r'[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?', self.text[self.i:])
            if not m:
                raise ValueError('invalid condition number')
            self.i += len(m[0])
            self.skip()
            unit = re.match(r'[A-Za-z]*', self.text[self.i:])[0]
            if unit not in ('', 'mm', 'mil', 'in', 'ps', 'deg', 'fs'):
                raise ValueError('unsupported condition unit ' + unit)
            self.i += len(unit)
        else:
            m = re.match(r'[A-Za-z_][A-Za-z_0-9.]*', self.text[self.i:])
            if not m:
                raise ValueError('unsupported condition syntax')
            name = m[0]
            self.i += len(name)
            if name not in ('true', 'false'):
                self.refs.add(name[2:] if name.startswith(('A.', 'B.')) else name)
                if self.eat('('):
                    while not self.eat(')'):
                        self.skip()
                        if self.i == len(self.text) or self.text[self.i] not in "'\"":
                            raise ValueError('function arguments must be strings')
                        self.string()
                        self.eat(',')

    def unary(self):
        self.skip()
        if self.text.startswith('!', self.i) and not self.text.startswith('!=', self.i):
            self.i += 1
            self.unary()
        else:
            self.term()
            for token in ('==', '!=', '<=', '>=', '<', '>'):
                if self.eat(token):
                    self.term()
                    break

    def conjunction(self):
        self.unary()
        while self.eat('&&'):
            self.unary()

    def expression(self):
        self.conjunction()
        while self.eat('||'):
            self.conjunction()

    def parse(self):
        if self.text:
            self.expression()
            self.skip()
            if self.i != len(self.text):
                raise ValueError('unsupported condition syntax at ' + self.text[self.i:self.i + 10])
        return self.refs


def outline(board):
    shapes = [n for n in board if isinstance(n, list) and n and n[0].startswith('gr_')
              and val(n, 'layer') == 'Edge.Cuts']
    edges, polygons, bounds = [], [], []
    curved, zero_length = False, 0
    for n in shapes:
        kind = n[0]
        if kind in ('gr_line', 'gr_arc'):
            a, b = xy(n, 'start'), xy(n, 'end')
            mid = xy(n, 'mid') if child(n, 'mid') else None
            if kind == 'gr_arc' and mid is None and child(n, 'angle'):
                # KiCad 5 arcs: start is the centre, end the first point, and the arc turns by `angle` degrees.
                centre, turn = a, math.radians(number(n, 'angle'))
                rotate = lambda p, t: (centre[0] + (p[0] - centre[0]) * math.cos(t) - (p[1] - centre[1]) * math.sin(t),
                                       centre[1] + (p[0] - centre[0]) * math.sin(t) + (p[1] - centre[1]) * math.cos(t))
                a, mid, b = b, rotate(b, turn / 2), rotate(b, turn)
            edges.append((a, b))
            bounds.extend((a, b))
            zero_length += kind == 'gr_line' and math.dist(a, b) < 1e-6
            if kind == 'gr_arc':
                curved = True
                bounds.extend(arc_bounds(a, mid or a, b))
        elif kind == 'gr_rect':
            a, b = xy(n, 'start'), xy(n, 'end')
            p = [a, (b[0], a[1]), b, (a[0], b[1])]
            polygons.append(p)
            bounds.extend(p)
        elif kind == 'gr_poly':
            p = points(n)
            if len(p) >= 3:
                polygons.append(p)
                bounds.extend(p)
        elif kind == 'gr_circle':
            a, b = xy(n, 'center'), xy(n, 'end')
            r = math.dist(a, b)
            bounds.extend(((a[0] - r, a[1] - r), (a[0] + r, a[1] + r)))
            polygons.append([])
            curved = True
    # KiCad 10.0.6 chains outline segments whose ends lie within 0.01 mm of each other (PCBench: 28 outlines with
    # gaps of 1.2 to 10 µm pass its DRC, none above), so ends that close are merged before the loops are followed.
    tolerance = 0.01 + 1e-9
    cells, parent = defaultdict(list), {}

    def key(p):
        while parent[p] != p:
            parent[p] = parent[parent[p]]
            p = parent[p]
        return p

    for p in sorted({p for edge in edges for p in edge}):
        parent[p] = p
        cx, cy = math.floor(p[0] / tolerance), math.floor(p[1] / tolerance)
        for q in [q for dx in (-1, 0, 1) for dy in (-1, 0, 1) for q in cells[cx + dx, cy + dy]]:
            if math.dist(p, q) <= tolerance:
                parent[key(q)] = key(p)
        cells[cx, cy].append(p)
    graph = defaultdict(list)
    for index, (a, b) in enumerate(edges):
        if key(a) != key(b):  # a stub shorter than the tolerance is not a side of the outline
            graph[key(a)].append((key(b), index))
            graph[key(b)].append((key(a), index))
    seen, loops = set(), []
    for start in sorted(graph):
        if start in seen:
            continue
        component, pending = set(), [start]
        while pending:
            p = pending.pop()
            if p in component:
                continue
            component.add(p)
            pending.extend(q for q, _ in graph[p] if q not in component)
        seen.update(component)
        if len(component) >= 2 and all(len(graph[p]) == 2 for p in component):
            loop, p, previous_edge = [], start, -1
            while True:
                loop.append(p)
                q, edge = next((q, e) for q, e in graph[p] if e != previous_edge)
                p, previous_edge = q, edge
                if p == start:
                    break
            loops.append(loop)
    bbox = (min(p[0] for p in bounds), min(p[1] for p in bounds),
            max(p[0] for p in bounds), max(p[1] for p in bounds)) if bounds else None
    simple = not curved and len(polygons) + len(loops) == 1
    polygon_area = area((polygons + loops)[0]) if simple else 0
    # Footprints can carry part of the outline (edge connectors, board templates); those shapes are not followed.
    in_footprints = sum(1 for fp in children(board, 'footprint') + children(board, 'module') for n in fp
                        if isinstance(n, list) and n and isinstance(n[0], str) and n[0].startswith('fp_')
                        and val(n, 'layer') == 'Edge.Cuts')
    return {'present': bool(shapes), 'closed': bool(polygons or loops), 'bbox': bbox, 'in_footprints': in_footprints,
            'zero_length': zero_length,
            'area_mm2': polygon_area or ((bbox[2] - bbox[0]) * (bbox[3] - bbox[1]) if bbox else 0),
            'area_method': 'simple outline polygon' if polygon_area else 'outline bbox approximation'}


def class_for(net, classes, settings):
    by_name = {c['name']: c for c in classes}
    names = (settings.get('netclass_assignments') or {}).get(net, []) or []
    if isinstance(names, str):
        names = [names]
    candidates = [by_name[n] for n in names if n in by_name]
    if candidates:
        return candidates[0]
    for p in settings.get('netclass_patterns', []):
        pattern = p.get('pattern', '')
        wildcard = re.escape(pattern).replace(r'\*', '.*').replace(r'\?', '.')
        match = re.fullmatch(wildcard, net, re.S) is not None
        if not match:
            try:
                match = re.fullmatch(pattern, net) is not None
            except re.error:
                pass
        if match and p.get('netclass') in by_name:
            return by_name[p['netclass']]
    return classes[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('board', type=Path)
    parser.add_argument('--tracemaker')
    parser.add_argument('--json', type=Path)
    parser.add_argument('--small-pad-mm', type=float, default=2)
    parser.add_argument('--soft-zones', action='store_true',
                        help='suggest --soft-zones (and analyse escape with it) even without a plane, e.g. for stale pours after deleting routing')
    args = parser.parse_args()
    if not math.isfinite(args.small_pad_mm) or args.small_pad_mm <= 0:
        parser.error('--small-pad-mm must be positive and finite')
    if not str(args.board).isprintable():
        parser.error('board path contains control characters; rename the file')
    report = {'board': str(args.board), 'findings': []}
    sections = defaultdict(list)

    def finding(section, severity, text):
        # Net, rule, class and footprint names are the board author's text: a line break in one could forge a
        # finding or a command block for the agent reading the report.
        text = flat(text)
        sections[section].append(f'- **{severity}**: {text}')
        report['findings'].append({'section': section, 'severity': severity, 'message': text})

    try:
        expressions = sexprs(args.board.read_text())
        if len(expressions) != 1 or expressions[0][0] != 'kicad_pcb':
            raise ValueError('Not a KiCad PCB s-expression')
        board = expressions[0]
        project_path = args.board.with_suffix('.kicad_pro')
        project = json.loads(project_path.read_text()) if project_path.exists() else {}
        finding('Project and lattice', 'info' if project else 'block',
                f'Project: {project_path}' if project else 'Missing sibling .kicad_pro: restore the matching project before routing; fallback rules are not design intent.')
        settings = project.get('net_settings', {})
        minimums = project.get('board', {}).get('design_settings', {}).get('rules', {})
        if not project:
            # As the engine (read_design_rules): KiCad's built-in minimums, then the rules a KiCad 5 board stores
            # in itself, (setup (trace_min ...)) and (net_class NAME "descr" (clearance) (trace_width) ... (add_net)).
            minimums = dict(min_via_diameter=0.4, min_through_hole_diameter=0.3,
                            min_via_annular_width=0.1, min_hole_clearance=0.25,
                            min_hole_to_hole=0.25, min_copper_edge_clearance=0.5,
                            min_microvia_diameter=0.2, min_microvia_drill=0.1)
            setup = child(board, 'setup')
            for old, new in (('trace_min', 'min_track_width'), ('via_min_size', 'min_via_diameter'),
                             ('via_min_drill', 'min_through_hole_diameter'), ('uvia_min_size', 'min_microvia_diameter'),
                             ('uvia_min_drill', 'min_microvia_drill'), ('edge_clearance', 'min_copper_edge_clearance')):
                if val(setup, old) is not None:
                    minimums[new] = number(setup, old)
            for old, new in (('blind_buried_vias_allowed', 'allow_blind_buried_vias'), ('uvias_allowed', 'allow_microvias')):
                if val(setup, old) is not None:
                    minimums[new] = val(setup, old) == 'yes'
            legacy = [n for n in children(board, 'net_class') if len(n) > 1 and not isinstance(n[1], list)]
            fields = (('clearance', 'clearance'), ('trace_width', 'track_width'), ('via_dia', 'via_diameter'), ('via_drill', 'via_drill'))
            settings = dict(classes=[dict({new: number(n, old) for old, new in fields if val(n, old) is not None}, name=n[1])
                                     for n in legacy],
                            netclass_assignments={a[1]: n[1] for n in legacy for a in children(n, 'add_net') if len(a) > 1})
            if legacy:
                finding('Project and lattice', 'info', f'{len(legacy)} net classes and the minimums below are the ones stored in this KiCad 5 board, as TraceMaker reads them without a project.')
        elif not minimums:
            finding('Project and lattice', 'block', 'Project has no board design rules; TraceMaker uses zero minimums.')
        for key in sorted(minimums):
            if key.startswith('min_'):
                finding('Project and lattice', 'info', f'{key} = {minimums[key]}' + (' spokes' if key == 'min_resolved_spokes' else ' mm'))
        defaults = dict(name='Default', priority=2147483647, track_width=0.25, clearance=0.2, via_diameter=0.6, via_drill=0.3)
        source_classes = settings.get('classes', [])
        defaults.update({k: v for c in source_classes if c.get('name') == 'Default' for k, v in c.items() if v is not None})
        classes = [defaults]
        classes.extend(dict(defaults, **dict({k: v for k, v in c.items() if v is not None},
                                             priority=c.get('priority') if c.get('priority') is not None else 0))
                       for c in source_classes if c.get('name') not in (None, '', 'Default'))
        nets = {n[1]: n[2] for n in children(board, 'net') if len(n) >= 3}
        # KiCad 5 called footprints modules.
        footprints = children(board, 'footprint') + children(board, 'module')
        net_names = sorted({n for n in nets.values() if n}
                           | {net_of(pad, nets) for fp in footprints for pad in children(fp, 'pad')} - {''})
        used = Counter(class_for(n, classes, settings)['name'] for n in net_names)
        for c in classes:
            c['effective_track'] = max(c['track_width'], minimums.get('min_track_width', 0))
            c['effective_clearance'] = max(c['clearance'], minimums.get('min_clearance', 0))
            c['pitch_mm'] = pitch(c['effective_track'], c['effective_clearance'])
        report['minimums'], report['net_classes'] = minimums, classes
        sections['Project and lattice'].extend(['', '| Class | Nets | Effective track | Effective clearance | Implied pitch |',
                                                 '|---|---:|---:|---:|---:|'])
        for c in classes:
            sections['Project and lattice'].append(f"| {flat(c['name']).replace('|', '/')} | {used[c['name']]} | {c['effective_track']:g} | {c['effective_clearance']:g} | {c['pitch_mm']:g} |")
        finest = min(c['pitch_mm'] for c in classes)
        setters = [c['name'] for c in classes if c['pitch_mm'] == finest]
        report['pitch_mm'] = finest
        finding('Project and lattice', 'info', f"Router pitch: {finest:g} mm, set by {', '.join(setters)}. ALL defined classes count, even unused ones.")
        for c in classes:
            others = [k['pitch_mm'] for k in classes if k is not c]
            if others and c['pitch_mm'] < 0.75 * min(others):
                finding('Project and lattice', 'slow', f"Class {c['name']} has pitch {c['pitch_mm']:g} mm vs next finest {min(others):g} mm; only keep if used ({used[c['name']]} nets currently resolve to it).")
        finding('Project and lattice', 'info', f"netclass_patterns: {len(settings.get('netclass_patterns', []))}; resolved once per net, not in the routing inner loop.")

        # Engine diagnostics are the authority on which rules TraceMaker applies (drc::RuleEngine, KiCad 10.0.3
        # parity): run `tracemaker drc` once, here, and reuse its output below.
        binary = shutil.which(args.tracemaker or os.environ.get('TRACEMAKER', 'tracemaker'))
        drc_text, drc_code = None, None
        if binary:
            try:
                p = subprocess.run([binary, 'drc', str(args.board)], capture_output=True, text=True, timeout=600)
                drc_text, drc_code = p.stdout + '\n' + p.stderr, p.returncode
            except (OSError, subprocess.TimeoutExpired) as exc:
                finding('Custom rules', 'info', f'TraceMaker DRC unavailable: {exc}')
        # One entry per warning, flattened: a rule name may hold line breaks.
        engine_warnings = [flat(w) for w in re.split(r'^(?=warning: )', drc_text or '', flags=re.M) if w.startswith('warning: ')]
        report['drc_warnings'] = engine_warnings
        rules_path = args.board.with_suffix('.kicad_dru')
        rules = []
        if rules_path.exists():
            try:
                rules = children(sexprs(rules_path.read_text()), 'rule')
            except ValueError as exc:
                finding('Custom rules', 'block', f'Cannot parse {rules_path}: {exc}; custom rules would be ignored.')
        if not rules:
            finding('Custom rules', 'info', 'No parsed custom rules.')
        allowed = {'track', 'via', 'through_via', 'micro_via', 'buried_via', 'blind_via', 'pad', 'zone', 'graphic'}
        # The engine's vocabulary. Any other name (e.g. Parent.Reference, memberOfGroup, insideCourtyard) drops the
        # whole rule in the router and in `tracemaker drc`; KiCad still applies it.
        evaluated = {'NetClass', 'NetName', 'Type', 'Layer', 'L', 'Reference', 'Pad_Type', 'Width', 'Size_X', 'Size_Y',
                     'Position_X', 'Position_Y', 'isPlated', 'existsOnLayer', 'insideArea', 'intersectsArea',
                     'enclosedByArea', 'inDiffPair', 'memberOfFootprint', 'intersectsCourtyard',
                     'intersectsFrontCourtyard', 'intersectsBackCourtyard'}
        positional = {'insideArea', 'intersectsArea', 'enclosedByArea', 'memberOfFootprint', 'intersectsCourtyard',
                      'intersectsFrontCourtyard', 'intersectsBackCourtyard', 'Reference', 'Pad_Type', 'Width',
                      'Size_X', 'Size_Y', 'Position_X', 'Position_Y'}
        caches_disabled = False
        for rule in rules:
            name, condition = rule[1], val(rule, 'condition', '')
            constraints = children(rule, 'constraint')
            kinds = [c[1] for c in constraints]
            said = [w for w in engine_warnings if flat(f"rule '{name}'") in w]
            dropped = [w for w in said if 'rule ignored' in w or 'rule not applied' in w]
            reasons, refs, parsed = [], set(), True
            try:
                refs = Condition(condition).parse()
            except ValueError as exc:
                parsed = False
                if drc_text is None:
                    dropped.append(f'cannot parse condition ({exc}) [preflight parser; engine not run]')
            if drc_text is None and parsed and refs - evaluated:
                dropped.append(f"uses {', '.join(sorted(refs - evaluated))} [preflight vocabulary; engine not run]")
            for w in dropped:
                finding('Custom rules', 'block', f"Rule '{name}' is not applied by TraceMaker (router and `tracemaker drc`): {w}. "
                        'KiCad still enforces it: fix the condition, or route and fix its violations by hand.')
            # The engine keeps a rule that calls a KiCad function it cannot evaluate, with the call taken as false: the
            # rule then misses whatever only that call selects, and KiCad still reports those items.
            partial = [w for w in said if w not in dropped and 'KiCad evaluates and TraceMaker does not' in w]
            for w in partial:
                finding('Custom rules', 'block', f"Rule '{name}' is not applied by TraceMaker the way KiCad applies it: {w}. "
                        'KiCad still enforces it in full: rewrite the condition without that function, or route and fix its '
                        'violations by hand.')
            if dropped:
                reasons.append('rule not applied')
            for kind in kinds:
                if kind not in ('disallow', 'physical_hole_clearance'):
                    reasons.append('constraint ' + kind)
                elif kind == 'physical_hole_clearance' and refs & {'NetName', 'NetClass', 'inDiffPair'}:
                    reasons.append('net-dependent physical_hole_clearance')
            caches_disabled |= bool(reasons)
            finding('Custom rules', 'slow' if reasons else 'info',
                    f"Rule '{name}': {', '.join(kinds) or 'no constraints'}; condition `{condition or 'true'}`; "
                    + ('disables the per-class caches board-wide (' + '; '.join(reasons) + ')' if reasons else 'cache-ok'))
            routed_items = {w for c in constraints if c[1] == 'disallow' for w in c[2:] if isinstance(w, str)} & (allowed - {'pad', 'zone', 'graphic'})
            if routed_items and refs & positional and not dropped:
                finding('Custom rules', 'info', f"Rule '{name}': position/size/footprint disallow, enforced on every new track and via; "
                        'the search checks it at each lattice point it visits (some extra time, caches kept).')
            for c in constraints:
                if c[1] == 'disallow':
                    unsupported = [w for w in c[2:] if isinstance(w, str) and w not in allowed]
                    if unsupported:
                        finding('Custom rules', 'info', f"Rule '{name}': disallow {', '.join(unsupported)} is not checked by TraceMaker; KiCad DRC still reports it.")
        report['caches_disabled'] = caches_disabled

        for c in classes:
            drill = max(c['via_drill'], minimums.get('min_through_hole_diameter', 0))
            diameter = max(c['via_diameter'], minimums.get('min_via_diameter', 0), drill + 2 * minimums.get('min_via_annular_width', 0))
            changed = nm(diameter) > nm(c['via_diameter']) or nm(drill) > nm(c['via_drill'])
            finding('Vias', 'quality' if changed else 'info',
                    f"{c['name']}: class diameter/drill {c['via_diameter']:g}/{c['via_drill']:g} → effective {diameter:g}/{drill:g} mm"
                    + ('; board minimum overrides class (larger vias cost routing space).' if changed else '.'))
        finding('Vias', 'info', f"Blind/buried permitted: {minimums.get('allow_blind_buried_vias', False)}; microvias permitted: {minimums.get('allow_microvias', False)}.")

        out = outline(board)
        report['outline'] = out
        if out['zero_length']:
            finding('Outline and placement', 'block', f"{out['zero_length']} Edge.Cuts lines of zero length: KiCad's DRC calls the outline malformed (invalid_outline) and skips its edge checks. Delete them.")
        finding('Outline and placement', 'info' if out['closed'] or out['in_footprints'] else 'block',
                f"Edge.Cuts present: {out['present']}; closed loop detected: {out['closed']}; area {out['area_mm2']:g} mm² ({out['area_method']}). Closure/bounds are approximate; KiCad DRC is authoritative."
                + (f" {out['in_footprints']} Edge.Cuts shapes are drawn in footprints and not followed here: check the outline with KiCad's DRC (invalid_outline)."
                   if out['in_footprints'] and not out['closed'] else ''))
        copper_layers = [n[1] for n in child(board, 'layers')[1:] if isinstance(n, list) and len(n) > 1 and n[1].endswith('.Cu')]
        planes, zone_details = [], []
        for index, z in enumerate(children(board, 'zone'), 1):
            keepout = child(z, 'keepout')
            if keepout:
                flags = ', '.join(f'{n[0]}={n[1]}' for n in keepout[1:] if isinstance(n, list) and len(n) > 1)
                finding('Zones', 'info', f'Rule area {index}: {flags}; layers {val(z, "layer", child(z, "layers")[1:])}.')
                continue
            layers = child(z, 'layers')[1:] or [val(z, 'layer', '')]
            layers = copper_layers if '*.Cu' in layers else [l for l in layers if l.endswith('.Cu')]
            if not layers:
                continue
            # KiCad 10 writes the zone's net by name, older files by number with a net_name.
            raw = val(z, 'net', '')
            net = val(z, 'net_name', nets.get(raw, '' if str(raw).isdigit() else raw))
            fill = child(z, 'fill')
            bridge, gap = number(fill, 'thermal_bridge_width'), number(fill, 'thermal_gap')
            mode = val(z, 'connect_pads', 'thermal (default)')
            polygons = [points(p) for p in children(z, 'polygon')]
            zone_area = sum(area(p) for p in polygons)
            fraction = zone_area / out['area_mm2'] if out['area_mm2'] else 0
            filled_layers = {val(p, 'layer', layers[0]) for p in children(z, 'filled_polygon')}
            filled = bool(filled_layers) and all(l in filled_layers for l in layers)
            teardrop = bool(child(child(z, 'attr'), 'teardrop'))
            plane = bool(net) and fraction >= 0.25 and not teardrop
            detail = dict(index=index, net=net, layers=layers, priority=number(z, 'priority'), filled=filled,
                          connect_pads=mode, thermal_gap=gap, thermal_bridge_width=bridge,
                          island_removal_mode=val(fill, 'island_removal_mode', 'default'),
                          island_area_min=val(fill, 'island_area_min', 'default'),
                          area_fraction=fraction, plane=plane, teardrop=teardrop)
            zone_details.append(detail)
            if plane:
                planes.append(detail)
            finding('Zones', 'info', f"Zone {index}: net {net or '(none)'}, layers {', '.join(layers)}, priority {detail['priority']:g}, filled={filled}, connect_pads={mode}, thermal gap/bridge={gap:g}/{bridge:g}, island removal={detail['island_removal_mode']}, island area min={detail['island_area_min']}; outline-area ratio {fraction:.1%} per layer" + ('; PLANE.' if plane else '.'))
            if not filled:
                finding('Zones', 'block', f'Zone {index} unfilled on one or more layers: refill in KiCad before routing so connectivity is current.')
            if teardrop:
                finding('Zones', 'block', f'Zone {index} is a teardrop: Edit → Remove Teardrops before stripping tracks/routing; regenerate after routing.')
            if not net:
                finding('Zones', 'info', f'Zone {index} has no net; not a conductive routing plane.')
            width = class_for(net, classes, settings)['track_width']
            spokes = minimums.get('min_resolved_spokes', 0)
            narrow = bridge < width or spokes >= 2 and bridge <= width
            if narrow:
                finding('Zones', 'quality', f'Zone {index}: starved-thermal risk if thermal relief is used (bridge {bridge:g}, class track {width:g}, min_resolved_spokes {spokes}, connect_pads={mode}; pads can override the zone mode). Narrow means bridge ≤ class track width for the spoke-count heuristic. Router does not model thermal spoke starvation. Refill and judge in KiCad.')
        report['zones'], report['planes'] = zone_details, len(planes)
        if not zone_details:
            finding('Zones', 'info', 'No conductive zones.')
        finding('Zones', 'info', 'Plane classification uses zone outline areas, not clipped/refilled copper; overlaps, cutouts and clearance subtraction can change actual coverage.')

        routed = 0
        for kind in ('segment', 'arc', 'via'):
            items = children(board, kind)
            count = sum(locked(n) for n in items)
            routed += len(items)
            finding('Existing copper', 'info', f'{kind}: {len(items)} total, {count} locked, {len(items) - count} unlocked.')
        refs_locked = []
        small, pads_by_net = 0, defaultdict(set)
        for fp in footprints:
            ref = next((p[2] for p in children(fp, 'property') if p[1] == 'Reference'),
                       next((t[2] for t in children(fp, 'fp_text') if t[1] == 'reference'), '?'))
            if locked(fp):
                refs_locked.append(ref)
            at = child(fp, 'at')
            origin = xy(fp, 'at')
            angle = math.radians(float(at[3]) if len(at) > 3 else 0)
            pad_boxes = []
            for pad in children(fp, 'pad'):
                layers = child(pad, 'layers')[1:]
                if not any(l.endswith('.Cu') for l in layers):
                    continue
                local_bounds = pad_bounds(pad)
                size = (local_bounds[2] - local_bounds[0], local_bounds[3] - local_bounds[1])
                if pad[2] == 'smd' and size[0] < args.small_pad_mm and size[1] < args.small_pad_mm:
                    small += 1
                net = net_of(pad, nets)
                if net:
                    pads_by_net[net].add((ref, pad[1]))
                p = xy(pad, 'at')
                x = origin[0] + p[0] * math.cos(angle) + p[1] * math.sin(angle)
                y = origin[1] - p[0] * math.sin(angle) + p[1] * math.cos(angle)
                # Pad angles in the board are absolute, unlike their positions.
                pa = child(pad, 'at')
                rotation = math.radians(float(pa[3]) if len(pa) > 3 else 0)
                corners = [(lx, ly) for lx in (local_bounds[0], local_bounds[2])
                           for ly in (local_bounds[1], local_bounds[3])]
                world = [(x + lx * math.cos(rotation) + ly * math.sin(rotation),
                          y - lx * math.sin(rotation) + ly * math.cos(rotation)) for lx, ly in corners]
                pad_boxes.append((min(p[0] for p in world), min(p[1] for p in world),
                                  max(p[0] for p in world), max(p[1] for p in world)))
            bbox = out['bbox']
            if bbox and pad_boxes and all(b[2] < bbox[0] or b[0] > bbox[2] or b[3] < bbox[1] or b[1] > bbox[3] for b in pad_boxes):
                finding('Outline and placement', 'info', f'{ref}: all copper pads entirely outside outline bbox; treated as unplaced (conservative rotated pad bounds).')
        finding('Existing copper', 'info', f"Locked footprints: {len(refs_locked)}" + (': ' + ', '.join(sorted(refs_locked)) if refs_locked else '.'))
        finding('Existing copper', 'info', 'Locked items are never moved/ripped. Existing unlocked routing is kept and routed around; rip-up may move it.')
        finding('Pads', 'quality' if small and planes else 'info', f'{small} SMD copper pads below {args.small_pad_mm:g} mm in BOTH dimensions (paste-only ignored).'
                + (' Use --keep-vias-off-pads with the plane strategy.' if small and planes else ' --keep-vias-off-pads matters when using a plane strategy.'))
        report['small_smd_pads'] = small

        names = set(net_names)
        pairs = sorted((n, n[:-1] + partner) for n in net_names for suffix, partner in (('P', 'N'), ('+', '-'))
                       if n.endswith(suffix) and len(n) >= 2 and n[:-1] + partner in names)
        report['diff_pairs'] = pairs
        finding('Differential pairs', 'info', f'{len(pairs)} final P/N or +/- pairs: ' + ('; '.join(f'{a} ↔ {b}' for a, b in pairs) or 'none'))
        misses = set()
        for n in net_names:
            for ending, replacement in (('DP', 'DM'), ('_P', '_M'), ('D+', 'DP'), ('D-', 'DM'), ('DP', 'D-'), ('DM', 'D+')):
                if n.endswith(ending):
                    other = n[:-len(ending)] + replacement
                    if other in names and (n, other) not in pairs and (other, n) not in pairs:
                        misses.add(tuple(sorted((n, other))))
            if n[-1:] in ('P', 'N', 'p', 'n', '+', '-'):
                partner = {'P': 'N', 'N': 'P', 'p': 'n', 'n': 'p', '+': '-', '-': '+'}[n[-1]]
                desired = n[:-1] + partner
                for other in net_names:
                    if other != n and other.casefold() == desired.casefold() and not any(n in p and other in p for p in pairs):
                        misses.add(tuple(sorted((n, other))))
        for a, b in sorted(misses):
            finding('Differential pairs', 'quality', f'Possible near-miss {a} / {b}: not paired. If these are a differential pair, rename to identical case-sensitive stems with final uppercase P/N or +/- (e.g. USB_P/USB_N or USB_D+/USB_D-).')

        estimate = sum(max(0, len(p) - 1) for p in pads_by_net.values())
        connections, connection_source = estimate, 'pad-count estimate, before existing copper/planes'
        # The zone and via options the suggested route uses; the escape analysis runs with the same ones.
        soft = bool(planes) or args.soft_zones
        zone_opts = ['--soft-zones', '--keep-vias-off-pads'] if soft else []
        if soft and args.small_pad_mm != 2:
            zone_opts += ['--vias-off-pads-below', f'{args.small_pad_mm:g}']
        if not binary:
            finding('Escape and engine warnings', 'info', 'TraceMaker not found: skipping engine DRC warnings and escape checks; set --tracemaker or TRACEMAKER. This is not routing clearance sign-off.')
        elif drc_text is not None:
            for line in engine_warnings:
                finding('Escape and engine warnings', 'info', line)
            if not engine_warnings:
                finding('Escape and engine warnings', 'info', 'TraceMaker DRC emitted no rule warnings.')
            if drc_code not in (0, 5):
                finding('Escape and engine warnings', 'block', f'TraceMaker DRC failed (exit {drc_code}): {drc_text.strip()[-500:]}')
            else:
                m = re.search(r'^unconnected_items\s+(\d+)\s*$', drc_text, re.M)
                connections = int(m[1]) if m else 0
                connection_source = 'TraceMaker DRC unconnected_items'
            try:
                with tempfile.TemporaryDirectory(prefix='tracemaker-preflight-') as tmp:
                    escape_path = Path(tmp) / 'escape.json'
                    p = subprocess.run([binary, 'escape', str(args.board), '--json', str(escape_path)] + zone_opts,
                                       capture_output=True, text=True, timeout=600)
                    if p.returncode != 0 or not escape_path.exists():
                        raise RuntimeError(f'TraceMaker escape failed (exit {p.returncode}): {(p.stderr or p.stdout).strip()[-300:]}'
                                           + (' (this binary predates `escape --soft-zones`; update TraceMaker)' if 'not expected' in p.stderr + p.stdout else ''))
                    escape = json.loads(escape_path.read_text())
                report['escape'] = escape
                dead = escape['dead']
                groups = [(part['ref'], group) for part in escape.get('parts', []) for group in part.get('dead', [])]
                descriptions = [f"{ref}: {', '.join(group['pins'][:12])}" + (f" (+{len(group['pins']) - 12} more)" if len(group['pins']) > 12 else '')
                                + f" ({group['reason']})" for ref, group in groups]
                walled = sum(len(g['pins']) for _, g in groups if 'custom disallow rules' in g['reason'])
                mode = 'fills ignored (--soft-zones, as the suggested route)' if soft else 'fills as fixed copper (no --soft-zones)'
                # "Dead" means no escape on the analysis lattice, not proof that none exists; pins whose net the
                # existing copper completes are `satisfied` and not searched.
                notes = []
                if walled:
                    notes.append(f'{walled} walled in by the custom disallow rules: relax the rule for those pins or fan them out by hand')
                if dead > walled and not soft and any(z.get('filled') for z in zone_details):
                    notes.append('zone fills count as fixed copper; if they are stale (routing deleted), rerun with --soft-zones and route with it')
                finding('Escape and engine warnings', 'quality' if dead else 'info',
                        f"Escape analysis ({mode}): {escape.get('pins', 0)} pins to route, {escape.get('satisfied', 0)} already connected, {dead} dead; "
                        + ('; '.join(descriptions[:10]) if dead else 'every pin checked can leave its package.')
                        + ('; more in JSON report' if len(descriptions) > 10 else '')
                        + ('. ' + '; '.join(notes).capitalize()[:1] + '; '.join(notes)[1:] + '.' if notes else ''))
            except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as exc:
                finding('Escape and engine warnings', 'info', f'Escape analysis unavailable: {exc}')
        work = 1000000 if connections < 100 else 10000000 if connections < 500 else 50000000
        command = [binary or 'tracemaker', 'route', str(args.board), '-o',
                   str(args.board.with_name(args.board.stem + '-routed.kicad_pcb')), '--json', 'route.json'] + zone_opts
        if pairs:
            command.append('--diff-pairs')
        # The work budget is only deterministic while no variant reaches --time (default 120 s), which stops it
        # early without a message; the benchmarks pair --work with --time 3600 for the same reason.
        command.extend(['--work', str(work), '--time', '3600'])
        report.update(connections=connections, connection_source=connection_source, recommended_command=shlex.join(command))
        finding('Recommended routing', 'info', f'{connections} connections ({connection_source}); --work {work} is a starting point, not a completion guarantee. Budget tiers: <100: 1M; <500: 10M; otherwise 50M.')
        if planes and not args.soft_zones and all(l in ('F.Cu', 'B.Cu') for z in planes for l in z['layers']):
            finding('Recommended routing', 'info', 'The pours are on outer layers only, where --soft-zones does not always help (KiCad demos at 3M work, unconnected after refill: StickHub 30 → 15, sonde xilinx 0 → 2). Also route without --soft-zones --keep-vias-off-pads and keep the result with the better sign-off.')
        finding('Recommended routing', 'info', 'Resolve block findings first. Review quality/slow findings; then sign off both boards with refilled KiCad DRC.')
        print(f'# TraceMaker board preflight: {args.board.name}\n\nUnits: mm. Severity: block / slow / quality / info. Read-only; not an exact geometric DRC.')
        for title, lines in sections.items():
            print(f'\n## {title}\n')
            print('\n'.join(lines))
        print('\n```sh\n' + report['recommended_command'] + '\n```')
        report['severity_counts'] = dict(sorted(Counter(f['severity'] for f in report['findings']).items()))
        if args.json:
            args.json.write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
        return 0
    except (OSError, ValueError, TypeError, KeyError, IndexError) as exc:
        print(f'Preflight tool error: {exc}', file=sys.stderr)
        return 3


if __name__ == '__main__':
    sys.exit(main())

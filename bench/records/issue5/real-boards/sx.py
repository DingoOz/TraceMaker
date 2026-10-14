"""A small s-expression reader and footprint scanner for KiCad boards (tooling for the issue-5 records, not part of the engine)."""
import re, sys, json, pathlib

TOK = re.compile(r'\(|\)|"(?:\\.|[^"\\])*"|[^\s()"]+')

def parse(text):
    stack = [[]]
    for m in TOK.finditer(text):
        t = m.group(0)
        if t == '(':
            stack.append([])
        elif t == ')':
            l = stack.pop()
            stack[-1].append(l)
        elif t[0] == '"':
            stack[-1].append(bytes(t[1:-1], 'utf-8').decode('unicode_escape') if '\\' in t else t[1:-1])
        else:
            stack[-1].append(t)
    return stack[0][0]

def find(node, name):
    return [c for c in node if isinstance(c, list) and c and c[0] == name]

def footprints(path):
    root = parse(pathlib.Path(path).read_text(errors='replace'))
    out = []
    for fp in find(root, 'footprint') + find(root, 'module'):
        lib = fp[1]
        layer = (find(fp, 'layer') or [[None, 'F.Cu']])[0][1]
        at = (find(fp, 'at') or [[None, '0', '0', '0']])[0]
        x, y = float(at[1]), float(at[2]); ang = float(at[3]) if len(at) > 3 else 0.0
        props = {p[1]: p[2] for p in find(fp, 'property') if len(p) > 2}
        pads = []
        for p in find(fp, 'pad'):
            pat = (find(p, 'at') or [[None, '0', '0']])[0]
            px, py = float(pat[1]), float(pat[2]); pang = float(pat[3]) if len(pat) > 3 else 0.0
            sz = (find(p, 'size') or [[None, '0', '0']])[0]
            dr = find(p, 'drill')
            drill = 0.0
            drill2 = 0.0
            if dr:
                d = [v for v in dr[0][1:] if isinstance(v, str) and re.match(r'^-?[0-9.]+$', v)]
                drill = float(d[0]) if d else 0.0
                drill2 = float(d[1]) if len(d) > 1 else drill
            layers = (find(p, 'layers') or [[None]])[0][1:]
            pads.append({'number': p[1], 'type': p[2], 'shape': p[3], 'x': px, 'y': py, 'angle': pang - ang, 'w': float(sz[1]), 'h': float(sz[2]) if len(sz) > 2 else float(sz[1]), 'drill': drill, 'drill2': drill2, 'layers': layers})
        out.append({'lib': lib, 'ref': props.get('Reference', ''), 'back': layer == 'B.Cu', 'x': x, 'y': y, 'angle': ang, 'pads': pads, 'props': props})
    return out

if __name__ == '__main__':
    for path in sys.argv[1:]:
        fps = footprints(path)
        by = {}
        for f in fps:
            by.setdefault(f['lib'], []).append(f)
        both = [(lib, sum(1 for f in l if not f['back']), sum(1 for f in l if f['back'])) for lib, l in by.items() if any(f['back'] for f in l) and any(not f['back'] for f in l) and len(l[0]['pads']) >= 3]
        print(path, len(fps), 'footprints;', len(both), 'lib ids on both sides:', both[:6])

#!/usr/bin/env python3
# abcmp.py A B : per board routed, user CPU s, RSS, field s, identical output
import sys, os, json, filecmp, re
A, B = sys.argv[1:3]; root = '/home/dingo/Programming/TraceMaker-speed/build/ab/'
for b in sorted(os.listdir(root + A)):
    row = []
    for n in (A, B):
        d = f'{root}{n}/{b}'
        try: r = json.load(open(f'{d}/route.json'))
        except Exception: row.append('missing'); continue
        st = open(f'{d}/stderr.txt').read()
        u, m = st.strip().split('\n')[-1].split()
        f = re.search(r'CPU \(([\d.]+) s', st)
        row.append(f"{r['routed']}/{r['connections']} {float(u):6.1f}s {int(m)//1024:5d}MB field {f.group(1) if f else '?'}s")
    same = filecmp.cmp(f'{root}{A}/{b}/out.kicad_pcb', f'{root}{B}/{b}/out.kicad_pcb', shallow=False) if os.path.exists(f'{root}{B}/{b}/out.kicad_pcb') else False
    print(f"{b[:28]:28s} | {row[0]} | {row[1]} | {'identical' if same else 'DIFFERENT'}")

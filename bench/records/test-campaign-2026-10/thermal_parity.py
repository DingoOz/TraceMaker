import sys, json, hashlib, pathlib, subprocess, re
sys.path.insert(0, "bench")
import drc_judge, run
def kicad(p):
    st = p.stat()
    key = hashlib.sha1(f"{p.resolve()}:{st.st_mtime_ns}:{st.st_size}:{True}:{drc_judge.JUDGE_CODE_VERSION}".encode()).hexdigest()[:16]
    f = run.DRC_CACHE / f"{key}.json"
    if not f.exists(): return None
    d = json.load(open(f))
    out = set()
    for v in d["violations"]:
        if v["type"] == "starved_thermal":
            m = re.search(r"pad (\S+) \[.*\] of (\S+)", v["items"][1]["description"])
            out.add(f"{m.group(2)}.{m.group(1)}")
    return out
for p in sorted(pathlib.Path(sys.argv[1]).glob("*.kicad_pcb")):
    k = kicad(p)
    r = subprocess.run(["build/release/src/app/tracemaker", "drc", str(p), "--refill-zones"], capture_output=True, text=True).stdout
    ours = set(re.findall(r"  pad (\S+) on", r))
    print(f"{p.stem:40} kicad {None if k is None else len(k)} ours {len(ours)}  only-kicad {sorted(k - ours) if k is not None else '-'} only-ours {sorted(ours - k) if k is not None else '-'}")

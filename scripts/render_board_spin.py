import subprocess, concurrent.futures as cf, pathlib, sys
"""Render the two finished boards of the placement video in 3D (kicad-cli pcb render), 90 angles each, 8 at a time.
Run from the repository root after the composer (or bench/ses_import.py) has written build/video_place/fr_human.kicad_pcb."""
V = pathlib.Path("build/video_place")
jobs = []
for tag, board in (("fr", V / "fr_human.kicad_pcb"), ("tm", V / "tm.kicad_pcb")):
    out = V / f"spin_{tag}"
    out.mkdir(exist_ok=True)
    for k in range(90):
        jobs.append((board, out / f"s_{k:03d}.png", k * 4))
def run(j):
    board, png, az = j
    if png.exists():
        return
    subprocess.run(["kicad-cli", "pcb", "render", str(board), "-o", str(png), "-w", "900", "-h", "660", "--quality", "high", "--perspective",
                    "--rotate", f"-58,0,{az}", "--zoom", "1.1", "--background", "transparent"], capture_output=True, check=True)
with cf.ThreadPoolExecutor(8) as ex:
    list(ex.map(run, jobs))
print(len(jobs), "renders")

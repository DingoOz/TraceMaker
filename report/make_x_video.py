#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Short square clip for social posts: TraceMaker placing a board's parts, then routing it.

  build/report-venv/bin/python report/make_x_video.py build/xvideo/kitspace_aquarius --out report/x_aquarius.mp4

Inputs in the directory (see report/VIDEO.md, "Social clip"): pf/ (report/render_place.py --by-motion),
rf/ (report/render_events.py --step T), tm_events.jsonl (for the live routed count), placed.json (placement time and
result) and video_facts.json (numbers for the end card, all measured: KiCad DRC of the routed board, ratsnest
lengths as drawn by render_place.py). Routing plays at a printed speed-up; placement is labelled condensed, with its real time.
"""
import argparse
import json
import pathlib
import subprocess

from PIL import Image, ImageDraw, ImageFont

FPS = 30
W = H = 1080
BG = (15, 17, 21)
INK = (236, 238, 240)
DIM = (140, 146, 156)
ACCENT = (64, 200, 220)
GOOD = (90, 210, 120)
FONT_DIR = pathlib.Path(__import__("matplotlib").__file__).parent / "mpl-data/fonts/ttf"


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(FONT_DIR / ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf")), size)


def canvas() -> tuple[Image.Image, ImageDraw.ImageDraw]:
    im = Image.new("RGB", (W, H), BG)
    return im, ImageDraw.Draw(im)


def header(d: ImageDraw.ImageDraw, step: str, title: str, right: str) -> None:
    d.text((40, 34), step, font=font(30, True), fill=ACCENT)
    d.text((40 + d.textlength(step + "  ", font=font(30, True)), 34), title, font=font(30, True), fill=INK)
    d.text((W - 40 - d.textlength(right, font=font(26)), 38), right, font=font(26), fill=DIM)


def paste_frame(im: Image.Image, path: pathlib.Path) -> None:
    fr = Image.open(path).convert("RGB")
    im.paste(fr, ((W - fr.width) // 2, 100))


def fade(im: Image.Image, a: float) -> Image.Image:
    return Image.blend(Image.new("RGB", im.size, BG), im, max(0.0, min(1.0, a)))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--out", required=True)
    ap.add_argument("--route-step", type=float, default=0.0813, help="routing seconds per frame (render_events --step)")
    a = ap.parse_args()
    src = pathlib.Path(a.dir)
    facts = json.loads((src / "video_facts.json").read_text())
    frames = src / "xframes"
    frames.mkdir(exist_ok=True)
    for f in frames.glob("*.png"):
        f.unlink()
    n = 0

    def emit(im: Image.Image) -> None:
        nonlocal n
        im.save(frames / f"f_{n:05d}.png")
        n += 1

    # Title, 2 s.
    for k in range(2 * FPS):
        im, d = canvas()
        t1, t2 = "TraceMaker", "places and routes a real KiCad board"
        d.text(((W - d.textlength(t1, font=font(96, True))) / 2, 380), t1, font=font(96, True), fill=INK)
        d.text(((W - d.textlength(t2, font=font(36))) / 2, 510), t2, font=font(36), fill=ACCENT)
        t3 = facts["board_line"]
        d.text(((W - d.textlength(t3, font=font(26))) / 2, 580), t3, font=font(26), fill=DIM)
        emit(fade(im, min(k / 10, (2 * FPS - k) / 8)))

    # Placement.
    # Placement: the kept candidate's recorded states with motion interpolated (render_place.py --by-motion), so the
    # clip is condensed rather than sped up by a fixed factor; the real time is printed instead of a clock.
    pf = sorted((src / "pf").glob("frame_*.png"))
    ptotal = facts["place_s"]
    real = f"{int(ptotal // 60)}:{int(ptotal % 60):02d}"
    for i, f in enumerate(pf):
        im, d = canvas()
        header(d, "1", "Placing the parts", "condensed")
        paste_frame(im, f)
        d.text((40, 1000), real, font=font(44, True), fill=INK)
        d.text((40 + d.textlength(real + " ", font=font(44, True)), 1014), "real time, incl. router checks of each candidate",
               font=font(24), fill=DIM)
        emit(fade(im, i / 8))
    for k in range(int(0.8 * FPS)):  # hold the placed board
        emit(im)

    # Routing: live routed count from the stats events.
    stats = [(j["t"], j["routed"], j["total"]) for j in map(json.loads, (src / "tm_events.jsonl").read_text().splitlines())
             if j.get("type") == "stats"]
    rf = sorted((src / "rf").glob("frame_*.png"))
    speed = a.route_step * FPS
    for i, f in enumerate(rf):
        t = i * a.route_step
        routed, total = 0, stats[-1][2]
        for ts, r, _ in stats:
            if ts <= t:
                routed = r
        if i == len(rf) - 1:
            routed = stats[-1][1]
        im, d = canvas()
        header(d, "2", "Routing", f"{speed:.1f}× speed")
        paste_frame(im, f)
        d.text((40, 1000), f"{t:4.1f} s", font=font(44, True), fill=INK)
        lab = f"{routed}/{total} connected"
        d.text((W - 40 - d.textlength(lab, font=font(36, True)), 1006), lab, font=font(36, True), fill=GOOD if routed == total else INK)
        if routed == total and i < len(rf) - 1:
            sub = "tidying: shorter tracks, fewer vias"
            d.text((W - 40 - d.textlength(sub, font=font(22)), 970), sub, font=font(22), fill=DIM)
        emit(fade(im, i / 6))
    last = im
    for k in range(int(0.8 * FPS)):
        emit(last)

    # End card, 4 s: the finished board, dimmed, under the measured results.
    board = Image.open(rf[-1]).convert("RGB")
    for k in range(4 * FPS):
        im, d = canvas()
        im.paste(fade(board, 0.35), ((W - board.width) // 2, 100))
        d = ImageDraw.Draw(im)
        y = 250
        for big, small, col in facts["card"]:
            d.text(((W - d.textlength(big, font=font(64, True))) / 2, y), big, font=font(64, True), fill=col and tuple(col) or INK)
            d.text(((W - d.textlength(small, font=font(28))) / 2, y + 78), small, font=font(28), fill=DIM)
            y += 160
        t = "TraceMaker"
        d.text(((W - d.textlength(t, font=font(40, True))) / 2, 960), t, font=font(40, True), fill=ACCENT)
        emit(fade(im, min(k / 8, (4 * FPS - k) / 10)))

    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(FPS), "-i", str(frames / "f_%05d.png"), "-c:v", "libx264",
                    "-pix_fmt", "yuv420p", "-profile:v", "high", "-crf", "18", "-movflags", "+faststart", a.out], check=True)
    print(f"{a.out}: {n} frames, {n / FPS:.1f} s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Records the next board's six arms from their live-viewer websockets, as --record would (a "t" field is added).

Waits for a board directory that is not in the skip list, then for each arm's "live view" line, connects to
ws://127.0.0.1:<port>/ws and writes <board>/<arm>/events.jsonl until the router closes the socket.
"""
import asyncio, json, pathlib, sys, time

import websockets

HERE = pathlib.Path(__file__).resolve().parent
SKIP = set(sys.argv[1:])


async def record(arm_dir, port, t0):
    out = open(arm_dir / "events.jsonl", "w")
    for _ in range(60):
        try:
            async with websockets.connect(f"ws://127.0.0.1:{port}/ws", max_size=None, ping_interval=None) as ws:
                async for m in ws:
                    if isinstance(m, bytes):
                        continue
                    j = json.loads(m)
                    out.write(json.dumps({"t": round(time.time() - t0, 6), **j}) + "\n")
                    out.flush()
            return
        except (OSError, websockets.exceptions.WebSocketException):
            await asyncio.sleep(1)


async def main():
    while True:
        boards = sorted((d for d in HERE.glob("sample*") if d.is_dir() and d.name not in SKIP), key=lambda d: d.stat().st_mtime)
        if boards:
            break
        await asyncio.sleep(2)
    b = boards[-1]
    print("recording", b.name, flush=True)
    t0 = time.time()
    tasks = []
    for k in range(1, 7):
        a = b / f"a{k}"
        while not ((a / "log").exists() and "live view" in (a / "log").read_text(errors="ignore")):
            await asyncio.sleep(0.2)
        tasks.append(record(a, 8900 + k, t0))
    await asyncio.gather(*tasks)
    print("done", b.name, flush=True)


asyncio.run(main())

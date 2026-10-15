#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drive a board session from a shell, one tool call per command (doc 19).

A model that has a shell tool but no MCP client cannot keep a process open between commands. `relay.py start` starts `tracemaker session --jsonl` under a small
daemon that holds it, and `relay.py call` sends one tool call and prints the answer. Standard library only. Every request and answer is also appended to
`transcript.jsonl` in the session's folder, which is what the evaluation (`bench/agent_score.py`) reads.

  relay.py start NAME BOARD OUT [--dir DIR] [--tracemaker TM] [--work-limit N] [--threads T]
  relay.py call  NAME TOOL ['{"arg": 1}'] [--dir DIR] [--wait S]     # the arguments are one JSON object; the answer is one JSON line
  relay.py result NAME [--dir DIR] [--wait S]                        # the answer of a call that was still running when `call` gave up waiting
  relay.py stop  NAME [--dir DIR]                                    # ends the session (save first: `call NAME save`)

A call that takes longer than --wait seconds (default 240) prints {"pending": true}; ask `result` for it. Calls to one session are answered in order.
"""
import argparse
import json
import os
import pathlib
import socket
import subprocess
import sys
import threading
import time

DEFAULT_DIR = "/tmp/tracemaker-relay"


def folder(args):
    return pathlib.Path(args.dir) / args.name


def send(sock_path, msg, wait):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(wait + 5)
    here = os.getcwd()
    os.chdir(sock_path.parent)  # a socket path is limited to about 100 characters: connect by its short name
    try:
        s.connect(sock_path.name)
    finally:
        os.chdir(here)
    s.sendall((json.dumps(msg) + "\n").encode())
    data = b""
    while not data.endswith(b"\n"):
        chunk = s.recv(1 << 20)
        if not chunk:
            break
        data += chunk
    s.close()
    return data.decode().strip()


def serve(args):
    d = folder(args)
    sock_path = d / "sock"
    session = subprocess.Popen([args.tracemaker, "session", args.board, "--jsonl", "--out", args.out, "--log", str(d / "session.log"), "--threads", str(args.threads)]
                               + (["--work-limit", str(args.work_limit)] if args.work_limit else []),
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open(d / "stderr.txt", "w"), text=True, bufsize=1)
    transcript = open(d / "transcript.jsonl", "a")
    lock = threading.Lock()
    pending = {"answer": None, "running": False}
    done = threading.Condition(lock)

    def run(request):
        session.stdin.write(json.dumps(request) + "\n")
        session.stdin.flush()
        line = session.stdout.readline().strip()
        with done:
            transcript.write(json.dumps({"request": request, "answer": json.loads(line) if line else None}) + "\n")
            transcript.flush()
            pending["answer"], pending["running"] = line or '{"error":{"code":"failed","message":"the session ended"}}', False
            done.notify_all()

    if sock_path.exists():
        sock_path.unlink()
    srv = socket.socket(socket.AF_UNIX)
    os.chdir(d)
    srv.bind(sock_path.name)
    srv.listen(4)
    (d / "ready").write_text("ok")
    while True:
        conn, _ = srv.accept()
        msg = json.loads(conn.makefile().readline())
        reply = None
        with done:
            if msg["op"] == "stop":
                reply = '{"stopped": true}'
            elif msg["op"] == "call" and pending["running"]:
                reply = '{"error":{"code":"not_allowed","message":"a call is still running; ask for its result first"}}'
            elif msg["op"] == "call":
                pending["running"], pending["answer"] = True, None
                threading.Thread(target=run, args=(msg["request"],), daemon=True).start()
            elif not pending["running"] and pending["answer"] is None:
                reply = '{"error":{"code":"not_allowed","message":"no call is outstanding"}}'
            if reply is None:
                done.wait_for(lambda: not pending["running"], timeout=msg["wait"])
                if pending["running"]:
                    reply = '{"pending": true}'
                else:
                    reply, pending["answer"] = pending["answer"], None
        conn.sendall((reply + "\n").encode())
        conn.close()
        if msg["op"] == "stop":
            break
    try:
        session.stdin.close()
        session.wait(timeout=60)
    except Exception:
        session.kill()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("op", choices=["start", "call", "result", "stop", "serve"])
    ap.add_argument("name")
    ap.add_argument("rest", nargs="*")
    ap.add_argument("--dir", default=os.environ.get("TM_RELAY_DIR", DEFAULT_DIR))
    ap.add_argument("--tracemaker", default=os.environ.get("TM_BINARY", str(pathlib.Path(__file__).resolve().parent.parent / "build/release/src/app/tracemaker")))
    ap.add_argument("--work-limit", type=int, default=0)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--wait", type=float, default=240)
    args = ap.parse_args()
    d = folder(args)
    if args.op in ("start", "serve"):
        if len(args.rest) != 2:
            ap.error("start NAME BOARD OUT")
        args.board, args.out = (str(pathlib.Path(x).resolve()) for x in args.rest)
        args.dir = str(pathlib.Path(args.dir).resolve())
        if args.op == "serve":
            return serve(args)
        d.mkdir(parents=True, exist_ok=True)
        for f in ("ready", "transcript.jsonl"):
            (d / f).unlink(missing_ok=True)
        subprocess.Popen([sys.executable, __file__, "serve", args.name, args.board, args.out, "--dir", args.dir, "--tracemaker", args.tracemaker, "--work-limit", str(args.work_limit),
                          "--threads", str(args.threads)], start_new_session=True, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=open(d / "relay.err", "w"))
        for _ in range(100):
            if (d / "ready").exists():
                print(json.dumps({"started": args.name, "dir": str(d)}))
                return 0
            time.sleep(0.1)
        print(json.dumps({"error": {"code": "failed", "message": "the session did not start; see " + str(d / "relay.err")}}))
        return 1
    if args.op == "call":
        if not args.rest:
            ap.error("call NAME TOOL ['{...}']")
        tool = args.rest[0]
        try:
            arguments = json.loads(args.rest[1]) if len(args.rest) > 1 else {}
        except json.JSONDecodeError as e:
            print(json.dumps({"error": {"code": "bad_argument", "message": f"the arguments are not JSON: {e}"}}))
            return 1
        print(send(d / "sock", {"op": "call", "request": {"tool": tool, "args": arguments}, "wait": args.wait}, args.wait))
    elif args.op == "result":
        print(send(d / "sock", {"op": "result", "wait": args.wait}, args.wait))
    else:
        print(send(d / "sock", {"op": "stop"}, 30))
    return 0


if __name__ == "__main__":
    sys.exit(main())

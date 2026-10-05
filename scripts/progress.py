#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Updates dev/progress.json (shown on the progress website).

  progress.py status M1 in_progress|done|todo     set a milestone's status (done also ticks all tasks)
  progress.py current M2                          set the current milestone
  progress.py done M1 "substring"                 tick the first unticked task containing substring
  progress.py add M1 "task text" [--done]         add a task
  progress.py tasks M1 "task a" "task b" ...      replace a milestone's task list
"""
import json
import pathlib
import sys

P = pathlib.Path(__file__).resolve().parent.parent / "dev" / "progress.json"


def main(argv: list[str]) -> int:
    d = json.loads(P.read_text())
    ms = {m["id"]: m for m in d["milestones"]}
    cmd, args = argv[0], argv[1:]
    if cmd == "current":
        d["current_milestone"] = args[0]
    else:
        m = ms[args[0]]
        if cmd == "status":
            m["status"] = args[1]
            if args[1] == "done":
                for t in m["tasks"]:
                    t["done"] = True
        elif cmd == "done":
            hit = next((t for t in m["tasks"] if not t["done"] and args[1].lower() in t["text"].lower()), None)
            if not hit:
                print(f"no open task matching {args[1]!r} in {args[0]}")
                return 1
            hit["done"] = True
        elif cmd == "add":
            m["tasks"].append({"text": args[1], "done": "--done" in args})
        elif cmd == "tasks":
            m["tasks"] = [{"text": t, "done": False} for t in args[1:]]
        else:
            print(__doc__)
            return 2
    P.write_text(json.dumps(d, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

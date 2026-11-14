# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-project settings of the TraceMaker KiCad actions (doc 18 §20 E2, D185).

The actions read `tracemaker.json` in the board's project folder, so a project keeps its own settings and nothing has to be set before
KiCad starts. Every key is optional; an environment variable of the same meaning (README) still wins over the file, so old setups keep
working. The file is read strictly: an unknown key or a value of the wrong type is an error naming it, so a typo does not silently
change nothing.

    {
      "version": 1,
      "route_args": "--time-total 120",  # tracemaker route options (TRACEMAKER_ARGS); one limit for the whole run
      "power_width_mm": 0.5,           # supply nets routed this wide where it fits (route --power-width); 0 = off
      "octilinear": true,              # off-angle tracks become 0/45/90-degree doglegs where they fit (route --octilinear)
      "refill": "auto",                # "auto": after footprints moved; true: always; false: never (TRACEMAKER_REFILL)
      "report": "browser",             # "browser": run report opened in the web browser; "project": also kept next to the board; "off"
      "reroute": false,                # route the whole board again (TRACEMAKER_REROUTE)
      "place": "",                     # refine, auto, routable, eco or full: move footprints first (TRACEMAKER_PLACE)
      "place_args": "",                # extra tracemaker-place options (TRACEMAKER_PLACE_ARGS)
      "net_work": 2000000              # search work of "Route selected nets" (TRACEMAKER_NET_WORK)
    }
"""
from __future__ import annotations

import json
import os

FILE_NAME = "tracemaker.json"

# key: (type check, default, environment variable, how the variable's text becomes the value)
DEFAULTS = {
    "route_args": "--time-total 120",
    "power_width_mm": 0.5,
    "octilinear": True,
    "refill": "auto",
    "report": "browser",
    "reroute": False,
    "place": "",
    "place_args": "",
    "net_work": 2000000,
}
ENV = {
    "route_args": "TRACEMAKER_ARGS",
    "power_width_mm": "TRACEMAKER_POWER_WIDTH",
    "octilinear": "TRACEMAKER_OCTILINEAR",
    "refill": "TRACEMAKER_REFILL",
    "report": "TRACEMAKER_REPORT",
    "reroute": "TRACEMAKER_REROUTE",
    "place": "TRACEMAKER_PLACE",
    "place_args": "TRACEMAKER_PLACE_ARGS",
    "net_work": "TRACEMAKER_NET_WORK",
}
PLACE_MODES = ("", "refine", "auto", "routable", "eco", "full")
REPORT_MODES = ("browser", "project", "off")


class SettingsError(ValueError):
    pass


def _check(key: str, v, where: str):
    """The value, checked, or SettingsError naming `where` and the key."""
    def bad(what: str):
        raise SettingsError(f"{where}: \"{key}\" {what}")
    if key in ("route_args", "place_args"):
        if not isinstance(v, str):
            bad("must be a string of options")
    elif key == "power_width_mm":
        if isinstance(v, bool) or not isinstance(v, (int, float)) or not 0 <= v <= 20:
            bad("must be a width in millimetres from 0 (off) to 20")
        v = float(v)
    elif key == "refill":
        if v not in ("auto", True, False):
            bad('must be "auto", true or false')
    elif key == "report":
        if v not in REPORT_MODES:
            bad('must be "browser", "project" or "off"')
    elif key in ("reroute", "octilinear"):
        if not isinstance(v, bool):
            bad("must be true or false")
    elif key == "place":
        if v not in PLACE_MODES:
            bad("must be one of refine, auto, routable, eco, full (or empty)")
    elif key == "net_work":
        if isinstance(v, bool) or not isinstance(v, int) or v <= 0:
            bad("must be a positive whole number")
    return v


def _from_env(key: str, text: str):
    """An environment variable's text as the value it stands for (the variables' old spellings: "1"/"0" for switches)."""
    if key == "power_width_mm":
        try:
            return float(text)
        except ValueError:
            raise SettingsError(f"{ENV[key]}: expected a width in millimetres, got {text!r}") from None
    if key == "net_work":
        try:
            return int(text)
        except ValueError:
            raise SettingsError(f"{ENV[key]}: expected a whole number, got {text!r}") from None
    if key == "refill":
        return {"1": True, "0": False}.get(text, text)
    if key in ("reroute", "octilinear"):
        return text == "1"
    return text


def load(project_dir: str | None, environ: dict | None = None) -> tuple[dict, str]:
    """The settings for a project: defaults, then `tracemaker.json` in `project_dir`, then the environment.

    Returns (settings, source): source says where they came from, for the action's first line of output."""
    env = os.environ if environ is None else environ
    s = dict(DEFAULTS)
    source = "defaults"
    path = os.path.join(project_dir, FILE_NAME) if project_dir else None
    if path and os.path.isfile(path):
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
        except json.JSONDecodeError as e:
            raise SettingsError(f"{path}: not valid JSON: {e}") from None
        if not isinstance(data, dict):
            raise SettingsError(f"{path}: must be a JSON object")
        if data.get("version", 1) != 1:
            raise SettingsError(f"{path}: \"version\" must be 1")
        for k, v in data.items():
            if k == "version":
                continue
            if k not in DEFAULTS:
                raise SettingsError(f"{path}: unknown key \"{k}\" (known: {', '.join(sorted(DEFAULTS))})")
            s[k] = _check(k, v, path)
        source = path
    over = [k for k in DEFAULTS if env.get(ENV[k]) not in (None, "")]
    for k in over:
        s[k] = _check(k, _from_env(k, env[ENV[k]]), ENV[k])
    if over:
        source += " and " + ", ".join(ENV[k] for k in over)
    return s, source


def route_args(s: dict) -> list[str]:
    """The `tracemaker route` options the settings ask for, besides the plugin's own (--emit-items, --reroute, --report)."""
    import shlex
    args = shlex.split(s["route_args"])
    if s["power_width_mm"] > 0 and "--power-width" not in args:
        args += ["--power-width", f"{s['power_width_mm']:g}"]
    if s["octilinear"] and "--octilinear" not in args:
        args += ["--octilinear"]
    return args

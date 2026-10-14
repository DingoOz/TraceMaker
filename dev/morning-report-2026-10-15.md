# Overnight report: the two open issues (#4 viewer authentication, #24 JLCPCB routing rules)

Written during the unattended run that started 2026-10-14 about 11:50 UTC, after your "implement all open issues if they have not already been addressed". Everything is committed locally on two
branches and **not pushed** (no PRs): you asked for the work and a report, not for publication. Assumptions taken without being able to ask: `dev/assumptions.md` A46-A53.

## What was open

Two issues were open (`gh issue list`): **#4** (Live viewer authentication, from a contributor's security review) and **#24** (JLCPCB's three routing rules). Both were unaddressed. The two open pull
requests (#17 and #21, contributor l3wi) are not issues; they were conflicting with `main` earlier and I did not touch them (A53). Issues #5-#8 and #26 were closed earlier in the day.

| Issue | Branch | State |
|---|---|---|
| #4 Live viewer authentication | `issue4-viewer-auth` (3 commits on `main`: the change, the fixes from the review, the records) | built and verified; **one behaviour change you should decide on** (below) |
| #24 JLCPCB's three routing rules | `issue24-jlcpcb-rules` (one commit on top of #4, because both add a row to `docs/12-decisions.md`) | built, off by default, measured on the plane tiers; **`--si-rules` is not the best measured combination**, see below |

## #4: the viewer no longer listens on every interface

The issue was right: docs/09 promised `127.0.0.1` plus a token and the code bound `0.0.0.0` with no authentication and no Origin check. Now (D116, doc 09 "Security"):

- **Default bind `127.0.0.1`** everywhere (`route --view-host`, `tracemaker-view --host`, `return-paths`, `footprint-check`, the Python module). `TM_VIEW_HOST=0.0.0.0` in the environment sets it once for every command.
- **Any other address needs a random 128-bit token** on the page and on `/ws`: in the URL once (`?token=...`, the run prints it), then an `HttpOnly; SameSite=Strict` cookie; `--view-token T` chooses it, `--view-insecure` drops it
  (said in the log). No TLS: the notice says the token crosses the network unencrypted and recommends an SSH tunnel on an untrusted network.
- **Origin check on every WebSocket upgrade**, and for a loopback viewer a **Host check** (DNS rebinding). Checked in a real browser: a page served from another origin read 112 messages (the whole board) from the old
  server and none from the new one.
- At most 16 WebSocket viewers and 128 HTTP connections; no-referrer/nosniff/frame-deny headers; constant-time token compare.
- Tests: 6 new server cases (15 in the server suite), clean under ASan/UBSan **and TSan**, a headless-Chromium run (`bench/records/issue4`).

**What you need to decide / know:** this is a behaviour change for the way you watch routes. Your standing habit is `--view` and opening `http://<host>:8766/` from another machine. From this branch on you need
`--view-host 0.0.0.0` (or `export TM_VIEW_HOST=0.0.0.0` once) and the URL with the token that the run prints (`live view: http://192.168.1.83:8766/?token=...`). The memory note "always start routes with --view" now
also means: pass the host, and give you the token URL. If you would rather keep the old default, it is one line (`ServerOptions::host`), but the issue's argument stands: the stream is the whole board.
The progress website on port 8765 (`tracemaker-devsite`) is a separate service and still binds `0.0.0.0` (your CLAUDE.md says so); I did not touch it.

## #24: the three rules, as options (D117, doc 05 section 44)

`route --si-rules`, or one at a time:

1. **Critical signals first.** `--critical-nets 'CLK*'` names nets; `--critical-auto` finds them from the component rules' roles (crystal pins, USB, Ethernet pairs, PCIe, DDR, HDMI, MIPI, RF line, ...) and from differential pairs, in three tiers
   (clocks/crystals/RF, high-speed links, memory buses); `--critical-first` routes them first, tier by tier, after `--first-nets` (which stay in front). The table is my reading of the catalogue (A50).
2. **Fewer vias.** `--critical-via-cost F` (a via costs critical nets F times as much), a per-net via count in the report, `--critical-via-max N` warns.
3. **Never cross a split.** A lattice map of the cells over a void of the adjacent reference plane, built from the zone fills (`--split-cost P` charges critical nets for those cells; `--split-hard` closes them); the audit of #7 reports what is
   left after the route, per net, in the log, the `--json` summary, and a new **Critical nets** viewer panel (with each net's return path in the overlay).

All off by default (the issue's condition): with no option the routed boards are **byte-identical to main on the quick (30/30), mid (11/11), planes (8/8) and planes `--soft-zones` (8/8) tiers**, and `--critical-auto` alone (a report) changes no copper either.
Tests: 8 router/map cases plus a CLI test (the options are on the `tui` screen with their help); the raster is compared with a point-in-region scan cell by cell (rule 3).

**Measured** (planes tier with `--soft-zones`, 4 boards with 28 critical nets, 3M work units, one seed; compared on the nets both runs routed; details in `bench/records/issue24/README.md`): the split cost (`--split-cost 4`) and the via
cost (`--critical-via-cost 3`), without the ordering, roughly halve the splits crossed (20 -> 11) and cut vias (18 -> 12 to 16). **`--critical-first` by itself adds crossings (62 -> 72) and cancels most of the split cost's benefit when
combined (splits 21 -> 18; with all three 26 -> 25)**, so `--si-rules`, which is all three as the issue lists them, is *not* the best measured combination: `--critical-auto --split-cost 4 --critical-via-cost 3` is. Mean completion moved
by -0.8 to +0.2 points (`--si-rules` 90.5 %, baseline 91.3 %); the clean-pass count did not move. Noise is as large as some of these differences (the four USB pairs that some runs lose are also unrouted in a single-variant
baseline run). **Correction:** my first tables said `--si-rules` routed all 28 critical nets; that was my aggregator counting unrouted nets as routed (the per-net `unrouted` field did not exist yet). The numbers above are from the final binary and
the fixed comparison. Without `--soft-zones` the baseline routes 3 of the 28 and `--si-rules` 4 (same 396/879 connections).

**Two things the work found that you should know:**
- A first version made `--si-rules` route 3 of 147 connections on RoyalBlue: critical nets that cannot be routed spent the whole work budget in big search windows. It is fixed (a wider window only after a critical net's first path
  crossed a void), but it shows that "critical first" can starve the rest when critical nets are unroutable. `--critical-first` alone cost 7 connections on that board (60 -> 53 of 147 in the single-variant check) and was neutral on the tier's completion.
- Without a fix the router crossed a slot by hopping onto the plane layer, and the smoothing pass shortcut a detour back over the split. Both are fixed and tested.

**Not done / limits** (doc 05 section 44): defaults stay off; x3 and +4 pitches are the first values tried, not tuned (A49); the map does not know the antipads of the vias the route adds (the audit does); only the smoothing pass is
split-aware, not length tuning or pair coupling; a rule's own `plane_net` parameter is not read; no per-role via cap beyond the report threshold.

## Independent review

After both commits were done I had a fresh agent review each one read-only (it did not know my conclusions; it probed the viewer live). Nothing it found was a way past the viewer's token or Origin/Host checks, and nothing
changed a board with the options off. It did find real defects, which I verified, fixed with tests and committed as a second commit on each branch:

- **#4:** a burst of WebSocket upgrades overshot the 16-viewer cap (27 got in); 128 idle sockets from one address could lock everyone out of the connection cap (now 16 per address); `TM_VIEW_HOST` was ignored by
  `tracemaker-view`, the Python module and the plugin; IPv6 URLs lacked brackets; a Host like `localhost:80@evil.com` passed the loopback check; the token cookie was not port-scoped. Left as is: the plugin and the Python
  module take `--view-host` but not `--view-token`/`--view-insecure` (they get a random token, printed in the log).
- **#24:** `--split-hard` could close a through-hole critical pad in (the plane's antipad counted as a void); a refined path that failed the exact check lost the legal first path; `--critical-auto` ignored
  `--rules-override`; the in-pad via price was not scaled; the smoothing pass was cubic; every portfolio variant refilled the board. Docs and help that described an earlier design were corrected.

## What I did not do

- Push anything, open PRs, or touch PRs #17 and #21.
- Close #4 and #24 (they are not merged).
- Use the real GPUs (tests and benchmarks ran `--no-gpu`).

## To ship

`git push -u origin issue4-viewer-auth issue24-jlcpcb-rules`, then two PRs (#24's base is `issue4-viewer-auth`), as with #33-#36. Say "ship" and I will; merging is yours to ask for.

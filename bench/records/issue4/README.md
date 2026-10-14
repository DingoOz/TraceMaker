# Issue #4 records: live viewer authentication

Design: [docs/09-visualization.md](../../../docs/09-visualization.md) section 2 "Security", decision D116 in `docs/12-decisions.md`, protocol note in `docs/13-viewer-protocol.md`.
Issue: the viewer bound `0.0.0.0:8766` with no authentication (the design doc promised `127.0.0.1` plus a token), and no Origin check on `/ws`.

## What changed

| | before | after |
|---|---|---|
| default bind | `0.0.0.0` (route `--view-host`, `tracemaker-view --host`, return-paths, footprint-check, the Python module) | `127.0.0.1` |
| another address | open | a random 128-bit token is required on the page and on `/ws` (`?token=` once, then an `HttpOnly; SameSite=Strict` cookie); `--view-token T` sets it; `--view-insecure` drops it and the log says so |
| WebSocket from a web page on another site | accepted (browsers do not apply the same-origin rule to WebSockets) | refused (403): an `Origin` must be the server's own |
| DNS rebinding to a loopback viewer | possible | refused: without a token a loopback viewer checks `Host` |
| number of viewers | unbounded | 16 WebSocket viewers (503 beyond), 128 HTTP connections |
| headers | none | `Referrer-Policy: no-referrer`, `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`, `Cache-Control: no-store` on errors |

`TM_VIEW_HOST` (environment) sets `--view-host` once for every command, so watching routes from another machine is one line in a shell profile plus the token URL the run prints.

## What was checked (`verification/`, `e2e/`)

- `verification/ctest-release.txt`: the full suite, 336 of 336 pass; 4 new server test cases (`[security]`): the loopback default and the Host and Origin checks, the token and cookie flow (page, files and
  stream; wrong and wrong-length tokens; a token does not excuse a foreign Origin), a token of one's own / `allow_open` / refused tokens, the client and connection caps.
- `verification/sanitizers.txt`: the server tests under ASan + UBSan and TSan.
- `e2e/e2e-output.txt`, `e2e/viewer-auth-no-token.png`, `viewer-auth-with-token.png`: headless Chromium against live viewers. A viewer on `0.0.0.0`: the page without the token is a 401
  page, with the token the viewer goes LIVE, the cookie is `HttpOnly` and `SameSite=Strict`, and a later visit without the token in the URL is LIVE too. A page served from another origin
  (`probe.html`) that opens a loopback viewer's `/ws`: the binary from `main` before this change delivered 112 messages (the whole board); this branch refuses it.
- The Node tests and the type check of the viewer (the only viewer change is the token on the WebSocket URL).

## Not covered

- TLS: the token and the stream cross the network unencrypted (docs/09 says so; an SSH tunnel with the loopback bind is the answer for an untrusted network). A token in the URL does not
  stop someone who can read the traffic.
- No account system; the stream is read-only and nothing a client sends is acted on (the control channel is not built).
- The 256 MiB state backlog per client is unchanged; with the cap of 16 viewers the worst case is bounded by 16 x that, which was not measured.
- The progress website (`tracemaker-devsite`, port 8765) is a separate service and still binds `0.0.0.0` by the project's own choice (CLAUDE.md); it was not touched.
- A behaviour change for anyone who watched routes from another machine: they now need `--view-host 0.0.0.0` (or `TM_VIEW_HOST=0.0.0.0`) and the URL with the token.

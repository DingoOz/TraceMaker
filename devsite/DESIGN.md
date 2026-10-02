# Development dashboard: design and style

The design of the live progress website served during TraceMaker's development (`devsite/`,
`http://<host>:8765/`). It records what the page shows, how it is built, and the visual and interaction rules, so
the dashboard can be maintained or reproduced for another project.

## 1. Purpose and principles

The page answers one question at a glance: **where is the project right now, and is it healthy?** The user
followed development there instead of in the terminal, often from another machine on the LAN.

- **Truthful by construction.** Every number comes from an artefact the work already produces: the roadmap file,
  JUnit test results, git, `nvidia-smi`, benchmark summaries and the source tree. Nothing is typed into the page by
  hand, so it cannot drift from reality.
- **Read-only.** The page never changes the repository. It only reads files and runs read-only commands.
- **Live without effort.** It polls every 5 seconds and shows whether it is still connected. The viewer never
  has to reload.
- **Calm.** It uses a neutral surface, one accent colour, status colours only for status, and motion only where it
  carries meaning (the live pulse and bar fills).
- **No build step, no dependencies.** The server uses only the Python standard library. The page is a single
  HTML file with inline CSS and JavaScript, plus two CDN libraries (highlight.js and Google Fonts).

## 2. Architecture

```
dev/progress.json ─┐
dev/activity.jsonl ┤
build/*/test-results/*.xml ┤
git log / status ──┤   devsite/server.py          devsite/static/index.html
nvidia-smi ────────┼─► GET /api/status (JSON) ◄── fetch every 5 s, render panels
kicad-cli version ─┤   GET / (static page)
docs/*.md ─────────┤
bench/results/*/summary.json ┤
source tree (newest file + git diff) ┘
```

- **Server.** `ThreadingHTTPServer` bound to `0.0.0.0:8765` so other machines on the LAN can view it. It runs as
  the systemd user unit `tracemaker-devsite` with `Restart=on-failure`. Request logging is silenced.
- **One endpoint.** `/api/status` returns one JSON document with everything the page needs. That keeps the client
  trivial: one fetch, then render every panel.
- **Probe cache.** Slow probes are cached with per-source time-to-live values so many viewers cost almost nothing:

  | Source | TTL |
  |---|---|
  | tests, builds, latest code | 3 s |
  | git, GPUs | 5 s |
  | code size, docs, benchmarks | 10 s |
  | kicad-cli version | 1 h |

- **Fail soft.** Each probe returns an empty value on error (a missing file, a timed-out command). The page then
  shows an explicit empty state instead of breaking. On the client, a failed fetch keeps the last good render.

### Data contract

| Key | Source | Notes |
|---|---|---|
| `progress` | `dev/progress.json` | Milestones with `id`, `title`, `status` (`done`, `in_progress`, `todo`), `gate`, `tasks[{text, done}]`; `current_milestone` |
| `activity` | `dev/activity.jsonl` | Last 60 events, newest first; each has `ts`, `kind` (`build`, `test`, `note`, `decision`, `milestone`) and `text`. Written by `scripts/devlog.py` |
| `tests` | `build/<preset>/test-results/*.xml` | One entry per build preset. Catch2 writes one test case per section, so cases are merged by name and the worst status wins |
| `builds` | `build/*/CMakeCache.txt` | Preset name, configure time and binary time |
| `code` | source walk | Non-blank lines by language and by module; skips build output, data and dependencies |
| `git` | `git` | Branch, commit count, last commits, uncommitted file count |
| `gpus` | `nvidia-smi` | Name, used and total MiB, utilisation, temperature |
| `docs` | `docs/*.md` | Title (first heading), path, last change |
| `benchmarks` | `bench/results/*/summary.json` | Runs with 20 or more boards only (smoke runs are hidden), last 30, oldest first |
| `latest_code` | newest source file | See section 6 |

## 3. Layout

```
┌───────────────────────────────────────────────────────────────────────────┐
│ [logo] TraceMaker                               ● live · updated 2 s ago  │
│        Development progress · …                                  [Theme]  │
├──────────┬──────────┬──────────┬──────────┬──────────┐                    │
│ Roadmap  │ Current  │ Tests    │ Lines of │ Commits  │   stat tiles       │
│  62% ▬▬  │ M7 40% ▬ │ 43/43    │ code     │ 56       │   (auto-fit grid)  │
├──────────┴──────────┴──────────┴──────────┴──────────┘                    │
│ ┌ main column (1.6 fr) ───────────────┐ ┌ side column (1 fr) ───────────┐ │
│ │ LATEST CODE                         │ │ TESTS (per preset, collapsible)│ │
│ │ ROADMAP (milestones, collapsible)   │ │ GPUS (memory meters)           │ │
│ │ BENCHMARKS (table)                  │ │ ACTIVITY (scrolling feed)      │ │
│ │ CODE BY MODULE (horizontal bars)    │ │ COMMITS                        │ │
│ └─────────────────────────────────────┘ │ ENVIRONMENT & DOCS             │ │
│                                         └────────────────────────────────┘ │
└───────────────────────────────────────────────────────────────────────────┘
```

- **Page width** is capped at 1400 px and centred, with a 16 px side gutter so phones get no horizontal scroll.
- **Order follows attention.** Headline tiles come first, then what is happening now (latest code), then the plan
  (roadmap), then outcomes (benchmarks). The side column holds health signals (tests, GPUs) and history
  (activity, commits).
- **Responsive.** Below 980 px the two columns stack into one. Below 640 px milestone rows drop their percentage
  bar and the task list loses its indent. Tiles use `repeat(auto-fit, minmax(160px, 1fr))`, so they reflow
  without media queries.
- **Spacing rhythm.** Gaps of 12 px between tiles and 16 px between panels; panel padding 16 px; tile padding
  14 × 16 px.

## 4. Visual system

### Colour tokens

All colours are CSS custom properties on `:root`. Components use roles, never raw hex values.

| Token | Light | Dark | Role |
|---|---|---|---|
| `--page` | `#f4f4f1` | `#0f1012` | Page background |
| `--surface` | `#fcfcfb` | `#1a1a19` | Panels and tiles |
| `--surface-2` | `#f0efec` | `#232321` | Hover rows, gate boxes |
| `--border` | `#e2e1dc` | `#2e2e2b` | 1 px borders and dividers |
| `--text` | `#0b0b0b` | `#ffffff` | Primary text |
| `--text-2` | `#52514e` | `#c3c2b7` | Secondary text, panel titles |
| `--muted` | `#7a7974` | `#8d8c84` | Hints, timestamps, table headers |
| `--accent` | `#2a78d6` | `#3987e5` | The one accent: bars, current milestone, links, commit hashes |
| `--accent-soft` | `#cde2fb` | `#184f95` | Accent backgrounds |
| `--track` | `#e8e7e3` | `#2c2c29` | Empty part of progress bars |
| `--good` / `--good-text` | `#0ca30c` / `#006300` | `#0ca30c` / `#4cc94c` | Pass, done, live |
| `--warn` | `#fab219` | `#fab219` | Warnings |
| `--critical` | `#d03b3b` | `#e66767` | Failures, lost connection |
| `--glow` | none | blue glow | Halo on active bars, dark mode only |
| `--code-bg` | `#f6f8fa` | `#0d1117` | Code panel, matching the GitHub highlight themes |
| `--add-bg` | accent at 8% | accent at 12% | Changed lines in the code panel |

- **One accent.** Blue marks progress and "current". Status colours (green, amber, red) are reserved for state
  and never used for decoration.
- **Dark mode is designed, not inverted.** It has its own steps of the same hues: a brighter accent, a lighter
  green for text, a softer red. It also adds the one decorative effect, a faint glow on active progress bars.

### Typography

- **Inter** (400, 500, 600, 700) for interface text at 14 px with 1.45 line height.
- **JetBrains Mono** (400, 600) for identifiers, numbers, paths, hashes and code. All numbers use tabular
  figures, so columns and changing counters don't jitter.
- **Hierarchy.** The title is 20 px bold with −0.01 em tracking. Tile values are 28 px semibold with −0.02 em
  tracking. Panel titles are 13 px uppercase with +0.06 em letter spacing in the secondary text colour, so they
  read as labels rather than headings. Body text is 13 to 14 px; hints and metadata are 11.5 to 12 px.

### Shape and depth

- Panels and tiles have 12 px radius and a 1 px border with no shadow. Depth comes from the page/surface
  contrast, not from shadows. Bars use 4–5 px radius, chips are pills (999 px), buttons 8 px.
- The only shadow is on the floating tooltip, which genuinely sits above the page.

### Logo

A 34 px rounded square outline with a single accent-coloured trace: a horizontal run, a 45-degree jog and another
horizontal run between two pads. It is the product in one glyph: an octilinear route between two terminals.

## 5. Components

- **Stat tiles.** A label, a large number, an optional progress bar and a one-line hint that gives the
  denominator or context ("41 of 66 tasks · 4 of 13 milestones", "all passing · 3 build presets"). A tile never
  shows a bare number without its context.
- **Progress bars.** 8 px tall (10 px for meters, 12 px for module bars) with a `--track` background and an
  accent fill. The fill animates its width with `cubic-bezier(.2,.8,.2,1)` over 0.6 s, so updates are visible but
  not distracting. A bar for in-progress work gets the `active` glow in dark mode. Every bar has a tooltip with the
  exact numbers.
- **Milestone rows.** These are native `<details>` elements, so they work without JavaScript and are keyboard
  accessible. The summary row is a four-column grid: id, title, status chip, and bar with percentage. The body
  shows the milestone's **gate** (its acceptance criterion) in a tinted box, then the task checklist. The current
  milestone's id is in the accent colour and opens by default. Which rows are open is remembered across refreshes.
- **Status chips and icons.** Every status pairs an SVG icon with a word, so state is never carried by colour
  alone:

  | State | Icon | Word |
  |---|---|---|
  | Done or pass | filled circle with a tick | "Done", "Pass" |
  | In progress | half-filled circle | "In progress" |
  | Not started | empty ring | "Not started" |
  | Fail | filled circle with a cross | "Fail" |
  | Skipped | ring with a dash | "Skipped" |

- **Test presets.** One collapsible group per build preset (release, cpu-only, asan, …). The summary shows the
  counts and the time of the last run. Release is open by default, and any preset with a failure opens
  automatically. Failing tests sort to the top.
- **GPU meters.** Memory used against total with the free amount, utilisation and temperature below. The GPUs
  were shared with other jobs, so this panel answers "can I run a GPU job now?".
- **Activity feed.** Newest first, scrolling at 420 px. Each event shows a relative time (absolute time in the
  hover title) and a small uppercase kind label. Milestone and decision events are tinted with the accent.
- **Benchmarks table.** The newest run comes first. TraceMaker's clean-pass column is bold, and Freerouting's
  published results sit in adjacent columns for direct comparison. Completion and the number of boards with added
  DRC errors follow. A one-line caption above the table defines "clean" and where the Freerouting numbers come
  from. Numeric columns are right-aligned in monospace.
- **Code by module.** Horizontal bars scaled to the largest module, with the language totals in one line below.
- **Empty states.** Each panel has its own sentence for "no data yet" that says what would fill it ("No test
  results yet. Run ctest.").

## 6. The "Latest code" panel

Added on request during development so the user could watch the code being written.

- **What is shown.** The server finds the most recently modified source file in the code directories. It diffs
  that file against `HEAD`. It shows up to 80 lines around the **newest contiguous block of changed lines**,
  starting a few lines above the block when it fits. A new, untracked file is shown from its end.
- **Highlighting.** highlight.js 11.9 (C++, Python, TypeScript, CMake, Shell, JSON, Markdown, GLSL and others) is
  applied line by line, so line numbers and change markers stay aligned. The page switches between the GitHub
  light and GitHub dark stylesheets with the theme.
- **Change markers.** Changed lines get a faint accent background and a 3 px accent bar on the left, like a diff
  view, but the code stays readable as code rather than as a diff.
- **Header.** File path in monospace, the visible line range, total lines, how many lines changed, the file's age,
  and a pill with the language name.
- **Stability.** The panel is re-rendered only when the file or its modification time changes. That keeps the
  reader's scroll position during the 5-second refreshes. Line numbers are not selectable, so copying code copies
  only code.

## 7. Interaction and behaviour

- **Liveness indicator.** A green dot with a slow pulse (2 s) and the text "live · updated N s ago". If no
  successful refresh happens for 15 s, the dot turns red, stops pulsing, and the text says "connection lost".
- **Theme toggle.** By default the page follows the operating system (`prefers-color-scheme`). The Theme button
  overrides it by setting `data-theme` on `<html>`. The choice is remembered in `localStorage`, wrapped in
  try/catch so private windows still work. Dark values are declared under both the media query and the
  `data-theme` selector, so the toggle wins in either direction.
- **Tooltips.** One shared floating tooltip follows the pointer for any element with `data-tip`. It is clamped to
  the viewport and ignores pointer events.
- **Motion.** `prefers-reduced-motion` disables the pulse and the bar transitions.
- **Safety.** All dynamic text goes in through `textContent`. The only `innerHTML` uses are static icon strings
  and highlight.js output, which escapes its input.

## 8. Keeping it truthful (process)

The design only works if the inputs are kept current. The rules used during development were:

1. Tick tasks and set milestone status in `dev/progress.json` as work lands, not in batches.
2. Log notable events with `scripts/devlog.py --kind build|test|note|decision|milestone "text"`.
3. Benchmark runs write `bench/results/<run>/summary.json` with `set`, `boards`, `clean_pass`, `completion` and
   `seconds`, plus the Freerouting comparison fields, so they appear automatically.
4. Tests write JUnit XML into `build/<preset>/test-results/`, created at configure time.
5. After changing `server.py`, restart with `systemctl --user restart tracemaker-devsite`. Changes to
   `index.html` need only a browser reload.

## 9. Reusing the design

To reproduce the dashboard for another project, keep these choices and swap the data sources:

- one JSON endpoint built from artefacts the project already produces, with short probe caches;
- a single static page, a five-second poll and a visible liveness state;
- neutral surfaces, one accent, status colours only for status, icons next to every status colour;
- tiles that always carry their denominator, collapsible details for depth, empty states that say what to do;
- light and dark themes as two designed sets of the same tokens, following the system with a remembered
  override.

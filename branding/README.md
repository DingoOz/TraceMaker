# TraceMaker brand

The logo is a trace on two layers: a red (front copper) trace, a via, and a blue (back copper) trace between two pads,
the same colours as the live viewer. Chosen from `logo-options/` (option 04).

| File | Use |
|---|---|
| `tracemaker-logo.svg` | Logo with wordmark for light backgrounds (transparent, text outlined) |
| `tracemaker-logo-dark.svg` | The same for dark backgrounds |
| `tracemaker-trace.svg`, `tracemaker-trace-dark.svg` | The trace alone, a wide strip |
| `tracemaker-icon.svg` | Square icon on a dark tile (favicon, app icon, KiCad package icon) |
| `tracemaker-social.svg` | 1280 x 640 social preview source |
| `favicon.ico` | 16, 32, 48 and 64 px |
| `png/` | Icon at 16 to 512 px, the logos at 1392 px, `tracemaker-social-1280x640.png` |
| `logo-options/` | The 25 first options, kept for reference |

Colours: red `#d64545` / `#ef5b5b` (dark), blue `#2f6fd6` / `#5b9cf5` (dark), ink `#222a35` / `#eef1f6` (dark), icon tile `#101a2e`.
Wordmark: Lato Bold, "Trace" in ink, "Maker" in blue.

Where it is used: `README.md`, `viewer/index.html` (favicon) and `viewer/src/hud.ts` (header), `devsite/`, `kicad_plugin/icons/`
and `scripts/make_pcm_package.py` (`resources/icon.png`). The favicon and header are inlined in those files; if you change
`tracemaker-icon.svg`, update the copies (search for `viewBox="0 0 64 64"` and `data:image/svg+xml`) and the PNGs in
`png/` and `kicad_plugin/icons/`.

GitHub's social preview cannot be set from a file: upload `png/tracemaker-social-1280x640.png` under the repository's
Settings > General > Social preview.

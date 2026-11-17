# Errata: the default route explained (report.pdf, 2026-11-14)

These claims were found wrong against the code on 2026-11-17, while the doc-site page that replaces this report was written
(`docs/explanation/default-route.md`, M25). The PDF is not rebuilt here (no LaTeX on the machine). Variant numbers are 0-based, as in the
report's table.

1. "The order is shortest first (variants 1, 2 and 4 change it)": variant 2 is longest first and variants 3 and 4 are jittered. Variant 1
   is shortest first.
2. "`plane_smd` 25 µm" as its automatic pitch: the automatic pitch is 0.2 mm / 6, rounded down to 30 µm. 25 µm is variant 5's half
   pitch on a small board, and that variant was the one kept in the report's run.
3. Search windows: each margin (2, 6, 20 mm, whole board) also adds a quarter of the connection's length. Negotiated searches stop at
   the 20 mm window and never use the whole board.
4. The portfolio's tie-break is fewer unfinished bus lanes first, then fewer vias, then shorter copper. The report leaves out the
   bus-lane step.
5. The relax rescue's "rounds of 20 M until complete, 200 rounds or time" holds only without `--work`. With `--work`, a pass gets
   ceil(work / 20 M) rounds that add up to `--work`.
6. The history cost applies to every cell a search enters, not only to cells where it crosses routed copper. Crossing costs 1 mm plus
   4 × the history.

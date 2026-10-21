---
status: current
code: [scripts/kicad-cli, scripts/kicad-python]
---

# Run kicad-cli from Docker

KiCad's design-rule check is the judge of every benchmark (decision [D8](../decisions/D008.md)). When KiCad is not
installed, `scripts/kicad-cli` runs `kicad-cli` from the `kicad/kicad:10.0.6` Docker image instead.

```bash
docker pull kicad/kicad:10.0.6
scripts/kicad-cli version
scripts/kicad-cli pcb drc --format json -o drc.json board.kicad_pcb
```

To call it as plain `kicad-cli`, which the tests and `bench/run.py` look for, put `scripts/` on your `PATH` or link
the script into a folder that is:

```bash
ln -s "$PWD/scripts/kicad-cli" ~/.local/bin/kicad-cli
python3 bench/check_env.py      # finds kicad-cli, the engine binary, Freerouting and the fixtures
```

## How it works

The script mounts your home directory, and the current directory when it is outside your home, at the same paths in
the container, so file arguments work unchanged. It runs as your user, so the files it writes are yours.

- **Files must be under your home or the current directory.** A board in `/tmp` that is neither is invisible to the
  container, and `kicad-cli` reports that it cannot open it.
- **A different KiCad version:** `TRACEMAKER_KICAD_IMAGE=kicad/kicad:9.0.6 scripts/kicad-cli ...`. Benchmark
  results are only comparable with the same version; the records say which one they used.
- **"permission denied" on the Docker socket:** you were added to the `docker` group after this shell started. The
  script then retries through `sg docker`; a new login fixes it for good.

`scripts/kicad-python` does the same for KiCad's Python (`pcbnew`), which the board generators such as
`scripts/make_plane_smd_board.py` need.

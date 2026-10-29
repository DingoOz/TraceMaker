---
status: current
code: [scripts/make_release.py, CMakePresets.json, CMakeLists.txt]
---

# Quick start

Get TraceMaker running and route the example board that comes with it, in about five minutes with a download or
fifteen with a build. Pick your platform:

| Platform | How | Tested |
|---|---|---|
| Linux x86_64, glibc 2.38 or newer (Ubuntu 24.04+, Debian 13, current Fedora) | download a release | yes: CI installs it in a clean container on every release |
| Any Linux with GCC 13 or newer | build from source | Ubuntu 26.04 (what CI and development use) |
| macOS 15 or 26 on Apple silicon | build from source | by a contributor ([Chris Hemingway](https://github.com/cHemingway), M2, macOS 15): every test passes, results match Linux; not in CI |
| Windows 10 or 11 | the Linux download inside WSL2 | not yet tried |

No GPU, no CUDA and no KiCad are needed for any of them. A CUDA 12.x GPU on Linux makes routing faster
and gives the same results ([Use the GPUs](how-to/gpu.md)).

## 1. Install

=== "Linux: download"

    Check your C library first: `ldd --version` must say 2.38 or newer (Debian 12 and Ubuntu 22.04 are too old;
    build from source there).

    ```bash
    curl -LO https://github.com/DingoOz/TraceMaker/releases/download/v0.12.1/tracemaker-0.12.1-linux-x86_64.tar.gz
    tar xzf tracemaker-0.12.1-linux-x86_64.tar.gz
    cd tracemaker-0.12.1-linux-x86_64
    export PATH="$PWD/bin:$PATH"
    EX=share/tracemaker/examples/plane_smd
    tracemaker version
    ```

    The [releases page](https://github.com/DingoOz/TraceMaker/releases) has newer versions when there are any; the
    commands are the same with the new number. The archive carries the programs, the live viewer, the example board
    and the agent skills, and needs nothing else installed.

=== "Linux: build from source"

    Install the compiler and libraries. On Ubuntu or Debian:

    ```bash
    sudo apt install g++-15 cmake ninja-build git python3 python3-yaml python3-dev \
        libboost-dev libboost-system-dev libeigen3-dev nlohmann-json3-dev libcli11-dev \
        libsqlite3-dev libzstd-dev catch2 pybind11-dev
    ```

    (`g++-13` or `g++-14` in place of `g++-15` if your release does not have it. Libraries that are missing are
    fetched and built with the project, which makes the first build slower.) Then:

    ```bash
    git clone https://github.com/DingoOz/TraceMaker.git
    cd TraceMaker
    cmake --preset cpu-only
    cmake --build --preset cpu-only
    export PATH="$PWD/build/cpu-only/src/app:$PATH"
    EX=tests/boards/plane_smd
    tracemaker version
    ```

    With a CUDA 12.x GPU, use the preset `release` in place of `cpu-only` (and `build/release/...` in the path).
    To use the live viewer, also build its page once: `cd viewer && npm install && npm run build && cd ..`
    (needs Node.js). More in [Build and test](how-to/build-and-test.md).

=== "macOS (Apple silicon)"

    Install Apple's Command Line Tools (`xcode-select --install`) and [Homebrew](https://brew.sh), then:

    ```bash
    brew install cmake ninja eigen cli11 nlohmann-json catch2 zstd boost clipper2
    git clone https://github.com/DingoOz/TraceMaker.git
    cd TraceMaker
    cmake --preset macos-metal -DCMAKE_PREFIX_PATH="$(brew --prefix)"
    cmake --build --preset macos-metal
    export PATH="$PWD/build/macos-metal/src/app:$PATH"
    EX=tests/boards/plane_smd
    tracemaker version
    ```

    `macos-cpu` builds without Metal. Metal speeds up only the cost-to-go fields
    ([GPU, §8](07-gpu.md)). For the live viewer, build its page once:
    `cd viewer && npm install && npm run build && cd ..` (`brew install node`).

    !!! note "Tested by a contributor, not in CI"
        [Chris Hemingway](https://github.com/cHemingway) built and tested this on an Apple M2 with macOS 15 ([GPU, §8.2](07-gpu.md)); CI does not
        build macOS, so a change can break it unnoticed. If a step fails, please
        [open an issue](https://github.com/DingoOz/TraceMaker/issues) with the output.

=== "Windows (WSL2)"

    TraceMaker has no native Windows build. It should run in WSL2, the Linux system built into Windows, but this
    has not been tried yet. Please [tell us](https://github.com/DingoOz/TraceMaker/issues) how it goes.

    In PowerShell, as administrator:

    ```powershell
    wsl --install -d Ubuntu-24.04
    ```

    Restart if asked, open **Ubuntu 24.04** from the Start menu, and follow the **Linux: download** tab from there.
    Your Windows drives are under `/mnt/c`, `/mnt/d`, …, so a board in your Documents folder is
    `/mnt/c/Users/<you>/Documents/...`. The live viewer opens in your Windows browser at the address it prints.

    The KiCad plugin in the release is for KiCad on Linux; it cannot be used by KiCad for Windows.

## 2. Route the example board

The example is a 24 × 14 mm four-layer board with a 0.4 mm-pitch ball array, a QFN microcontroller and two power
planes. `EX` was set in step 1.

```bash
mkdir -p out
cp $EX/plane_smd.kicad_pro out/routed.kicad_pro      # the design rules travel with the board
tracemaker route $EX/plane_smd.kicad_pcb -o out/routed.kicad_pcb \
    --soft-zones --via-in-pad --keep-vias-off-pads --first-nets XIN,XOUT \
    --work 3000000 --variants 4 --no-gpu
```

It takes a few seconds and ends with:

```
routed 49/49 connections, 108 tracks, 42 vias, ...
```

The options suit this dense board; a plain `tracemaker route board.kicad_pcb -o out.kicad_pcb` is where to start on
your own boards. [Route your first board](tutorials/first-board.md) explains each one.

## 3. Check it

```bash
tracemaker drc --refill-zones out/routed.kicad_pcb
```

With nothing listed after `zones refilled`, the board is clean. `--refill-zones` matters: without it the check
uses the zone fill saved in the input file, which predates the new tracks.

KiCad's own check is the final word. With KiCad 10 installed (any platform), open `out/routed.kicad_pcb` in the PCB
editor, or run:

```bash
kicad-cli pcb drc --refill-zones out/routed.kicad_pcb
```

No KiCad? [Run kicad-cli from Docker](how-to/kicad-cli-docker.md).

## 4. Watch it route (optional)

```bash
tracemaker route $EX/plane_smd.kicad_pcb -o out/routed.kicad_pcb --view --hold \
    --soft-zones --via-in-pad --keep-vias-off-pads --first-nets XIN,XOUT \
    --work 3000000 --variants 4 --no-gpu
```

Open the address it prints (`http://127.0.0.1:8766/`) in a browser. `--hold` keeps the viewer open after routing
ends; stop it with Ctrl+C.

## Next

- [Route your first board](tutorials/first-board.md): the same board, step by step, with what each option does.
- [Route from inside KiCad](tutorials/kicad-plugin.md): the plugin for KiCad 10 on Linux.
- [Command line](reference/cli.md): every option.

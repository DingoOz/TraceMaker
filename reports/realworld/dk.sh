#!/usr/bin/env bash
# Runs a command in the report toolbox image (LaTeX, ngspice, matplotlib, numpy, scipy) with the repository mounted at the same path, as the host user.
REPO=/home/dingo/Programming/TraceMaker
exec docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e MPLCONFIGDIR=/tmp/mpl -v "$REPO:$REPO" -w "$PWD" tm-report:3 "$@"

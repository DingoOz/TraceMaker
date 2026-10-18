#!/bin/bash
# The 0.9.0 snapshot binary (bench/results/rel090-tierC/tracemaker) behind today's bench/run.py: it has no
# --state-out, so that option and its value are dropped. Everything else is passed through unchanged.
args=()
while [ $# -gt 0 ]; do
  case "$1" in
    --state-out|--state-in) shift 2 ;;
    *) args+=("$1"); shift ;;
  esac
done
exec /home/dingo/Programming/TraceMaker/bench/results/rel090-tierC/tracemaker "${args[@]}"

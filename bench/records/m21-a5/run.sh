#!/bin/bash
# A5 (doc 18): the critical-net options of #24 over five seeds on the plane tier (soft zones, 3 M work), three runs at a time.
cd $REPO
jobs=()
for s in 1 2 3 4 5; do
  jobs+=("base|--critical-auto|$s" "first|--critical-auto --critical-first|$s" "split|--critical-auto --split-cost 4|$s"
         "via|--critical-auto --critical-via-cost 3|$s" "splitvia|--critical-auto --split-cost 4 --critical-via-cost 3|$s"
         "firstsplit|--critical-auto --critical-first --split-cost 4|$s" "si|--si-rules --no-si-stitch|$s" "sistitch|--si-rules|$s")
done
printf '%s\n' "${jobs[@]}" | xargs -P 3 -I{} bash -c '
  IFS="|" read -r name opts seed <<< "{}"
  python3 bench/run.py --fixtures $DEMOS --set planes --work 3000000 --refill --jobs 4 --threads 4 --view \
    --name a5-$name-s$seed --route-args="--soft-zones --seed $seed $opts" > build/scratch/a5/a5-$name-s$seed.log 2>&1'
echo all done

#!/bin/bash
# Kills a viastudy process whose resident memory exceeds LIMIT_GB (the machine is shared). Runs for MINUTES minutes.
LIMIT_KB=$(( ${LIMIT_GB:-40} * 1024 * 1024 ))
end=$(( $(date +%s) + ${MINUTES:-90} * 60 ))
while [ $(date +%s) -lt $end ]; do
  ps -o pid=,rss=,args= -C viastudy,viastudy_fb | while read pid rss args; do
    if [ "$rss" -gt "$LIMIT_KB" ]; then echo "$(date +%T) kill $pid rss=${rss}kB $args" | cut -c1-200; kill $pid; fi
  done
  sleep 3
done

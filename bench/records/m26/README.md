# M26: finish the BGA boards that never complete (doc 18 §21, D191)

## F8: the warm start is the ramp's floor (doc 05 §84)

srj20 sample040, warm from its best board (2 open, `srj20-again/sample040/first/a1`), a ramp that runs out of rounds while relaxed:

```
--relax --variants 4 --threads 4 --fast-bends --no-kb --round-work 1000000 --rounds 2 --time 300 --time-total 300
--ramp-from 0.01 --ramp-step 0.02 --ramp-rounds 1 --seed 5 --warm-pcb <best>
```

| Binary | Open | KiCad judge |
|---|---|---|
| main 7333069 (`f8/old`) | 16 | — |
| F8 (`f8/new`) | 2 | 2 unconnected, no added error |

The scoring round routes the same connections and moves some copper (370 of 1,243 segments, 111 of 356 vias differ from the warm board). The unit test
(`clearance ramp from a warm start`) starts a ramp from a complete 24-connection board; without the floor it returned 17 routed. ctest: 476
tests, all passed (2 skipped as before). The quick tier was not run: the default route cannot reach the change (the relax rescue turns the floor
off, and nothing else starts a ramp from a warm start).

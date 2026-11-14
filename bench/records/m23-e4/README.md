# M23 E4: return-path audit in every run report (D188, doc 05 §81)

The planes set (8 KiCad demos, `bench/sets/planes.txt`, fixtures from `bench/prepare_demos.py`), each routed once with the default
options, `--work 1000000 --threads 4 --no-kb --report`. `planes/<board>.log` is the route log and `planes/<board>.critical.json` the
`critical_nets` part of its summary.

| board | critical nets found | void crossings | flagged (split, unstitched layer change, detour over 2 mm) |
|---|---|---|---|
| RoyalBlue54L-Feather | 6 | 0 | 0 |
| StickHub | 18 | 8 | 4 |
| complex_hierarchy | 0 | – | – |
| interf_u | 2 | 8 | 1 |
| multichannel_mixer | 0 | – | – |
| pic_programmer | 0 | – | – |
| plane_smd | 2 | 0 | 0 |
| sonde_xilinx | 0 | – | – |

StickHub routed with and without `--no-critical-audit` (`stickhub-audit.log`, `stickhub-no-audit.log`): the two boards written are
byte-identical (`cmp`), whole-run wall time 40 s and 44 s (noise: two runs at once on a loaded machine).

The four boards with none found (no clock, crystal, RF, USB, Ethernet, PCIe or DDR role and no differential pair) were logged before the
report learnt to say so; `planes/pp.log` is pic_programmer again after that change (its report says the nets were looked for and there was none).

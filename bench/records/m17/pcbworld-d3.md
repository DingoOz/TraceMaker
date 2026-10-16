# PCBWorld-Bench D3 test boards, run `m17-pcbworld-d3`

| class | boards | clean | clean % | mean routability | boards open | median s | Freerouting RC12 clean | TraceMaker clean, FR not | FR clean, TraceMaker not |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| D3-A | 99 | 97 | 98.0 | 0.994 | 2 | 1.3 | 94 (94.9 %) | 4 | 1 |
| D3-B | 10 | 10 | 100.0 | 1.000 | 0 | 10.0 | 9 (90.0 %) | 1 | 0 |
| D3-C | 10 | 9 | 90.0 | 0.996 | 1 | 61.4 | 8 (80.0 %) | 1 | 0 |

Freerouting 2.1.0 with the paper's settings (`-mp 10`, 4 GiB), 3 runs per board, judged by the same judge as TraceMaker above:

| class | boards | clean, mean of runs (@1) | clean, any of 3 runs (best of 3) | mean routability | median s per run | TraceMaker clean, FR best-of-3 not | FR best-of-3 clean, TraceMaker not |
|---|---:|---:|---:|---:|---:|---:|---:|
| D3-A | 99 | 0.670 | 73 (73.7 %) | 0.978 | 4.1 | 24 | 0 |
| D3-B | 10 | 0.500 | 5 (50.0 %) | 0.992 | 7.0 | 5 | 0 |
| D3-C | 10 | 0.200 | 3 (30.0 %) | 0.758 | 66.3 | 6 | 0 |

Published (arXiv 2607.05915, Table 3; best of five rollouts, Freerouting and PPO the mean of four seeds; KiCad 9 and the paper's own rule files):

| method | D3-A CP | D3-A Rout. | D3-B CP | D3-B Rout. |
|---|---:|---:|---:|---:|
| Reference (the human layout) | 1.00 | 1.00 | 1.00 | 1.00 |
| Freerouting 2.1.0 | 0.80 | 0.91 | 0.78 | 1.00 |
| KiCadRoutingTools | 0.74 | 0.94 | 0.20 | 0.86 |
| OrthoRoute | 0.02 | 0.53 | 0.00 | 0.44 |
| GPT-5.4 (agent) | 0.65 | 0.91 | 0.00 | 0.62 |
| PPO (trained on synthetic boards) | 0.86 | 0.95 | 0.45 | 0.85 |

Boards TraceMaker loses (the nightly Freerouting 2.5.0-RC12 flag from PCBench's data, or a clean run of Freerouting 2.1.0 here):

- D3-A `AVR-ISP_pogo-plug_1.27mm_AVR-ISP_pogo-plug_1.27mm`: routed 3/6, added errors none, 2 s

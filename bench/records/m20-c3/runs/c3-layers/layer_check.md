# Layer advisor check c3-layers

Set pcbench-c3-layers, counts 2,4,6, 10,000,000 work units; KiCad refills zones before judging.

| Board | 2 layers (KiCad) | 4 layers (KiCad) | 6 layers (KiCad) | Brute force | Advisor | Match |
|---|---|---|---|--:|--:|---|
| EtherCAT_shield_v1_EtherCAT_shield_v1 | open | open | open | none | none | yes |
| Hardware_Playground_E3_Pcb_rev0.3 | open | open | open | none | none | yes |
| Hardware_Playground_serial_gw_ATMEGA328P | open | open | open | none | none | yes |
| MicroMaple_MicroMaple | open | clean | clean | 4 | 4 | yes |
| fifogfx_c64cart | open | open | open | none | none | yes |
| kitspace_OtterPill | open | open | open | none | none | yes |
| kitspace_ardfpga | open | open | open | none | none | yes |
| kitspace_test-jig | open | open | open | none | none | yes |
| memsarray_mems_modules | open | open | open | none | none | yes |
| poncho_fpga_fpga-dongle | open | open | open | none | none | yes |
| DronPi_dronPi | open | open | open | none | none | yes |
| Hardware_Playground_multisensor_cr2032 | open | open | open | none | none | yes |
| Hardware_Playground_sd_wav_pcm5100 | open | open | open | none | none | yes |
| PCIE-to-MXM-Adapter_PCIEx1toMXM3.0 | open | open | open | none | none | yes |

Answers match on 14 of 14 boards.

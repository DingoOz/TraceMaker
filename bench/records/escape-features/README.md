# Escape features from the EtherCAT shield review (2026-10-17, D126, doc 05 §47)

Options (all off by default): `--escape-length MM`, `--escape-persist`, `--radial-supply`, `--via-drops`, `--escape-stubs`.

- `ab-first-round.txt`: the router's own count (routed/connections, 20 M work, 8 threads, `--soft-zones`, the board's rules, first closure round) for 8 boards x 6 configurations
  (base, E = `--escape-plan`, EL = E + length 12, ELP = EL + persist, ELR = EL + radial, ALL). EtherCAT 177 -> 184 with length 12; E3 242 -> 237; Own-Mailbox 415 -> 413 with length (419 with `--escape-plan` alone);
  the rest neutral. That is a router count, not KiCad's verdict.
- `ethercat-variants.txt`: more variants on EtherCAT alone (length 6/12/20/40, drops, stubs, combinations): best 185 (length 12 + via drops), stubs 183.
- Closure with the features on (closure-6: `--escape-plan --escape-length 12 --via-drops`, all 8 boards, killed under load): no gain on any board (PocketBone 3 open vs 1, fpga-dongle 63-82 vs 47, EtherCAT 17-20 vs 19).
- Why they do not close EtherCAT (renders `reports/realworld/tmp_img/ec2/`): the supply pins' vias (0.6 mm) sit in the escape band between straight 0.25 mm escapes at 0.5 mm pitch; the straight stubs of 13 pins now exist (stub >= 3 mm: 1 -> 13) but 22 pins
  of the top, bottom and right rows are still boxed in by neighbours' routes.

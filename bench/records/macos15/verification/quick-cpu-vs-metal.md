# macos-quick-cpu → macos-quick-metal

30 boards; routed boards byte-identical: 30/30.
Clean pass 25 → 25; mean completion 99.0% → 99.0%; router seconds 197.6 → 140.6.
Route CPU time 215 → 123 s (-42.5%) at the same work.
Byte-identical boards moved -57% to -17% in CPU time (the same work: noise, or speed).

Style (informational): bends per connection 0.7406 → 0.7406; acute angles 0 → 0; stair steps 35 → 35; short segments 364 → 364; off angle segments 761 → 761; detour ratio 1.0556 → 1.0556; long detours 35 → 35.

Comparison notes (not a gate; CPU speed needs equal wall time, doc 10 §5):
- every byte-identical board moved the same way, -57% to -17%: a change in speed (binary or machine), not scatter; run one binary twice to tell which

Gates: pass

# issue54-base-quick → issue54-quick

30 boards; routed boards byte-identical: 24/30.
Clean pass 25 → 27; mean completion 99.9% → 99.9%; router seconds 116.8 → 133.7.
Route CPU time 372 → 430 s (+15.7%) at the same work.
Byte-identical boards moved -6% to +2% in CPU time (the same work: noise, or speed).

Style (informational): bends per connection 0.7728 → 0.7777; acute angles 0 → 0; stair steps 29 → 30; short segments 295 → 298; off angle segments 599 → 688; detour ratio 1.0636 → 1.0501; long detours 37 → 31.

Comparison notes (not a gate; CPU speed needs equal wall time, doc 10 §5):
- route CPU time +15.7% in total
- CubeSAT-Reaction-Wheel_Edison_Motor_Servo: route CPU time 2.3 → 1.0 s (-58%)
- Hangul-Clock_Hangul: route CPU time 0.8 → 35.2 s (+4356%)
- atmel-programmer_atmel_programmer: route CPU time 0.6 → 17.0 s (+2642%)
- esp8266_envmonitor_environment-monitor-1.2: route CPU time 0.6 → 13.0 s (+2134%)
- fan_controller_fan_controller: route CPU time 13.3 → 0.6 s (-96%)
- mini-whip_mini-whip-power-feed: route CPU time 0.5 → 10.8 s (+1909%)

Gates: FAIL
- CubeSAT-Reaction-Wheel_Edison_Motor_Servo: judge failed (after)
- Hangul-Clock_Hangul: judge failed (before)
- atmel-programmer_atmel_programmer: judge failed (before)
- esp8266_envmonitor_environment-monitor-1.2: judge failed (before)
- fan_controller_fan_controller: judge failed (after)
- mini-whip_mini-whip-power-feed: judge failed (before)

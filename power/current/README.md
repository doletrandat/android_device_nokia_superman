# Lumia 730 battery reporting

The internal PM8226 IADC channel was qualified against USB unplug/reconnect:
median +1.271mA on USB, -258.796mA on battery, +1.271mA after reconnect.
Negative means discharge. The original current bridge remains unchanged.

The BMS companion adds `capacity` on the existing `smbb-bif` battery. It reads
coherent hardware OCV and signed 36-bit coulomb counts, uses measured VADC
reference channels and cached IADC gain/offset/sense resistance, and interpolates
the recovered stock BV-T5A temperature/SOC profile. PON OCV uses accumulated CC;
a later hardware OCV rebases the CC origin without resetting the counter.
Nominal full capacity is 2220mAh; ageing and full-cycle accuracy are unqualified.
The malformed original 70C table column is excluded; valid temperatures are
-20C through45C. The user reports correct Windows Phone percentage on this same
replacement BV-T5A.

A worker refreshes every5s. Health reads only the cache, with no sensor operation
in its read callback; samples older than60s are rejected. The only BMS write is
the snapshot latch bit, with readback and restoration on success and failure.
Failed restoration pins the module; `latch_retained` and write-only `release`
allow explicit recovery before normal unload. No CC reset, charger/voltage/input
limit, thermal, GPU or suspend policy change is made. The existing Health HAL
and BatteryManager discover capacity/current after one startup restart.

The existing oneshot loader checks board, SD CID, exact kernel/DT/config and file
hashes, waits at most30s for the battery, then loads provider/current bridge/gauge.
A per-boot lock prevents retries after failure. Cleanup removes only owned modules
in reverse dependency order and keeps dependencies when a consumer cannot unload.
`--check` verifies an already-ready stack or checks admission without loading it.

Build with the existing external module Makefile and matching linked kernel
outputs; no boot image or DT change is needed. Evidence:
`.codex-restart/artifacts/pf37-android-capacity-20261103` includes the external
-j20 build, actual linked export CRC checks for91 imports, ASan/UBSan tests of
actual calibration/math/latch/cache functions, and loader fault/ownership tests.
PF32 established physical current and normal Android current reporting; PF36
cross-checked BMS charge consumption against IADC integration within2.92% over21s.
PF37 checks real Health/BatteryService/BatteryManager percentage without simulated
battery properties. Charging remains a separate unresolved hardware-policy issue.

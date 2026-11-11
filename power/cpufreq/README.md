# Lumia 730 CPU turbo frequencies

Mainline caps MSM8226 at 787.2 MHz ("higher CPU frequencies need speedbin
support"): above that the APC rail (PM8226 S2) must move from the nominal to
the turbo voltage corner, and mainline has no CPR/SPM regulator for 8226.
Windows Phone ran this phone at 1.19 GHz.

Fuses on this unit (QFPROM row 22 = 0x20ad110c_000005e8): speed bin 0, PVS
version 2, PVS bin 23. Downstream msm8226-v2 speed0-bin-v2 allows 998.4,
1094.4 and 1190.4 MHz, all on the turbo corner (CPR ceiling 1.28 V, floor
1.10 V; PVS bin 23 open-loop turbo 1.14 V). Nominal (<= 787.2 MHz) is 1.15 V,
which firmware leaves on S2 (FTSMPS, range 1 = 10 mV steps, VSET 0x73).
Neither the L2 nor the CPU SAW drives the rail (VCTL/AVS/PMIC_DATA all 0).

`superman-apc-dvfs.ko` adds the three OPPs to cpu0 and registers a cpufreq
transition notifier: VSET 0x7b (1.23 V) before any change above 787.2 MHz,
0x73 after any change back, read back and checked; the range register is
never touched (as in the downstream spm-regulator). It refuses to load unless
S2 is an FTSMPS in range 1 at 0x73. It cannot be unloaded; reboot restores
stock behaviour. The rc rebinds cpufreq-dt so the new table is used; the
existing cpufreq rc then selects the performance governor.

CC7 test (boot 3631369d, phone on USB): 60 s per step, four parallel sha256
workers over an 8 MiB blob vs the 787.2 MHz reference: 998.4 504/0 bad,
1094.4 552/0, 1190.4 604/0 (1.57x the 787.2 MHz rate), peak 65 C (passive
trip 75 C). cpufreq cooling device max_state 6, still bound to cpu0-thermal.
Screen-off idle temperature equal at 787.2 and 1190.4 MHz (SPC ~97%).
Launcher swipe p50 34 ms -> 21 ms.

Build with the external-module recipe used by power/charger.
Evidence: handoff/evidence/cc7-cpufreq-20261104.

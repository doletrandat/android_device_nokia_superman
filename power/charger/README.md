# Lumia 730 charger watchdog

Android never charged this phone. The PM8226 SMBB charger watchdog is enabled
by firmware (CHG_WDOG_EN 0x1065 = 0x80, WDOG_TIME 0x20) and stays enabled:
plain and SEC_ACCESS-unlocked writes to clear it are ignored, so the mainline
driver's intended disable never takes effect. Unserviced, the charger idles
with the BATFET open while USB powers the system, and an active cycle stops
about 32 s after the last pet.

CC1 hardware test on boot c6ffb06a (USB 500 mA, 3.95 V, 29.5 C, Health Good):
CHG_EN toggling, vbatdet bypass, charge-done logic and trickle override
clearing all left CHG RT at 0. One write of 0x80 to CHG_WDOG_PET (0x1064)
gave FAST_CHG_ON, BATFET on and +230..285 mA into the battery; without further
pets charging stopped within 24..38 s. Periodic pets alone (no CHG_EN writes)
kept fast charge running.

`superman-charge-wdog.ko` pets every 5 s while USB or DC input is valid. It
never writes charge enable, limits (4.10 V / 900 mA) or thermal settings; the
qcom_smbb NTC gate and the hardware HOT comparator still decide whether charge
current flows. If the worker stops (unload, hang), the watchdog ends charging.

Build with the same external-module recipe as power/current against the
running kernel outputs. Evidence: handoff/evidence/cc1-charger-20261103.
The proper long-term home is the qcom_smbb Superman worker in the kernel.

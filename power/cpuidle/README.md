# Lumia 730 CPU idle (SPM power collapse)

Before CC4 the kernel had no cpuidle driver: idle cores sat in WFI on the fixed
1.15 V S2 rail. CC4 enables CONFIG_ARM_QCOM_SPM_CPUIDLE (selects QCOM_SPM) and
adds a DT idle state:

    /cpus/idle-states/spc: compatible "qcom,idle-state-spc", "arm,idle-state";
    entry/exit latency 150/200 us, min residency 2000 us, local-timer-stop;
    cpu@0..3: cpu-idle-states = <&spc>

`local-timer-stop` is required. The Cortex-A7 arch timer stops in power
collapse; without it a collapsed core only woke on IPIs (CC4a, cpu1 hung).
arch_mem_timer (timer@f9020000) provides the broadcast tick.

`cpuidle-qcom-spm.patch` registers SPC with CPUIDLE_FLAG_OFF on nokia,superman,
so a kernel boots with WFI only. `superman-cpuidle.rc` enables SPC per CPU after
sys.boot_completed. Writing 1 to the same sysfs files turns it off at runtime.

Hardware result (CC4b, boot ef549d8a): TZ accepted the warm boot address; all
four cores entered SPC thousands of times; screen-off whole-phone draw on
battery 196 mA with SPC vs 248 mA without (-21%).

The boot identity contracts and two startup scripts pin the kernel config and
boot image; update them together with any kernel change. Evidence and build
scripts: handoff/evidence/cc4-cpuidle-20261103.

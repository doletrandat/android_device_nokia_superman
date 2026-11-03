# Superman thermal bring-up

PF27 source, applied to Lumia 730 / MSM8226 by PF28 on 2026-10-29.

The PF27 audit found CPU0/CPU1 TSENS zones with 75 C passive and 110 C critical
trips (2 C hysteresis), with CONFIG_CPU_THERMAL disabled and no cooling device or
Android thermal HAL. PF28 installs the service and a coherently linked cooling
kernel. The service reports temperatures; the kernel supplies the cpufreq cooling
backend. Reporting alone does not throttle the CPU.

The HIDL 2.0 service reads exact `cpu0-thermal`, `cpu1-thermal`, and `smbb-bif`
identities, independent of zone numbering. It uses existing kernel trip points,
keeps thresholds/order stable, reports failures/NAN for unavailable readings,
and retains severity over failed reads. Callbacks report initial state and
severity changes, honor filters, reject duplicate/null registrations and drain
in-flight delivery on unregister. CPU cooling states are read when registered;
none are fabricated when no cooling device exists. Legacy CPU usage and fan RPM
interfaces report unsupported. No sysfs writes occur.

Battery readings have no invented shutdown/skin thresholds. The charger keeps its
existing stock limits and temperature policy. No skin/GPU sensor is advertised.
Android's global app thermal status is based on skin temperature in this framework;
CPU reporting alone does not provide a calibrated surface-temperature policy.

For an explicitly selected coherent image:

1. Include `product.mk` from the selected device product and `board.mk` from its
   BoardConfig. The service has its own init rc and VINTF fragment. The optional
   policy labels only the thermal class and permits reads, using the existing
   `hal_thermal_default` domain. No permissive-domain rule is added.
2. Merge `cpu-thermal.config` into the qualified kernel configuration. Kconfig
   additionally selects CPU_FREQ_THERMAL and SCHED_HW_PRESSURE. Existing CPU
   cooling cells/maps and cpufreq-dt's CPUFREQ_IS_COOLING_DEV flag already supply
   the rest of the path. Preserve the selected DT, charger and unrelated sources.
3. Rebuild/link the coherent kernel and qualify every shipped module's ABI. The
   three ARM object checks here are not a full kernel/module integration pass.
4. Cold-boot qualification must verify the cooling-device binding, frequency QoS
   cap behavior, Binder registration, Android clients, SELinux policy, actual
   temperature response and rollback. No heating, forced shutdown or emul_temp
   experiment was performed on the phone.

The selected SD system now contains the service/init/VINTF and compiled policy.
Service/test ARM binaries build successfully.
The portable source tests pass with AddressSanitizer. The actual HIDL implementation
passes fixture and real-sysfs tests in a temporary phone process, including severity
transitions, malformed reads, cooling-state validation and unregister draining.
The original PF27 test did not register a system service and removed its temporary
files. PF28 subsequently installed and registered the service on the real SD
system; see its separate deployment and boot qualification records.

Evidence: `aosp11/.codex-restart/artifacts/pf27-system-usability-20261029/`.

PF28 deployment evidence: aosp11/.codex-restart/artifacts/pf28-system-usability-apply-20261029/.
The installed SD boot is adcd98cd1c72aa383fe5c59aadbd2a6bd9c6143c5e8ab79e947219e454db8257.
User-selected Android boot qualification is recorded there; retain the historical PF27
report as the earlier staged state. The complete source selection and binary rollback
are preserved in the PF28 build/deployment manifests.

PF28 fresh-boot qualification on 2026-10-30: Android boot 6e34ce1d-3187-40cb-98b3-43c1deaed062
completed automatically and the user confirmed home/touch operation. Thermal HAL2.0
connected; real cooling cap 787200 -> 600000 -> 787200 kHz passed and restored.
Audio CORE ready / ADSP running; no ALSA card or audible-audio qualification.
Final ABI verification covers 34 selected module files / 2757 exact import CRCs;
native msm required a same-source rebuild. Unused legacy gpu-diag/msm.ko is
preserved but incompatible. See PF28 REPORT.md and VALIDATION-CORRECTIONS.md.

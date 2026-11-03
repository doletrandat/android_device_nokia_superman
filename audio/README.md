# Android audio bring-up

PF31 provides mono48kS16LE speaker playback and physical DMIC2 capture through
real ALSA, the primary Android HAL, AudioTrack and AudioRecord. The RM-1040
schematic maps IHF_PA_EN to MSM8226 TLMM13; the user heard the Android speaker
tone after that correction. PMIC GPIO2 is not a speaker enable and stays untouched.
DMIC2 capture received the speaker's 1kHz tone with 98% of steady signal power at
that frequency. Recording uses stock MICBIAS1, DEC1, SLIM TX1 and AFE4005.

The duplex PCM owns common supply and codec clocks once across both directions.
Stopping either stream preserves the other's lease; last-owner cleanup restores
the original registers and power. Failed cleanup retains dependent resources and
blocks new acquisition. AFE stop precedes SLIM sink/source disconnect. GPIO13
cleanup selects distinct supported native group states while its latch is low,
restores the inherited latch with output disabled, then restores descriptor input.
The kernel supports group pinconf; the per-pin config API is unsupported.

Actual HAL tests passed recording alone, both duplex start/stop orders and mute
with zero leaked muted samples. A normal foreground app passed three short real
AudioRecord captures concurrently with a three-second AudioTrack tone. A following
33-second normal playback/Internet/standby/reopen test passed with zero DSP errors.
Cold07 reproduced intermittent zero-data microphone reopenings despite successful
first capture and playback. Microphone stability is unqualified. APR route
restoration and distinct SLIM read wire transaction IDs are installed candidates;
their physical cold-boot qualification is pending.
Source and prebuilt PCM, loader, mono input policy and audio feature XML match the
installed selection. Carrier call audio, charging and other phone bringup remain
unfinished; no eMMC, NV, kernel, ramdisk, DT or graphics changes are part of this step.

The HAL prebuffers six 10ms playback periods within its 80ms ring. CORE startup
waits at most 15 seconds for the existing WCNSS attempt to finish before starting
ADSP. It never retries radio startup; --check exits before the wait. Cold05 proved
WCNSS up before ADSP and automatic Internet, but a later old-PCM reopen failed;
that failure remains preserved separately from the corrected shutdown tests.

The scoped product.mk addon retains the selected base providers. Installers bind
boot/config/FDT/kernel notes and actual module CRCs, preserve rollback, verify SD
readbacks and return root read-only. See both handoff ledgers for exact outcomes.

## Historical CORE-only bring-up

# Qualified audio CORE startup addon

PF27 source, applied to the selected PF24-derived SD system by PF28 on 2026-10-29.
This is a DSP/CORE startup repair candidate; analogue audio, ALSA card/PCM routes,
speaker and microphone support are still required.

On boot d60385f5-d13a-4e92-9280-37286e79ae6d the installed helper requires the old
PF22 boot hash a4ada7b9e6ef7c478eb5f465bdb2fb4f3669f1b83004ba02da3b5e5c13561f00.
The installed boot is 5d3b9deb29efa898d2113b5cd906bb7825348dc81ca373036a90d9f9e6c27bdf.
All eight audio module hashes and the firmware match their existing manifests;
ADSP is offline and the helper reports preflight failure. The hash mismatch is
a confirmed rejection gate, not proof that a fresh DSP start will succeed.

The replacement moves qualification into a separate audio-boot.contract with four
SHA256 fields: stored boot image, acquired running FDT, decompressed running config,
and running kernel notes. The supplied contract is pinned to the named acquisition.
It is not automatically derived from a mutable native manifest. Update it only with
an independently qualified boot/kernel/DT/config and corresponding acquisition.
Kernel notes are identity evidence, not a measurement of every byte of running RAM.
A bootloader that changes FDT bytes can legitimately cause a later rejection;
preserve and explain that difference before requalifying it.

Firmware and module hashes, exact board/SoC checks, unique ADSP endpoint, module
ownership, offline state and single-attempt lock all remain mandatory. The startup
sequence and its cleanup are otherwise retained. `--check` performs no writes even
on failure and names the rejecting stage. It never starts the DSP.

For a selected PF24-derived product, replace its old helper copy entry with this
helper and install the contract using product.mk. Retain PF24 firmware, all eight
modules, hash manifest and init rc; this addon does not supply those prerequisites.
Do not add duplicate PRODUCT_COPY_FILES destinations. PF28 replaced the installed helper and added the contract on the verified SD
system. The older PF27 delivery archive preserves the prior staged state.

Validation: 24 actual-script fixture cases pass, including changed stored image,
running FDT/config/kernel notes, firmware/module bytes, wrong board/SoC, ownership,
prior attempt and malformed contracts. Success and failure leave fixture kernel,
message log and property state unchanged. Shell syntax passes. PF28 passed a live DSP startup and CORE query on the original kernel before
installing the cooling kernel. Codec integration and audible playback remain
unimplemented; fresh-kernel startup results are in the PF28 evidence.

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

The80ms playback ring passed10 actual HAL hardware starts with clean standby and
reopening. Capture keeps its separate40ms ring. Reported HAL playback latency
is80ms; normal framework and cold-boot qualification are tracked in the ledgers.

# P35 isolated native-display test profile

This is an unselected source/file-overlay profile for Lumia 730/MSM8226. It uses
the retained P34 modules and Android binaries. It is not a boot image, full SD
image, deployed fix, hardware takeover result or 60fps result. No active product,
kernel, gralloc, init or boot selection is changed by preparing these files.

## P41 checker diagnostics (2026-10-08)

The unselected startup wrapper now records the actual checker exit status before
retaining the existing failure/hold behavior. P41 updates only the warm module,
checker and wrapper in a private sealed profile; it preserves the exact P38/P40
boot and probe phase. It still holds consumers on success and failure and selects
no flip test. Source/local validation is not hardware confirmation.

## Prepared stages

The `probe` stage loads the hash-pinned dependencies and P34 MSM with
`render_only=1 superman_diag=1 vram=16m`, then the guarded warm KMS module with
`allow_handoff=1`. KMS probe owns and writes its IRQ mask/ACK; this future stage
is not a read-only MMIO inventory. The check queries the discovered fixed KMS
resources and advancing real vblank without assigning a new scanout target.

The separate `flip` stage repeats probe, then invokes P34's three-target CPU
test: allocation/import, TEST_ONLY, six blocking fenced flips, and confirmed
original-buffer restoration. A failure retains backing according to the P34
driver's quarantine rules. Kernel helper waits can take seconds; startup does
not kill a process in a blocking commit. This is not a bounded eight-second
diagnostic. Physical target addresses/extent, latch and restoration still need
actual hardware evidence; a profile alone proves none of them.

Both stages stay headless even on PASS. Main/late_start/zygote starts are held
in a private system init copy. Private vendor overrides hold SurfaceFlinger,
boot animation and the allocator/composer services in an unstarted class and
remove graphics HAL interface declarations. Explicit boot animation starts and
the encryption-action SurfaceFlinger start are also held. Vendor overrides must
load last as `zz-superman-p35-hold.rc`; init parse order was checked against this
tree. The final installed init graph must be audited again at packaging.

Successful tests select Mesa, native gralloc/HWC, BGRA format 5 and the discovered
`hwc.drm.device` path without starting their consumers. They do not select a
hardcoded physical card number. The existing render node expectation remains
`renderD128`, with P34's MSM driver-version check. No UI-release action exists.
The profile is not an implementation of delayed GPU acquire-fence testing or
HWC/SF retirement/visible-presentation acceptance.

## Reproducible private preparation

Run `prepare-profile.py --phase probe|flip --output <new-P35-artifact-directory>`.
It checks the mount/capacity, copies 25 exact components to a private `overlay/`,
verifies the full retained P30 rollback image, and writes the source DTS, boot
command line, candidate properties and source/runtime hash records. Existing
outputs are refused and preserved. P30 support components come from retained
files or hash-identical output files; native components come from P34's exact
compiled-artifact records. Only the derived DT is compiled; there is no
kernel/AOSP build, system staging, packaging or ADB
operation in this tool.

The prepared overlays are under:

* `.codex-restart/artifacts/r37-p35-native-test-profile-20261008/probe-RUN04`
* `.codex-restart/artifacts/r37-p35-native-test-profile-20261008/flip-RUN04`

Each DT enables the warm KMS node, keeps normal MDSS disabled and simplefb bound,
and includes `androidboot.superman_native=p34` plus its exact probe/flip stage.
The P36 hardware boot repeated the complete command line. Startup now accepts
repeated identical native candidate/stage values, and rejects every conflicting,
empty or bare native value regardless of order. Both required flags must be
present. Board, property, exact boot/config/component and consumer guards still
apply before any module load. The actual P36 command line reproduces the old
failure and passes the corrected shell-flow test with hardware commands mocked.
P37 hardware evidence supersedes those prepared DTs: their cached board source
disabled the GPU/IOMMU and omitted P30's second stream binding. New preparation
uses `native-boot-dt.py` to extract the exact hash-pinned P30 binary DT from its
retained boot. It preserves every original node/property, including both GPU
IOMMU streams, interrupt provider, clocks and diagnostic quarantine. Only the
chosen bootargs, a new framebuffer phandle and the separate enabled warm-owner
node are permitted. The sealer checks this complete binary-tree equality.
`native-headless.dtsi` is a reference fragment; compiling a generic board tree
does not substitute for the pinned P30 derivation. Firmware acknowledgement is metadata, not hardware
proof. The P34 driver independently checks inherited state and clock readiness.

The three candidate properties are recorded in `candidate-properties.txt`:
`ro.superman.native_profile=p35`, `ro.superman.native_candidate=p34` and the exact
`ro.superman.native_stage`. EGL/gralloc/HWC/native_scanout/format selection
properties must remain unset until the startup decision. A future integration
must apply these only in an isolated candidate and assign a new build identity.

## Future boot integration and sealing

Neither overlay contains `/vendor/etc/superman-native-boot.contract`. Startup
fails at `unsealed` before any candidate module is loaded. Do not run the startup
script in P30 or add these files to the active product.

For a separately authorized candidate build, replace the five init/startup files
and install the hash-pinned overlay components into a private system tree. Keep
the P30 zImage and ramdisk bytes, boot addresses/layout and kernel config; append
the phase's compiled DT and use its exact recorded boot command line. Do not
include normal MDSS/panel cold initialization or the old GPU startup invocation.
Apply the recorded candidate properties in that private system. Audit all init
imports/services, property contexts, native module permissions, dynamic library
dependencies and component removals/overrides in the resulting installed image.
The prepared overlay is not a complete system image or drop-in product makefile.
Preparation now emits `candidate.dtb` directly from P30 and a reproducible
`candidate.dts`; do not rebuild it from the cached generic board source.

After that candidate boot exists, run `seal-contract.py --profile <profile-dir>
--boot <candidate-boot>`. The sealer rejects P30, malformed/truncated headers,
changed command line/layout, altered zImage/ramdisk, unexpected binary DT changes,
component/manifest mismatches and resealing. It writes the exact boot hash/stage
contract and adds its hash to the runtime manifest in that private overlay.
Then insert and read back all components, contract, properties and init files in
the separately identified candidate system image. Verify boot/system partition
hashes, actual installed contents and package provenance before any media write.
Sealing alone does not verify a system image, authorize deployment or establish
hardware readiness.

## Failure and rollback

Startup refuses wrong board/profile/boot/stage, preselected immutable properties,
running consumers, unknown preloaded MSM/KMS/dependencies, bad hashes and unhealthy
IOMMU state. It never unloads a module, restarts graphics or falls back to the
P30 framebuffer composer after owner activation. Failure and PASS both leave
consumers held for inspection. A partial property-selection failure cannot retry.
Forced unbind, driver removal and suspend remain unsupported in the P34 driver.

Preserve startup logs, boot ID, installed hashes, complete dmesg/coredump bytes,
test output and IRQ/target/latch/restoration observations before leaving a test.
The RAM startup log is `/dev/superman-native-startup.log`; if data is mounted it
is also copied to `/data/vendor/superman-native-diag/startup-<boot-id>.txt`.
On unconfirmed restoration, treat scanout backing as held until reboot; no
hot fallback or forced cleanup is supplied.

Rollback is a separate boot using the retained, hardware-tested P30 image:
`v70-graphicsdiag-r37-smp-p30-composition-lock-20261007-sdcard.img`, SHA256
`fa7938b942e4384b7401e613acdd44e3b2dee88b9f65f45e3f0b9d3d1642c659`,
2751463424 bytes. Its exact path and verified hash are in each `profile.json`.
P26 remains separately preserved. No rollback media is written by these tools.
Before any later restoration, capture test evidence, identify the actual removable
media and preserve needed userdata. Prefer a separately prepared P30 card; any
media write requires its own selected target/readback procedure. Internal eMMC
and EFIESP are outside this experiment. After returning to P30, verify actual
boot/config/components on the new boot; do not reuse an old boot ID as provenance.

## Acceptance and limits

First retain a successful probe-stage hardware capture: clock/provider readiness,
exact inherited timing, exclusive owned IRQ, advancing physical counter/vblank,
and unchanged original scanout target. Only then evaluate the flip stage on an
isolated boot: three simultaneous physical targets outside simplefb, correct
DMA/physical backing and layout, TEST_ONLY, latched output fences, no underflow,
original-buffer restoration and pin release. Failures are evidence, not success.

Subsequent separate gates must test delayed GPU producer/acquire fences, failure
retirement, HWC/SF client-target presentation, visible tearing/correctness and
performance. P30's initial GPU hang trigger is still unresolved; P34 recovery is
untested on hardware. Cold panel initialization, suspend and sustained 60fps are
not established by these profiles. No working native UI image can be claimed yet.

Historical local evidence for the superseded profiles: both DTS files compile; binary DT differences were constrained to the wrong baseline;
four init files pass Android's host verifier; 25 actual shell-flow cases pass with
device commands/filesystem mocked; 15 sealer cases pass using explicitly
nonbootable parser fixtures, and the real P30 boot is rejected. These checks do
not execute an IRQ, allocate a phone buffer, present a frame or validate hardware.

## P42 isolated flip phase (2026-10-08)

The separate P42 profile selects `flip` in the boot command line, exact P30-derived
binary DT chosen bootargs, stage property, and sealed boot-hash contract. Its
checker and startup wrapper are selected by exact component hashes. P41's
hardware-passed warm driver, kernel, ramdisk and all hardware DT bindings are
preserved. The startup records both probe and flip exit codes, and requires the
explicit successful original-restore record before a headless flip PASS. Both
PASS and failure keep all Android graphics consumers held. No UI release exists.

The full compressed candidate is built separately on New Volume. Flash/boot and
hardware acceptance remain separate from local packaging. Expect only brief CPU
solid-color test targets followed by a restore attempt; the verbose framebuffer
can remain visible after success. Actual three-buffer allocation/import, physical
latch, fences and original restore have not yet been hardware-confirmed. Read
its report and retain P41/P30 rollback images before testing.


P43 adds a separate sealed `gpu` phase and `--gpu-test`: direct Mesa surfaceless GLES2 shaders render into the same linear DMA-BUF targets used by KMS. Six producer fences are exported, waited before sampled read-only pixel checks, and passed explicitly into the six atomic commits. The first acquire fence is merged with a one-shot 500 ms **software** gate to test blocking wait behavior; this is not measured GPU latency. The original framebuffer must restore with a signaled output fence, and cleanup/failure paths keep the UI held.

The external `gpu-test/superman-acquire-gate.ko` requires the exact board, agreeing GPU-stage boot flags and `allow_gate=1`. It permits one fixed-delay gate, has no MMIO/DMA/display/GPU operations, and remains pinned until reboot. It is excluded from active product/kernel configuration. Host checks and packaging do not establish hardware GPU/acquire behavior, visible native Android UI, cold initialization, suspend/resume or sustained60fps.

# P34 inactive native-display candidate

P34 is a source and local-build artifact for Nokia Lumia 730 / MSM8226. It is not
an image, a deployed fix, a hardware-verified native display, or a 60fps result.
The selected P30 kernel, gralloc, product configuration and boot remain outside
this candidate. P31 Settings source/compiled correction remains undeployed.

## P41 continuation (2026-10-08)

P40 hardware passed inherited ownership, physical clock validation and the real
IRQ/frame gate, registering warm card1. Its userspace checker failed silently.
The exact runtime branch remains unresolved. Matching kernel source shows that
mode reset blocks DRM vblank accounting until drm_crtc_vblank_on; the original
warm probe omitted that call. P41 reflects the inherited running CRTC in DRM
only after the hardware gate, and disables accounting on registration failure or
removal. It adds no target, timing, clock or panel programming.

The checker now identifies the unique warm card using sysfs before opening it,
with a bounded missing-node wait and subsequent DRM identity validation. Opening
MSM card0 during old discovery initialized the GPU on P40; sysfs discovery avoids
that side effect. Probe failures report stage/status/errno, resource counts,
mode/plane/fence properties and actual vblank results. The startup wrapper retains
checker exit status. Existing guards and headless probe phase remain mandatory.
P41 local checks/build/package and later hardware results have separate records;
these source changes alone establish neither userspace vblank nor native UI.

## P40 continuation (2026-10-08)

P39 hardware identifies EBUSY in the inherited-state guard. IRQ_EN is zero;
CTL_FLUSH at fd900618 is 0x1dfb7 across advancing physical frames, with owned
RGB0/CTL bits 0x20008 clear. Current source accepts only that exact captured
pattern or zero. Any other pattern, any owned pending bit, and any IRQ mask
still fail before clock validation/IRQ ownership. It never clears inherited
flush bits. Logs report IRQ mask, hardware CTL0 flush and owned pending bits.

The active absolute fd900600 control block is hardware CTL0: mdp_phys is
fd900100 and msm8x26 CTL offsets are 0x500/0x600. Historical P32/P34 labels used
CTL1 for that same address; only current source labels are corrected. Mode,
addresses, real-vblank gate, supplier/PLL checks and target-retirement behavior
remain unchanged. P40 is a separately built headless probe candidate; it has
not been deployed or hardware-tested. See its isolated artifact report under
`.codex-restart/artifacts/r37-p40-inherited-flush-20261008`.

## P39 continuation (2026-10-08)

P38 hardware capture reached the warm-owner probe, which failed with EINVAL;
Linux cached BYTE0/PCLK0 rates were both zero. The exact return site was not
logged. This supports a missing-parent metadata diagnosis, not a stopped physical
scanout diagnosis. P32 had already measured inherited scanout with cached zeros.

Current `kms/` source adds `firmware-clock.h`: a strict read allowlist of the 14
P32 PLL/MMCC values, two matching snapshots, recovered-rate decoding and agreement
with any nonzero cached framework rates. A zero cache is accepted only after that
physical-register validation. NOCACHE providers are rejected before querying a
rate. The six supplier readiness checks, inherited mode/ownership guards and real
IRQ/frame advancement remain mandatory. No clock provider registration or clock,
DSI, PHY, PLL, reset or power programming is added. Failure logs identify supplier
and probe stages. MMCC is observed through a shared read-only mapping while its
clock driver retains resource ownership. Exact matching is deliberately narrow;
it does not provide cold initialization or independent PLL-lock proof.

The P39 external build and full headless probe image are separate under
`.codex-restart/artifacts/r37-p39-firmware-clocks-20261008`. They preserve the exact
P38 boot/DT/kernel/ramdisk and select only the new warm module plus updated manifests
and identity. The historical `.ko` beside this source and all P34 artifacts retain
their original bytes. P39 is not hardware-tested, and UI/flip phases remain held.
The remaining sections record the original P34 foundation and its local checks.

## What is applied here

`gralloc/` integrates the P33 allocator contract and refuses the framebuffer-copy
interface. `kernel-native-buffer-recovery.patch` records the four functional
changes applied to the private MSM copy: separate platform/GPU MMU eligibility,
contiguous scanout backing, VRAM reservation policy, and guarded A3xx recovery.
The selected cached kernel source has not been patched.

`kms/` implements a separate warm-only DRM display owner. MSM stays render-only;
normal MDSS/DSI stays disabled. This is a laboratory alternative to the ordinary
MSM KMS takeover described in P32, whose initialization resets/reprograms inherited
state. The simplefb supplier stays bound to retain its clock/power ownership.
The candidate borrows clock references and checks existing prepared/enabled
state and byte/pixel rates; it never prepares, enables, disables or retunes clocks.

The driver defaults to refusing probe. It requires the exact Superman/MSM8226
compatible bytes, explicit `allow_handoff=1`, firmware acknowledgement, a chosen
bootargs token `androidboot.superman_native=p34`, no SurfaceFlinger process, the
original simplefb geometry/reservation, and all immutable P32 RUN07 MDP/DSI
register values. It requires an exclusive IRQ, zero previous MDP IRQ mask and
CTL flush, at least four actual interface interrupts, and physical frame-counter
advancement before exposing KMS. Acknowledgement properties alone prove nothing.

Targets must be imported DMA-BUFs with genuinely contiguous physical backing,
32-bit DMA equal to physical address, linear ARGB8888, 720x1280, offset zero and
valid stride/extent. Imported target memory must not overlap protected simplefb.
No display IOMMU or address derived from a GPU IOVA is assumed. No dumb allocator
or fbdev emulation is registered. The P33 pool decision is integrated in MSM;
actual allocation of three targets and remaining pool capacity is untested.

The only explicit MMIO writes are RGB0 address/stride, CTL1 RGB0/CTL flush and
owned interface IRQ mask/ACK bits. No panel, DSI, PHY, PLL, reset, timing,
COUNT_ENABLE, brightness or power programming is implemented. Physical vblank
and a later frame with cleared flush bits are required to signal the latch event
and its output fence. Blocking commits are required; nonblocking/async commits
are rejected. At most three distinct target DMA-BUFs are pinned while native
scanout is active. Repeat GEM imports of the same DMA-BUF must retain the same
validated physical address and size; the first GEM and its attachment keep that
backing pinned. Pins are released only after a verified original-buffer restore. Missing
IRQ, stuck flush, underflow or unconfirmed restore retains backing until reboot.
The module keeps its ownership reference until reboot; forced removal, supplier
unbind and hot replacement are unsupported. System suspend is rejected because
panel suspend/resume and cold initialization remain unknown.

`composer/` is an isolated HWC2 copy. It accepts only this DRM driver and a fenced
full-screen client target; SurfaceFlinger composes layers with the existing Mesa
renderer. It validates gralloc handles, layout, usage, modifier and FD, handles
shared PRIME handles without leaking/closing another FB's handle on AddFB failure,
serializes import/release, refuses failed acquire-fence duplication, propagates
presentation failure, waits for the actual latch fence before releasing the old
composition, and disables callbacks on vblank errors instead of synthesizing them.

## Compiled but unselected

Unique Android modules are `gralloc.superman_native`,
`hwcomposer.superman_native`, and `superman-native-kms-check`. None is in
PRODUCT_PACKAGES or startup. The candidate DTS node is disabled and not included
by the product/prebuilt boot. The linked modules/HALs and exact source are retained
under `.codex-restart/artifacts/r37-p34-native-display-candidate-20261007`.
New byte-identical installed staging copies are removed after retention so that
a subsequent image cannot pick them up merely because these targets were built.

The check executable requires an explicit P34 boot and stopped SurfaceFlinger.
`--probe` queries the fixed KMS resources and actual vblank. `--flip-test` allocates
three CPU-produced test buffers, imports PRIME, performs TEST_ONLY then six
fenced flips, and requests a fenced original-buffer restore. It has never been
run on the phone. It does not exercise GPU production/acquire fences, HWC/SF,
tear-free presentation, or sustained application frame performance. Its blocking
DRM calls use kernel helper timeouts; it is not an eight-second diagnostic.

A future test profile must select the retained modules by exact hash, keep MSM
`render_only=1`, supply the boot/property guards before any graphics service,
choose the discovered physical DRM path rather than assume a card number, and
select `ro.superman.native_scanout=true`, gralloc/HWC `superman_native`, Mesa and
BGRA client targets. This selection/boot profile is not implemented or packaged
here. Do not hot-load it into the preserved P30 session.

## Evidence and remaining gates

Both modules and three Android targets link as ARM32. All 709 MSM and 112 warm-KMS
import CRCs match preserved Module.symvers. The private MSM link retains the
existing init/exit section warning in `adreno_unregister`; it is recorded in the
build log. Actual-source host tests use mocked hardware/DRM calls and ASan/UBSan.
They cover allocation/retirement guards, IRQ/flush/frame gating, original restore,
quarantine, stale-ring recovery and failure paths, malformed handles, shared PRIME
cleanup, concurrent import/release and acquire-FD failure. These checks do not
validate MMIO semantics, clock-provider readiness or the hardware handoff.

The saved ANR precedes the saved GPU hangs. Hardware IB registers differ from the
prefetched ring pointer, and their executing spans are absent from the decoded
capture. The initial hang/ANR trigger remains unresolved. The recovery candidate
requires both post-reset hardware ring pointers to be zero before resetting stale
software positions, and preserves idle/MMU/VBIF/transition guards and fence values.
Its success on the phone has not been tested.

The fresh read-only device capture failed because ADB had no connected device.
P33 remains the latest verified P30 runtime evidence. Before packaging or hardware
activation: refresh P30 provenance/fault evidence, define the isolated startup
profile and rollback procedure, prove prepared clock parents and three simultaneous
physical targets, then test actual IRQ/latch/restore, delayed GPU acquire fences,
HWC/SF buffer retirement and visible presentation. Cold panel initialization,
suspend behavior and sustained 60fps remain unresolved.

## P42 headless CPU flip candidate (2026-10-08)

P41 passed the guarded hardware probe and real userspace DRM vblank on boot
4c0d2254-c248-46ab-922c-2a2e3d015138. P42 reuses that exact warm driver and
P30 kernel/ramdisk. Its separate flip contract holds Android UI and tests three
CPU-filled linear buffers, PRIME import, TEST_ONLY, six latched output fences,
and original-framebuffer restoration. No GPU producer/acquire-fence or HWC/SF
performance result is supplied.

The checker now requires agreeing native/stage boot flags and stage property;
identical complete-command-line repetition observed on P41 is accepted, conflicting
or missing values are rejected. Probe still returns before opening MSM/render.
Flip errors identify allocation/map/export/import/FB/atomic/fence stages. Partial
allocations and mappings are cleaned; mapped and exported strides must agree.
TEST_ONLY requests no fence. Restoration is requested after every live atomic
attempt, including failure, and never claimed from an inactive no-op before a
live attempt. An unconfirmed restoration fails; the existing driver retains
submitted backing until reboot. Cleanup errors also fail the checker.

Actual-source host checks cover 70 flip cases, 20 boot predicates and 31 probe
cases; driver retirement/quarantine checks remain passing. This is a laboratory
image candidate, not a working native UI or 60fps result. See the P42 artifact
report and handoff ledgers for exact build/package/deployment status.


P43 adds a separate sealed `gpu` phase and `--gpu-test`: direct Mesa surfaceless GLES2 shaders render into the same linear DMA-BUF targets used by KMS. Six producer fences are exported, waited before sampled read-only pixel checks, and passed explicitly into the six atomic commits. The first acquire fence is merged with a one-shot 500 ms **software** gate to test blocking wait behavior; this is not measured GPU latency. The original framebuffer must restore with a signaled output fence, and cleanup/failure paths keep the UI held.

The external `gpu-test/superman-acquire-gate.ko` requires the exact board, agreeing GPU-stage boot flags and `allow_gate=1`. It permits one fixed-delay gate, has no MMIO/DMA/display/GPU operations, and remains pinned until reboot. It is excluded from active product/kernel configuration. Host checks and packaging do not establish hardware GPU/acquire behavior, visible native Android UI, cold initialization, suspend/resume or sustained60fps.

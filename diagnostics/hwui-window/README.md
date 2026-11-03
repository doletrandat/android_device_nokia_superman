# HWUI window diagnostic

Standalone public-API Android 11 app, excluded from PRODUCT_PACKAGES. It is a
diagnostic workload, not a production renderer change. It uses a normal hardware
accelerated window and eight independently recorded child Views. Text, opaque
patches, rounded path clipping and saveLayerAlpha are drawn with Canvas.

The private artifact build and guarded runner are retained at
`/home/doletrandat/Desktop/p69-hwui-window-test-20261013`.
The package is `org.superman.diagnostics.hwui`, activity `.WindowTest`.
Starting requires a new 16-character lowercase hexadecimal `token` extra;
an existing token directory is rejected without overwriting it. No permissions,
services, receivers, launcher entry or networking are requested. The diagnostic
is debuggable and signed with the public development test key; do not distribute
it as a production app.

The original default mode has 48 scheduled updates, separated by 150 ms. First all eight children
are invalidated; later only the first child changes while seven RenderNodes are
retained. This does not force full-window EGL damage or change HWUI properties.
Actual child onDraw counts and phases are recorded. Four captures occur after a
750 ms stationary interval, with no queued diagnostic updates during capture.
PixelCopy reads the app Window's most recently queued Surface buffer, cropped in
window coordinates to the content View. It excludes SurfaceFlinger's composed
output and physical scanout, and does not establish presentation timestamps.

PixelCopy performs GPU/CPU readback and can perturb timing. BEGIN/END markers
identify each readback. The software reference uses the same scene drawing code,
so it is a backend comparison, not an independent geometry oracle. An independent
uniform-patch oracle tests 6,144 interior pixels per capture, including alpha,
with channel tolerance 2. Text/shape differences are informational until examined;
software and GPU antialiasing need not be byte-identical. A successful patch check
does not prove text, all pixels, native composition, or display correctness.

Outputs are four window PNGs, four software PNGs and report.json under the app's
private `files/<token>/`. No production app data is captured. The app finishes
after four samples, on pause or at a main-thread 55-second deadline. That deadline
cannot interrupt a blocked UI/driver thread. The host runner has bounded ADB
operations and stops only this diagnostic package on an incomplete watch when
the boot/core-process guard still matches. Outputs remain available through run-as.

The host gate verifies exact board/P69 boot/config/native hashes, unchanged core
process IDs, empty diagnostic/render selectors and sufficient memory. Installation
is in userdata only. It starts from and returns to the current activity; no system
service restart, flash, trace arm, renderer/global property or driver replacement
is performed. Preserve failed installation/build/capture attempts separately.

Observed 2026-10-13: four captures complete, 24,576 uniform-patch comparisons pass;
all 32 text regions have maximum channel difference 2 from their software reference.
Child counts confirm seven retained nodes during the final two samples. User saw
no visible glitch. Shape-band differences remain; this limited workload did not
reproduce the intermittent systemwide fault. See the artifact REPORT.md for exact
boot/APK provenance and diagnostic overhead limitations.

## Transform extension

`--es mode transforms` runs four stages of96 Choreographer callbacks: parent
scrolling, child translation/scale, overlapping translucent children, and rotation
ending at exact identity matrices. Seven child display lists remain recorded once;
child0 changes its label once per stage. Callback timestamps are metadata, not proof
of presented frames or60fps. The schedule is bounds/endpoint tested on the host.

`--ez readbacks false` skips all PixelCopy, bitmap, software-reference and PNG work;
only per-stage metadata is saved. A separate `--ez readbacks true` run pauses750ms
after each stage and saves four app-window/software image pairs. The software
reference applies the observed child matrices, alpha, layout and parent scroll.
Its geometry shares source operations. Independent solid-patch checks apply only
at the final identity stage; zero checks on transformed stages are not passes.
Captures are never synchronized evidence of the physical panel.

2026-10-13 extension outcome is in
`/home/doletrandat/Desktop/p69-hwui-transform-test-20261013/REPORT.md`.
Both runs completed; the user reported missing content and flashing/tearing without
pixel readbacks, then symptoms only during motion in the capture run. Captured
app-window output contains missing rows and malformed colored geometry. Final
identity-stage patches fail6144/6144. This is a captured diagnostic failure,
not a successful rendering test or a deployed fix.

PixelCopy itself imports the last queued AHardwareBuffer, waits its fence, draws
through an intermediate GPU surface and reads back pixels. Therefore the failure
could arise in content production or that readback path; it does not by itself
localize the defect to HWUI, Mesa, SurfaceFlinger, or physical scanout. No kernel
GPU fault was indexed during these two runs. Global renderer settings are unchanged.

### Optional direct-buffer diagnostic (2026-10-13)

`--ez direct true` adds a guarded CPU mapping of this window's last queued linear
GBM buffer before and after PixelCopy during settled pauses. It records buffer
identity, next-frame IDs and texture matrix; comparisons must verify matching
identities and account for crop/format/transform. Mapping can perturb timing and
cache state. The JNI build and host layout tests pass, but the first device run
rebooted near startup without a retained direct capture. Runtime correctness is
unvalidated; do not rerun while investigating repeated reboots/full boot storage.
See `/home/doletrandat/Desktop/p69-hwui-direct-buffer-test-20261013/REPORT.md`.

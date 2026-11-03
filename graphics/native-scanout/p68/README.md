# P68 paired Settings/SF diagnostic foundation

Source/private-build artifact, not a packaged or deployed phone image and not
a visual-correctness fix. P67 remains the installed image.

`Output::composeSurfaces` calls this inactive hook immediately after the actual
RenderEngine draw, before its still-dequeued target reaches `queueBuffer`.
It selects the exact Settings SubSettings buffer/frame in that draw's request.
The input remains held by SF's current consumer/request and the target remains
dequeued; no queue, release, present or rendering result is substituted.

Activation requires the future read-only selector
`ro.superman.native_sf_pair_trace=p68`, exact Superman/MSM8226 compatible and
native gralloc/HWC properties, system UID, an internal physical display with
layer stack 0 and 720x1280 bounds, and an explicit 16-lowercase-hex runtime arm
in `debug.superman.target_trace`. No product currently selects this diagnostic.
The conditional init action creates `/data/vendor/superman-sf-pair-trace` as
system/graphics mode0700 only when that future selector is present.

One immutable arm per SF process, two selected attempts including failures,
five-second initiation window. At most two input/target pairs: four files of
3,686,400 pixel bytes each, 14,745,600 total plus metadata. Clearing/changing
the property cannot reset the budget. The hook skips reused composition-cache
results, absent/failed draw fences, protected/secure outputs, ambiguous Settings
requests and unrecognized handles/layouts. It duplicates the real output fence
CLOEXEC, waits at most250ms per attempt, then requires actual input/output
sync-file status1. It never invents a signaled fence or successful draw.

Read locks use only SW_READ_OFTEN. Linear native handles, exact buffer IDs,
dimensions/strides/formats, backing size/dev/inode and distinct input/target
backings are checked. Both mappings are unlocked before files are written.
Files use fixed names, O_EXCL/O_NOFOLLOW and mode0600; incomplete output and
sidecars are retained and rejected by the decoder. Partial failures consume
their slot. Pixel allocation is CPU scratch memory, not a new scanout buffer.

`decode-pair-trace.py INPUT_DIRECTORY NEW_OUTPUT_DIRECTORY` requires complete
pairs with matching arm/PID/attempt/source-frame/compose time, successful fences,
valid geometry and monotonic read/write times, exact byte counts and FNV hashes.
It renders RGBA Settings input beside BGRA SF target, preserving originals.
Transforms, alpha, other layers, color conversion and CPU visibility can make
these images differ. A numerical difference alone is not a corruption diagnosis.
These are post-draw/pre-queue readbacks, without a real-latch or physical-panel
image guarantee. This Settings-only hook does not capture a Recents app input.

The waits, CPU mappings, copies and userdata I/O perturb presentation and cache
visibility. The five-second initiation/250ms fence limits are not a hard bound
on mapper or storage time; this cannot benchmark rendered60fps or tearing.

Local validation builds the actual Output object/private ARM32 SF library and
checks the production capture body with mocked Android APIs under ASan/UBSan.
Synthetic pairs test writer/decoder behavior, not phone rendering correctness.
Integration still needs a candidate image identity/selector, a boot-bound host
trigger with proper restoration/readbacks and a fresh hardware trial. Do not
replace/restart the currently running SF to enable this source artifact.

# P67 current-target diagnostic candidate

Source and private ARM32 HWC validation only. P66 remains installed. This does
not select a product profile, change a boot/system/SD image, fix rendering, or
establish hardware success. A separately attributed future image must select
`ro.superman.native_target_trace=p67`; the existing P66 selector remains intact.

P66 traces subsequent commits. The immediate button still produced intact About
buffers after visible text loss. The user's lock-screen photo and earlier-boot
Recents video show missing content outside Settings. Their physical frames are
not synchronized to diagnostic timestamps. A camera's moire, rolling shutter,
compression and normal transitions must not be mistaken for a driver diagnosis.

P67 retains a CLOEXEC duplicate of each successful primary composition's actual
latch fence before its normal present fence is transferred. The duplicate is
owned by that composition and closes at retirement/restoration/destruction.
There is one retained descriptor per composition, temporarily two around swap,
with no additional buffer imports or producer-slot release changes. Descriptor
duplication failure disables the affected reads and preserves normal commit
status. Failed or missing fences never authorize a read.

The fixed `debug.superman.target_trace` arm is exactly16 lowercase hex digits.
One immutable token per primary HAL process grants at most:

- Two reads of the current composition, on the compositor's real-vblank callback.
  If a presentation reaches the compositor lock before the first callback,
  its first current read happens before committing the replacement instead.
- Two live commit attempts, including failures, each allowing the newly latched
  and old retiring target to be read after the existing successful latch wait.

Every attempted read/commit consumes its slot even on failure. Clearing or
changing the arm cannot replenish it. No new attempt starts more than5 seconds
after the first selected poll. This bounds initiation, not mapping/file duration.
At most six compact720×1280 BGRA outputs/22,118,400 pixel bytes and six metadata
files result. Only one3,686,400-byte temporary copy exists at a time.

All current reads hold the compositor lock that serializes commit, active swap,
retirement and laboratory restoration. Hot-unplug ClearDisplay now takes that
same lock before destroying imports. The retained current producer slot remains
owned under the existing BufferQueue/release contract; an import or fence alone
does not exclude a producer that violates the contract. No physical scanline is
frozen by this software lock.

The explicit acquire fence, reference latch fence and composition's retained
latch fence must each report success1. Existing P66-owned-handle/DMA-BUF
device+inode/layout/linearBGRA/HW_FB/no-protected guards remain. Mapper requests
SW_READ_OFTEN only, full-rect copy, then unlock before private file output. Only
the systemUID1000 mode0700 fixed capture directory is accepted. Outputs use
O_NOFOLLOW/O_EXCL/mode0600; partial or failed files remain evidence. Mapping can
flush or synchronize GPU work and alter the symptom. This is not a cache-policy
experiment and does not prove a read is observationally passive.

Metadata adds arm-observation time, callback vblank time (zero for a before-commit
read), sample origin, and both reference/composition latch descriptors/status.
Original frame, latch count, FB/GEM, backing identity, stride, monotonic read/write
intervals and hashes remain. Failed reads log fence identity/status before skip.

`decode-current-trace.py` accepts only P67 current/latched/retiring records,
validates successful fences/retained proof, initiation window, origin and callback
time, geometry, size, FNV and metadata-name agreement, then writes derived PNGs
to a new directory. Compare only same token/PID/backing device+inode/sourceframe.
Equal bytes do not prove physical scanout is correct. Use a contemporaneous
physical recording and saved logs to correlate a visible symptom; video file
creation time is not an established boot/frame association.

Before a future hardware arm, verify the new image's selected hashes, real SD
boot and config, startup gates, current owner, directory policy and empty arm.
The current P66 button is not compatible with P67 completion/log parsing: adapt
and test it separately before deployment. No current-phone re-arm or HAL restart
is part of this candidate. Native visual correctness, root cause and60fps remain
unvalidated.

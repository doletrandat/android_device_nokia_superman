# P66 client-target trace candidate

This is a diagnostic source/private-HWC build identity, not a new boot or SD image,
not a rendering correction, and not hardware validation. P65 remains installed.

Android screencap allocates a separate GraphicBuffer and re-composes layers into
it with framebuffer caching disabled. It does not read the scanned client target.
The supplied phone photograph after original-setting restoration shows missing
About-page text. The full-redraw experiment still showed text loss and tearing;
its later intact screencap was not simultaneous with the photograph.

The trace follows the HWC-owned, mapper-imported client-target handle, requiring
its DMA-BUF device/inode identity to match the DRM import. It accepts only the
existing 720x1280, linear BGRA8888, HW_FB target, excluding protected content.
The original acquire, atomic commit, real latch wait, swap, retirement and present
fence transfer remain in place. The diagnostic never treats a failed fence as
success. All mapping requests are SW_READ_OFTEN and copied pixels are not written
back into the target.

The current P65 path is inactive: `ro.superman.native_target_trace` must equal
`p66`, then `debug.superman.target_trace` must contain exactly 16 lowercase hex
characters. One arm per primary-display HAL process consumes two live commit
attempts, including failures. Changing/clearing the token cannot reset that
budget. No automatic service restart or re-arm is implemented.

For each selected attempt:

1. Before atomic commit, record target/fence identity. Copy pixels only if the
   explicit acquire fence already reports success. Pending, absent or errored
   fences skip the read; normal presentation continues.
2. After the existing actual-latch wait succeeds, inspect its retained output
   fence and capture the new target only if both fences report success.
3. Before swapping/destroying the old composition, capture its still-imported
   target under the same successful-latch requirement. FramebufferSurface's
   previous producer slot is released in `onFrameCommitted`, after present
   returns; an import pin alone does not exclude writes. Reading is safe under
   the existing producer/release contract; a producer violating that contract
   remains a possible cause of a changed snapshot.

At most six compact 720x1280 BGRA files (22,118,400 pixel bytes total), each with
metadata, are produced. One 3,686,400-byte temporary copy is allocated at a time.
Mapper 2 may report unknown bytes-per-pixel/stride (-1); then the checked owned
GBM handle supplies them, and the existing gralloc validates actual map stride.
Unknown layout outside that contract is rejected. Mapping is unlocked before
writing the private copy to disk. Directory/file failures do not change the
commit result or original lifetime decisions. Partial files remain evidence;
metadata/log completion flags must be checked.

Capture changes latency. Mapper calls and regular-file I/O have no hard wall-time
limit: two attempts bounds volume, not elapsed time. No timing, frame-rate or
tearing conclusion should be drawn from an armed trace. The latch counter/fence
and monotonic read intervals identify ownership, not the exact scanline/pixel
shown in a user photograph.

## Integration required before a hardware trace

The live source hooks and private ARM32 HWC have been compiled, but no profile
selector, init policy, build manifest, system image or SD image has been changed.
A future separately identified image must select the diagnostic and preserve the
P65 control/rollback. Do not substitute this module into the running HAL.

That future image also needs an explicitly prepared directory:
`/data/vendor/superman-target-trace`, owner system (UID 1000), mode 0700. Its HAL
runs as system. The helper requires a real directory and uses O_NOFOLLOW and
exclusive 0600 file creation. It creates no directory, follows no user-selected
path and overwrites no captures. SELinux/property/file access and real mapper
behavior must be checked on that future image before arming. No current-phone
preparation or property change was performed.

Host-side: preserve a pull of the entire capture directory and the corresponding
logcat/kernel/owner/boot identity before decoding. Run:

```
python3 decode-target-trace.py CAPTURE_DIR --output-dir NEW_DERIVED_DIR
```

Raw input is read-only; derived PNGs and JSON are separate. The decoder requires
matching names/metadata, full byte counts, successful fences, valid layout and
ordered timestamps, and an independently recomputed FNV content hash; it also
records SHA256. Compare only matching arm/process/DMA-BUF device+inode/source
frame. GEM IDs and fd integers alone are insufficient. Missing or skipped
snapshots cannot establish content equality. A changed row across ownership
points supports a lifetime/content discrepancy; equal samples do not prove the
physical scanout or sustained UI is correct.

# P69 bounded composition and ordinary-map metadata

These are bounded diagnostic sources. The separate P69 full SD package is now
verified locally at `/home/doletrandat/Desktop/p69-metadata-image-20261011`. It has
not been deployed or hardware tested and is not a rendering fix or proof of
correct GPU pixels or tear-free scanout.
P68's optional input/target pixel helper remains separate and unchanged.

The exact selector `ro.superman.native_metadata_trace=p69` is required. No product
property or package selector is changed by this source addition. An integrated
image must explicitly select this diagnostic before it can run. A host-started
`debug.superman.metadata_trace=m69-` followed by 16 lowercase hexadecimal digits
starts one window; there is no capture button, pixel read, or new mapping.
The native gralloc/HWC properties and exact root-compatible must also match.
Board/pipeline checks fail closed and are cached after the first valid arm in each
process. Processes denied the read-only compatible-file check emit no records.

Each process has one arm per lifetime. Clearing/replacing the arm suspends it,
never resets the budget. A fork starts its own process budget. The monotonic
window expires 120 seconds after that process's first eligible event. SurfaceFlinger
records at most 480 requests, at most one every 250 ms, up to 16 input requests
per sample. Truncation is explicit. Gralloc records at most 64 ordinary CPU map
attempts, without sampling cadence; those may exhaust earlier. A started map's
end and eventual unmap pair are logged even after the budget expires. Boundaries
are per process, not a simultaneous global start or guaranteed complete event log.

SF records actual request buffer ID/frame number and GraphicBuffer ID, target ID,
geometry, selected blend/format settings, position and texture matrices, existing
acquire/ready fence presence, the actual cache hit/miss/disabled decision, and the
real draw return status. It does not repeat the cache lookup or alter cache policy.
Secure/protected compositions are skipped. The small metadata sampling interval
means unsampled frames can still be faulty; selected settings are not the entire
cache key. No layer name or buffer pixels are logged.

Gralloc observes only the map/unmap already requested by the caller, including
read versus write usage and actual region after normal whole-buffer normalization.
The original result, mapping, lock count and unmap sequence remain in place.
A successful map token is stored in existing BO userdata to pair the ordinary
unlock; failed maps do not acquire an unmap token. Hardware-only locks are skipped.
Unmap returns void: its end status is `-ENODATA` (unknown), not a failure. Invalid
handle/region/nested/registration paths keep their existing behavior and diagnostics.
Access is serialized by the existing gralloc module mutex; SF state belongs to
its existing composition thread. Hook property/clock/stat/log calls preserve errno.

`fstat` of an already-owned DMA-BUF supplies device/inode backing metadata when
available; unknown identities are explicit. There are no FD duplications, sync
queries/waits, buffer lock calls from the SF helper, pixel copies, new GBM maps,
ioctls, new allocations by the hooks, or rendering/configuration policy changes.
Clock/stat/property/log overhead remains. Metadata and signaled fences cannot
prove correct content or physical presentation. Ordinary map timing includes hook
logging and scheduling; SF timing is CPU request-to-return time, not GPU time/FPS.

`decode-metadata.py` indexes a single-boot Linux log offline, reports incomplete
pairs, malformed/duplicate records, truncation, draw/map errors and shared backing
identities. It does not infer that reuse, an absent fence or a read-map caused a
glitch. Keep acquisition boot/image binding and original log seals alongside it.
It rejects NTFS/unknown mounts before opening the input log, symlinks and logs
above 32 MiB. It issues no phone commands and writes only to stdout.

Local verification and frozen build recipes are retained at
`/home/doletrandat/Desktop/p69-metadata-diagnostics-20261011`:
core boundary tests, 18 production-body/interface-mock cases under ASan/UBSan,
11 decoder tests, private ARM32 Output/gralloc compilation and library links,
static imports/exports against P68 verified providers, unchanged retained input
hashes, exact source snapshots and original failed host-build attempts. Mock tests
are not hardware mapping/render tests. No library was staged into the product or
copied to the phone. The P69 image separately integrates the selector, identity/startup gates and host
recorder. Its report records package checks. A provenance-checked hardware watch
still remains; both pixel selectors and the pixel arm must stay empty.

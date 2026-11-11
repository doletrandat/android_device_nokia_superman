# Private GPU clients and idle/resume diagnostic

`superman-gpu-context-cycle-test` is standalone and excluded from PRODUCT_PACKAGES.
It forks before loading Mesa and execs three independent workers. Each worker
creates an unshared EGL context through the installed Mesa implementation, with
a 1×1 surfaceless pbuffer and four private RGBA FBOs. Root-compatible bytes and
FD305 renderer guards match the existing offscreen test. No native window,
display buffer import, DRM master or KMS/HWC posting is involved.

```
superman-gpu-context-cycle-test --run default --size small
superman-gpu-context-cycle-test --run default --size full
superman-gpu-context-cycle-test --run nogmem --size full
superman-gpu-context-cycle-test --run nobin --size full
```

Three processes are used because multiple EGL contexts in one process can share
the same DRM file. Each worker logs its render-node FD and DRM client ID so the
host can verify distinct kernel clients. Their GPU scheduling order and overlap
are not established by process count alone. Workers use different backgrounds,
glyph phases and color channels, independent resources and independent CPU
expectations, and flush each draw without an intervening finish/readback.

Every six operations, each worker compares all reused buffers and a fresh
render-to-texture output, then waits on private pipes. Once all three workers
have reached that checkpoint, the coordinator waits 250 ms and records cached
GPU runtime status and active/suspended time counters before/after the wait.
Android continues running and may prevent suspension. Only observed sysfs state
supports a suspension claim; these samples are not atomic or extra MMIO reads.
Workers then verify retained pixels and submit another GPU copy using the old
resources before continuing. This exercises work after each shared idle interval.

Small mode is 128×128, 36 operations per worker, six idle intervals, 48 readbacks
per worker. Full mode is 720×1280, 12 operations per worker, two intervals,
16 readbacks per worker. Every compared pixel has an independent integer CPU
expectation; tolerance is three component values, and actual maximum errors are
recorded. Each worker must clean up and exit zero, and the coordinator must
successfully reap all three workers before declaring PASS.

Worker alarms are 30 seconds; the coordinator alarm is 45 seconds and kills
only its own worker PIDs. Each pipe receive also has a 12-second bound. Kernel
uninterruptible waits can defeat prompt signal handling; the host adds a timeout
and must stop on nonzero/disconnect/timeout, corrupt pixels, new faults or changed
core identity. Child failures are exercised in local IPC tests. The caller checks
boot/component hashes, memory, recent faults and empty capture selectors before
running. Full cohorts require at least 200 MB reported MemAvailable. The three
workers consume more memory and GPU time than the original single-client test.

Policy flags affect only worker environments before Mesa loads. No global
Android property, renderer or power policy is changed. Flag selection does not
prove every draw follows that particular binning/tiling path. Private GPU work
and readback can affect other processes and timing; this is not a read-only or
nonperturbing observation. This test does not cover arbitrary HWUI/Skia shaders,
stencil/AA/sRGB, native producer fences, Android composition/scanout, or sustained
60fps/stability. A PASS cannot establish the cause of systemwide UI glitches.

Exact sources, binary, local checks and P69 hardware records are retained at
`/home/doletrandat/Superman/history/desktop/p69-multiprocess-idle-test-20261013`.

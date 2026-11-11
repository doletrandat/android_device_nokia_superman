# Private GPU correctness diagnostic

`superman-gpu-offscreen-test` is standalone and excluded from PRODUCT_PACKAGES.
It loads the installed Mesa EGL implementation explicitly, requires exact root
compatibility `nokia,superman\0qcom,msm8226\0` and GL_RENDERER FD305, and uses a
1x1 surfaceless pbuffer plus private RGBA texture FBOs. It never creates an
Android native window, acquires DRM master, imports a display buffer or posts
to KMS/HWC. This is GPU work and CPU readback of private buffers, not a read-only
device query or an FPS benchmark.

```
superman-gpu-offscreen-test --run default --size small
superman-gpu-offscreen-test --run nogmem --size small
superman-gpu-offscreen-test --run nobin --size small
```

Each mode needs a separate process because Mesa reads its debug options once.
The executable changes only its own FD_MESA_DEBUG environment before dlopen.
`default` removes that inherited variable; the other modes set exactly one
source-supported Mesa20.3.4 flag. Other inherited IR3/GMEM/driver overrides are
rejected. No Android property, service environment or device policy is changed.
Mode selection is not proof that every draw took a particular tiling/binning path.

Small mode is128x128/36 operations. Full mode is720x1280/12 operations (`--size full`).
The caller must check available memory, boot/component provenance and recent GPU
faults first, especially for full mode. A20-second signal watchdog covers Mesa
loading, setup, execution and cleanup. This cannot guarantee interruption of an
uninterruptible kernel wait. The host runner also has a timeout and must treat
nonzero/timeout/disconnect, GL errors, changed core identity or new faults as failures.

The test repeatedly changes a64x64 GL_ALPHA glyph mask using full and16x16 partial
glTexSubImage2D uploads. It blends four coverage levels with premultiplied source-over,
uses full and moving scissor damage, optionally preserves and blends over old content,
and rotates three FBOs. Every six operations it samples a newly drawn FBO into a
fourth FBO before any explicit finish/readback, then reads every pixel of all four
outputs. An integer CPU model tracks the expected damage and retained content.
There is no glFinish inserted between uploads/draws; glReadPixels synchronizes only
at verification checkpoints. A small positive highp texture-coordinate bias avoids
ambiguous nearest-sampling boundaries at full resolution. Pixel tolerance is3 bytes
per component for mediump/rounding, but any mismatch beyond that fails the run.
Success is printed only after EGL cleanup; the host also requires exit0.

This finite workload does not cover arbitrary Skia shaders, stencil/AA/clipping,
sRGB, gralloc/native-window imports, producer acquire fences, Android layer reuse,
scanout or sustained stability. GPU work can influence timing of other processes;
private FBOs do not make it a nonperturbing observation. A PASS cannot clear every
GPU rendering path or justify changing Android's global renderer/debug policy.

Implementation/host oracle validation, ABI binding, six P69 hardware runs and
subsequent crash evidence are retained in
`/home/doletrandat/Superman/history/desktop/p69-offscreen-render-test-20261012/REPORT.md`.

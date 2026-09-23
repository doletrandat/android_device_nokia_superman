# Review-only MDSS/GPU proposal

Date: 2026-09-18

The companion patch is an unapplied review artifact:

`review-only-mdss-gpu-enable-20260918.patch`

## Proposed scope

The patch changes only the Superman board DTS and changes the existing
`&mdss` and `&gpu` nodes from `status = "disabled"` to `status = "okay"`.
Both nodes already exist in the shared MSM8226 device tree. No new registers,
clocks, power domains, firmware names, or driver code are introduced.

## Why this is useful

It isolates one hardware-path question: whether the existing Linux MDSS and
Adreno 305 descriptions can probe on Superman. A successful probe would be
useful evidence that the current software framebuffer path is not the only
available display path. It would not, by itself, prove that the panel can
render or that the Android UI can use the GPU.

## Deliberately excluded

- No panel node or DSI endpoint is proposed. The current Superman DT has no
  panel description, while the WOA package's `NOKIA_PANEL`/`oempanel.sys`
  describes a Windows OEM path rather than a Linux panel initialization
  sequence.
- ADSP is not enabled. The Android DT points it at `adsp_region` but leaves
  the remoteproc disabled; the WOA package exposes a Windows
  `qcadsp8626.mbn`, but no verified Android firmware ownership or compatible
  loading path has been established.
- WCNSS is not enabled. The MSM8226 DT snapshot has no corresponding enabled
  WCNSS remoteproc/firmware-loader node for this board.
- MPSS/AMSS and MBA are not enabled or resized. The WOA INF reservations are
  Windows-side evidence, not a safe Android memory-layout change.
- No Android product configuration, HAL, firmware, boot image, or userspace
  library is changed.

## Required validation before any controlled deployment

1. Add or identify the exact Superman panel node and DSI command sequence.
2. Confirm that the kernel contains the matching MDSS, MDP5, DSI, KGSL and
   panel drivers and that their dependencies are enabled.
3. Decide whether Android will use a matching hardware composer/gralloc path;
   the current product explicitly selects SwiftShader/default gralloc.
4. Build only after explicit approval, then validate DT compilation and
   inspect the generated DTB before any device write.
5. On hardware, capture bounded probe, DRM/MDSS, KGSL, and framebuffer logs;
   keep the current software-framebuffer image as the recovery baseline.

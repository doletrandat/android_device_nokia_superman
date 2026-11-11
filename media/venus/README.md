# Lumia 730 hardware video (Qualcomm Venus, MSM8226)

Status (CC18/CC19, 2026-11-04): the stock Windows Phone video firmware
(`qcvss8626.mbn`, VIDEO.VE.1.6-00021, May 2014) runs under mainline Venus. A 320x240
H.264 Baseline clip decodes 60/60 frames bit-exact with ffmpeg (`vdec.c`).
Decoder: H.264, VP8, VC-1 G/L, MPEG-4, MPEG-2, H.263, Xvid -> NV12. Encoder: NV12 ->
H.264, VP8, MPEG-4, H.263 (`v4l2q.c`). The boot image (DT + kernel) is permanent since
CC19; the driver is NOT auto-loaded yet.

What it took (each was a hard reset or hang until fixed):
- No genpd on venus0: mmcc's PWRSTS_ON GDSC turns "off" into asserting VENUS0_RESET
  with the bus live -> watchdog. Both DT nodes omit power-domains; the block reset
  (VENUS0_BCR 0xfd8c1020) is released by qcom_iommu on every runtime resume (CC21; DT `resets = <&mmcc 2>` on the SMMU).
- SMMU (`patches/kernel-qcom_iommu-venus.patch`, built-in): never touch CB_FSR at probe
  on a power-domain instance; IRQ guard while suspended; `qcom,msm8226-venus-iommu`
  cfg = ARM_V7S + no_afe + no_stall + ctx_restore, no halt/BFB/SIDs (TrustZone owns the
  global space; secure-id 0). LPAE/AFE gave an access-flag fault + stalled transaction.
- SMMU needs the "alt core" clock: venus0_vcodec0 as `tbu`, else TrustZone's
  restore_sec_cfg stalls.
- Secure contexts ctx@1000/@2000 declared so TZ gets its secure page-table pool.
- Firmware region must be in the TZ PIL window: 0x0f000000/0x500000, the tail after
  the relocatable ADSP image (0x0dc00000..0x0f000000). At 0x0f500000 PAS_MEM_SETUP is
  -EINVAL and AUTH_AND_RESET -EIO.
- Driver (`patches/venus-*.patch`): msm8226 resources from the downstream vidc node,
  devm_ioremap (SMMU lives inside the core window), TZ load with pas_shutdown first and
  a tolerated mem_setup failure, non-secure IOVAs from 0x40000000 (else "invalid setting
  for UC_REGION"), ISR guard, pm_runtime_forbid on msm8226 (no power collapse yet).

Manual load: videobuf2-common, -memops, -v4l2, -dma-contig, v4l2-mem2mem, then
venus-core (no manual reset release since CC21), venus-dec, venus-enc. Firmware at
/vendor/firmware/qcom/superman/qcvss8626.mbn (from the stock WP image; not in git).
Evidence: handoff/evidence/cc18-venus-20261104, cc19-venusboot-20261104.

TODO: driver-side VENUS0_RESET deassert; 720p/1080p, long-run, open/close, corrupt
streams, screen-off; power collapse; auto-load; Android codec bridge.

## Test results (CC21, 2026-11-05)
720p/1080p High profile with B-frames bit-exact; 720p 109 fps, 1080p 48 fps decode-only;
10 min soak of 650 open/close cycles without a failure; corrupt streams handled
(leading junk before SPS stalls cleanly); decode with the screen off is fine. CPU
reads of decoded buffers are uncached and slow, so a display path must import the
buffers (dmabuf), not copy them. Tools: vdec.c (`-` hashes frames, `n` timing only),
mkau.py (Annex B with AUD -> length-prefixed AUs), refhash.py, soak.sh.

Encoder (CC21): works after skipping the 8x8-transform property on HFI 1xx
(`patches/venus-venc.c.patch`). 320x240-1080p High profile, rate control on target,
deterministic output, 5 min concurrent encode+decode soak clean. Clients must set
per-frame timestamps, the OUTPUT crop (S_SELECTION) and the H.264 level control.
Tools: venc.c, encsoak.sh.

## Power collapse (CC22, 2026-11-05)
Venus powers down 2 s after last use. 8226 has no usable venus0 GDSC, so the stock
power collapse (halt AXI, GDSC off = block reset, TZ resume restarts the firmware) is
emulated: the SMMU (kernel, `reset_on_suspend`) asserts VENUS0_RESET when it suspends and
restores TZ's secure config on resume; the Venus core cycles the SMMU synchronously before
resuming a collapsed core, releases the VBIF halt and clears the stale boot status. Needs
the CC22+ kernel; older kernels must use the always-on modules (pm_runtime_forbid).
Tests: pc-test.sh (cycles), pcsoak.sh (randomized gaps). Known: unloading venus_core
latches an SMMU failure (TLB sync timeout); reload needs a reboot.

## Auto-load (CC23)
`init/superman-venus.rc` + `init/superman-venus.sh` load the stack once boot completes
(allowlisted kernel + hash-checked files), set /dev/video* to media:media 0660 (the Codec2 HAL runs as user media) and
publish `vendor.superman.venus.{state,decoder,encoder}`. Rollback:
`/vendor/etc/superman/cc23-venusautoload-rollback/rollback.sh`.

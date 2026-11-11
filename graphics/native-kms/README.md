# Lumia 730 native display (default from CC10)

Replaces Codex's firmware-KMS handoff and CPU frame copy with mainline msm:
MDP5 DMA0 -> CTL0 -> INTF1 -> DSI0 (4 lanes, 64.5 MHz) -> Burton panel.

Pieces
- `prebuilt/msm.ko` (e6243238): msm with `msm-cc5c.patch` plus `msm-cc5d-dsi-backlight.patch`
  (DSI commands no longer reapply link clock rates while the link is up, and
  the 8226 PLL set_rate keeps VCOLPF 0x71; fixes backlight flashes/lines and
  ~56 ms per DCS brightness write). cc5c: P33 VRAM scanout
  without a display MMU, MDP addfb guard for non-contiguous BOs, and the
  Superman 28nm-8226 DSI PHY quirk (UEFI LDO regulator/lane/timing values,
  PLL VREG_CFG 0x02, VCOLPF 0x71, GLB_CFG |= 0x10 after lock). Root cause of
  the green noise was the PLL loop filter/LDO (CC5).
- `prebuilt/panel-nokia-burton.ko`: graphics/panel/panel-nokia-burton.c.
- drm_hwcomposer (external/drm_hwcomposer, `drm_hwcomposer-nonblock.patch`):
  non-blocking page flips that wait only on the previous flip's out fence;
  SF present+fences 15 -> 6 ms per frame (CC8).
- gralloc.superman_native (P34) with ro.superman.gpu_display=true so SF's
  client target is BGRA 8888 (HW_FB scanout format).
- `../native-scanout/p35/superman-native-startup.sh` phase `kms`
  (`kms_profile` at post-fs: deps, msm render_only=0 vram=32m, panel, wait
  for DSI-1 connected, select drm stack; `--kms-ui` from `zz-superman-kms.rc`
  at boot: HALs, SF, release main/late_start/zygote).
- Boot: `prepare-boot.py` = CC4b kernel/ramdisk/cmdline + CC5 display DTB.
  LK repeats its cached cmdline before the image's, so a changed stage token
  always conflicts; the contract phase selects kms instead. Expected running
  FDT 9e5f71f7 by the LK replay model (byte-exact on the CC5 test boot).

Hardware notes: MDP5 8x26 has 3 pipes (VIG/RGB/DMA), one display mixer and
7 SMP blocks, so overlay offload can take at most 1-2 layers; composition
stays GPU client-only (hwc.drm.use_overlay_planes=0).

Panel power notes (CC15/CC16): the panel stays dark through prepare (brightness 0x00),
turns on in .enable after video starts, and runs the whole off sequence (display off,
53 00, sleep-in, 120 ms settle) in .disable while the DSI link is up. Without the settle
the AMOLED leaves ELVDD/ELVSS on: a brightening green line while off and a green burst
on wake. S4/L6/L15 stay on at screen-off (NFC and touch share them); TLMM GPIO110/111
are inputs and do not gate panel power on this unit. msm cc5e paints underflows black.

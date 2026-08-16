# Nokia Lumia 730 (Superman) AOSP 11 bring-up

> [!WARNING]
> This is an experimental, pre-boot Android port. No published image is known
> to boot on Lumia 730 hardware yet. Keep a verified recovery FFU and EFIESP
> backup before attempting any bootloader or storage change.

This repository is intended to live at `device/nokia/superman` inside an
`android-11.0.0_r48` source checkout. It contains original device integration
source and offline validation notes. Generated kernels, boot images, Microsoft
firmware, partition dumps, and device-specific backups are intentionally not
part of the repository.

Generate the required appended kernel payload after building the companion
Linux tree:

```bash
device/nokia/superman/scripts/prepare-kernel-prebuilt.sh \
  "$ANDROID_BUILD_TOP/kernel/lumia-superman"
```

This directory is an experimental Android 11 product configuration for the
Nokia Lumia 730 Dual SIM (RM-1040, `superman`). It is not a functional Android
port and its current `boot.img` must not be deployed to a phone.

## Verified identity and boot chain

The device identity used by the kernel project is:

```text
model: Nokia Lumia 730
compatible: lumia,superman / qcom,msm8226
```

Mainline4Lumia does not publish a Superman-specific Android device/product
repository. The known Mainline4Lumia Android repositories provide a common
layer and configurations for other Lumia devices, so this directory is a new
bring-up configuration rather than an imported known-good device tree.

The verified boot path is:

```text
Lumia stock UEFI / Windows Boot Manager
  -> \Windows\System32\BOOT\bootshim.efi
  -> \Stage2.efi
  -> root-level \emmc_appsboot.mbn
     (the Stage2-compatible lk1st ARM32 ELF, renamed at deployment)
  -> lk1st-msm8226
  -> legacy Android boot.img
```

The audited lk1st source is branch `msm8226-messy-lk1st-lumia`, commit
`b973657887cdf8551a8c77205e0106da9c168ef8`. Its filesystem boot path probes
microSD (`hd2`) before eMMC (`hd1`) and loads a root-directory filename whose
first seven characters are `boot.img`. Use the unambiguous name `boot.img`.

## Legacy boot-image contract

lk1st forces these load addresses:

```text
header version: 0
page size:      2048
kernel:         0x00008000
ramdisk:        0x02000000
tags / DTB:     0x01e00000
```

The kernel payload is the existing mainline ARM zImage followed immediately
by one ordinary DTB. The Android header has no separate DT payload
(`dt_size = 0`). lk1st therefore scans the kernel payload and selects the sole
appended DTB without Qualcomm board-ID matching.

The offsets are repeated in `BOARD_MKBOOTIMG_ARGS` because Android 11's build
logic consumes `BOARD_KERNEL_BASE` and `BOARD_KERNEL_PAGESIZE` but does not
consume the three `BOARD_*_OFFSET` variables by itself.

## Offline-validated normal boot artifact

Build command:

```bash
source build/envsetup.sh
lunch aosp_superman-userdebug
m -j8 bootimage
```

Output:

```text
out/target/product/superman/boot.img
SHA-256: 00bb2a2e988e32246ec7c0fb987f2b7c4811f423bf2e1b84e6e36118df33b4fc
```

Validated properties:

```text
boot magic:       ANDROID!
header version:   0
page size:        2048
kernel size:      8,743,549 bytes
kernel address:   0x00008000
ramdisk size:     782,701 bytes
ramdisk address:  0x02000000
second size:      0
tags address:     0x01e00000
command line:     init=/init androidboot.hardware=superman
                  androidboot.selinux=permissive printk.devkmsg=on
                  buildvariant=userdebug
```

The unpacked kernel SHA-256 is
`b7b7341171658db0f448b5eda12e422110cdd9571a8d60dbaa68b5d32de1ed85`.
It is byte-identical to `prebuilt/zImage-dtb`. Its first 8,713,320 bytes are
the existing zImage and its final 30,229 bytes are the existing Superman DTB.
No kernel rebuild was performed while creating or correcting the boot image.

This normal boot image is preserved as a layout reference. Its ramdisk does
not contain a usable diagnostic userspace, so it remains unsuitable for the
first hardware test.

## Offline-validated recovery diagnostic artifact

A separate recovery-style image provides a self-contained, headless ADB
diagnostic environment without changing the prebuilt kernel:

```text
out/target/product/superman/recovery.img
size:    15,519,744 bytes
SHA-256: add4f2a5b114144706542f50393447d3d4d5d9365d8a87f39f5730d3a8c1a898
```

The unpacked image has the exact lk1st contract:

```text
boot magic:       ANDROID!
header version:   0
page size:        2048
kernel size:      8,743,549 bytes
kernel address:   0x00008000
ramdisk size:     6,771,660 bytes
ramdisk address:  0x02000000
second size:      0
tags address:     0x01e00000
separate DT size: 0
command line:     init=/init androidboot.hardware=superman
                  androidboot.selinux=permissive printk.devkmsg=on
                  buildvariant=userdebug
```

The unpacked kernel SHA-256 is
`b7b7341171658db0f448b5eda12e422110cdd9571a8d60dbaa68b5d32de1ed85`,
which is byte-identical to `prebuilt/zImage-dtb`. The normal `boot.img` also
retains its documented SHA-256, so producing the recovery artifact did not
replace it.

The recovery ramdisk was extracted and checked offline. It contains root
`/init` as a symlink to `/system/bin/init`, the recovery init script at
`/system/etc/init/hw/init.rc`, `init.recovery.superman.rc`, `prop.default`,
`sepolicy`, `adbd`, `sh`, Toybox, the ARM linker, and all required shared
libraries. All 70 ELF files under `system/bin` and `system/lib` were checked;
none has an unresolved `DT_NEEDED` dependency. The four principal programs
(`adbd`, `init`, `sh`, and `toybox`) are 32-bit ARM executables using
`/system/bin/linker`.

The main recovery script imports `/init.recovery.${ro.hardware}.rc`; the
kernel command line supplies `androidboot.hardware=superman`. The Superman
hook selects configfs and ADB, while `prop.default` sets `ro.adb.secure=0`.
Recovery init mounts FunctionFS, starts `adbd`, and binds the gadget to
`${sys.usb.controller}` after FunctionFS becomes ready. Android init obtains
that property from the first entry registered in `/sys/class/udc`.

`system/etc/recovery.fstab` contains comments only and has zero device or
mount entries. Merely booting this ramdisk therefore has no configured path
that mounts or writes Lumia eMMC. This is an offline property, not a guarantee
that the untested kernel and device tree cannot affect hardware.

## Remaining limitations

Offline validation cannot establish that the recovery image boots or exposes
an observation channel on this phone:

- The boot ramdisk contains only first-stage `init` and mount-point
  directories. It does not contain `adbd`, `init.superman.rc`, property files,
  or the Android system/vendor trees.
- `init.superman.rc` is currently copied to the future vendor tree, not into
  the boot ramdisk. The USB properties in `device.mk` likewise do not make
  this boot image self-contained.
- The recovery ramdisk provides ADB userspace, but there is no debug UART or
  expected early display output if USB does not enumerate.
- The kernel has `CONFIG_USB_CHIPIDEA_UDC=y`, `CONFIG_USB_CONFIGFS=y`, and
  `CONFIG_USB_CONFIGFS_F_FS=y`; whether the controller probes and registers a
  UDC on this device remains a hardware-test question.
- `CONFIG_ANDROID_BINDER_IPC` is disabled, so Android framework services
  cannot run with this kernel.
- `CONFIG_DRM_SIMPLEDRM` and `CONFIG_FB_SIMPLE` are disabled. `CONFIG_DRM_MSM`
  is modular and its module is not in the ramdisk, so early display output is
  not expected.
- The custom modern Superman DTS still contains hardware assumptions that
  require stronger device-specific validation.

The normal `boot.img` still has neither screen nor ADB diagnostics and must not
be used for the first test. The recovery image addresses the userspace side of
the ADB path, but actual UDC registration, USB enumeration, and the Superman
device tree remain unverified on hardware. A failed recovery boot could still
be in lk1st, DT selection, kernel init, UDC probe, or userspace.

## Planned reversible microSD test

The first hardware test should use the offline-validated recovery diagnostic
image on a dedicated removable microSD:

```text
microSD
  one lk1st-readable ext2-compatible filesystem
  /boot.img    legacy image starting at byte zero
```

The card test is only reversible if the audited lk1st payload has already
been integrated into the verified UEFI chain with a separately reviewed
EFIESP change. Removing the card should then make lk1st continue to its eMMC
lookup. Do not create a raw `boot` partition or overwrite Lumia eMMC for the
first test.

Minimum success criteria for the first boot are host USB enumeration followed
by a stable ADB connection and collection of `dmesg`, `/proc/cmdline`, the
mounted filesystem list, and the live device tree. A blank screen alone is
not a failure signal because early framebuffer support is currently absent.

## Recovery and rollback references

Keep these artifacts off the test microSD and verify their hashes before any
future phone-storage change:

```text
RM1040-post-unlock-EFIESP.bin
SHA-256: 1566fc048261d0d82d3ff37013dfa94a1573b060fd5b1ef90e1f7e5e97c4baf1

RM1040_02040.00021.15235.30007_RETAIL_prod_signed_1004_02698C_000-VN.ffu
SHA-256: c6804b2d61959d6317c3ece41e1343eb363601e0e0da0e5f2ea90babdb3ce4b1
```

The next engineering step is a separately reviewed, explicitly authorized
removable-microSD deployment of the recovery diagnostic image through the
verified lk1st path. No phone-storage or EFIESP change is implied by this
offline validation. Kernel configuration changes for Binder and display come
only after that boot-path test provides an observation channel.

# Developing the Lumia 730 (superman) Android port

This is the map a new developer needs: how the phone boots, where each piece of source lives, how the shipped
image is actually produced, what will bite you, and what is still missing. It describes the state of the alpha
of 2026-11-11. Feature status for users is in the release `README.md`.

Start with section 4: the tools in `build/` take you from a change to images for a phone. Section 5 says what
is still missing: the system image is not yet the output of a plain AOSP build of this tree.

## 1. Boot chain

    UEFI (Nokia) -> bootmgr + BCD (EFIESP) -> bootshim.efi -> Stage2.efi -> emmc_appsboot.mbn (LK, "lk1st")
      -> boot.img (zImage + appended DTB + ramdisk) -> first-stage init -> /system -> Android

- **bootshim.efi / Stage2.efi** come from Mainline4Lumia LumiaQuickStart (no licence stated; never redistributed).
- **LK** is Mainline4Lumia lk2nd (branch `msm8226-messy-lk1st-lumia`), with two small binary patches by this
  project (see the release `LICENSES.md`). It boots `boot*.img` found on an ext2 filesystem ("fsboot"): first on
  a microSD card, then on the eMMC, where it only looks at partitions named `system`, `cache` or `userdata`.
  That is why the internal boot partition is called `cache`.
- LK's ext2 driver only accepts the `sparse_super` and `large_file` features. Its GPT reader rejects a protective
  MBR that runs past the end of the disk and then ignores the whole GPT; Windows Phone writes exactly such an MBR
  (see `internal-install/superman-gpt.c`).
- The **boot menu** cannot be navigated with keys: Volume Up starts the boot shim, the BCD default decides
  otherwise. `firmware-setup/bootmenu/` changes the default from Android.
- **Recovery from a bad boot.img:** hold Volume Down *after* LK starts (not at power-on) for lk2nd fastboot
  (USB `18d1:d00d`), then `fastboot boot <good boot.img>`. That RAM-boots without writing anything, and is also
  the safe way to try a new kernel. Never run `fastboot oem lk_log`: it hangs fastboot.
- Nokia's own bootloader mode (USB `0421:0714`) appears for a few seconds on every start, before the boot menu.
  WPinternals uses it to flash the stock FFU, which is the tested way back to Windows Phone.

## 2. Storage layouts

| | microSD | internal eMMC |
|---|---|---|
| boot | p1, ext2 `LUMIA_BOOT`, holds `boot.img` | GPT entry 27 `cache`, 64 MiB, same content with `boot-internal.img` |
| system | p2, ext4 (system-as-root) | entry 30 `android_system`, 2 GiB |
| data | p3, ext4, 1 GiB | entry 31 `android_data`, the rest (data filesystem 2.5 GiB) |

The internal layout reuses only Windows Phone's Data partition (entry 27); every other entry stays byte for byte.
Internal fstabs use `/dev/block/platform/soc/f9824900.mmc/by-name/*`: Android 11 first-stage init matches GPT
partitions by name. After SELinux loads, the ext2 driver refuses to mount, so ext2 filesystems are mounted as
type ext4. The SD ramdisk's `/init` is a small pre-init logger that needs an SD `LUMIA_BOOT`; the internal ramdisk
uses Android's init directly.

## 3. Where the source is

| What | Where |
|---|---|
| Device tree, HALs, init scripts, out-of-tree module sources, kernel/AOSP patches, installers | this repository |
| Kernel | `doletrandat/linux`, branch `lumia-superman-6.16-rc4` (see section 5 for its limits) |
| Exact shipped kernel tree + config | release `source/linux-6.16-rc4-superman.tar.xz` |
| Edited AOSP files (110 files against `android-11.0.0_r46`) | release `source/modules-and-aosp-changes.tar.xz`, `aosp-changes/` |
| Camera CCS/CAMSS, NFC and other out-of-tree module sources not kept here | same archive, `out-of-tree-modules/` |
| Shipped device tree, decompiled | release `source/boot-dtb/` |

In this repository, by area:

- `graphics/` native KMS display path (`native-kms/`), the panel driver, gralloc; Mesa patches.
- `audio/` codec, ALSA HAL, Bluetooth audio policy. `media/venus/` hardware video (kernel patches, loader,
  Codec2 HAL config, test tools). `camera/` GPU ISP daemon, V4L2 feed module, torch HAL.
- `modem/`, `provision/` modem start, per-phone modem seeds. `gnss/` GPS HAL. `wifi/`, `thermal/`.
- `power/` battery current and gauge, charger watchdog, cpufreq, cpuidle. `memory/zram/`.
- `firmware-setup/` first-boot firmware build from Windows Phone, DPP addresses, boot-menu tool.
- `internal-install/` install to the eMMC from a running SD Android (GPT tool, phone and PC scripts).
- `windows-setup/` guided Windows installer (`Superman-Setup.ps1`) with tests in `tests/`.
- `scripts/` boot image tooling (`prepare-quarantine-boot.py` splits/joins boot images and computes the boot id,
  `verify-arm-boot-layout.py` checks load addresses).

Most folders have their own README with the reasoning and the measurements behind them.

## 4. Tools in `build/`

Everything needed to go from a source change to images a phone can run. Each tool reproduces the released
artifacts exactly when given the released inputs; that is how they were checked.

| Tool | What it does |
|---|---|
| `build-kernel.sh SRC OUT` | builds zImage, modules and DTBs the way the shipped kernel was built; prints the config and build-notes hashes the phone pins |
| `boot-tool.py` | `info`, `split`, `build` (swap zImage / DTB / ramdisk, edit the command line), `internal` (internal-storage variant), `predict-fdt` |
| `update-contracts.py` | rewrites the two boot contracts and the file manifest in a file tree |
| `make-images.py` | from a **published SD image** + a boot image + changed files: a new SD image and the three internal partition images, with all contracts and the manifest updated inside |
| `reference/runtime-sd-cc60.fdt` | the phone's runtime device tree with the released SD boot image (input for `predict-fdt`) |

`make-images.py` starts from a released SD card image, so nothing from the original build host is needed. It
needs e2fsprogs and about 5 GB of free space, and should run on ext4 (elsewhere a superblock write counter
differs and with it the hashes, though not the contents).

### Typical changes

**A system file (script, config, library):** put the new file under an overlay tree at its phone path and run

    build/make-images.py --base superman-alpha-sdcard.img --overlay my-overlay --out out

Owner, mode and SELinux label of the old file are kept; the manifest line is updated. Only files that already
exist can be replaced this way.

**The kernel command line:**

    build/boot-tool.py build --base boot.img --cmdline-replace OLD NEW -o new-boot.img
    build/boot-tool.py predict-fdt --old-boot boot.img --old-runtime-fdt build/reference/runtime-sd-cc60.fdt \
        --new-boot new-boot.img -o new-runtime.fdt
    build/make-images.py --base superman-alpha-sdcard.img --boot new-boot.img --runtime-fdt new-runtime.fdt --out out

(`boot.img` is the file on the SD image's first partition: `debugfs -R "dump /boot.img boot.img" <partition>`,
or `adb pull /lumia_sd/boot.img`.)

**A new kernel:**

    build/build-kernel.sh linux-6.16-rc4-superman kout
    build/boot-tool.py build --base boot.img --zimage kout/arch/arm/boot/zImage -o new-boot.img

A new kernel has a new build-notes hash, and a changed DTB cannot be predicted: RAM-boot it
(`fastboot boot new-boot.img`), then `adb pull /sys/firmware/fdt` and `adb shell sha256sum /sys/kernel/notes`.
Pass the FDT as `--runtime-fdt` and the hashes as `--config-sha` / `--notes-sha` to `make-images.py`. Add the
notes hash to `media/venus/init/venus-kernels.txt` and the camera allowlist, and the config hash to the list in
`graphics/native-scanout/p35/superman-native-startup.sh`, and rebuild the out-of-tree modules against the new
kernel (they are in the image under `/vendor/lib/modules`; replace them through the overlay).

**After any image change:** `make-images.py` writes `IMAGES.json`. Its `windows_setup_tool` block lists the
values to put into `windows-setup/Superman-Setup.ps1` (`$KitHashes`, `$InternalImages`, `$ImageSha`,
`$ImageGzSha`) and `internal-install/superman-install-internal.sh` (`RELEASE_BOOT`, `INTERNAL_BOOT`,
`INTERNAL_FDT`).

## 5. What is still not in the repositories

- **A build of the system image from source.** The released system image was assembled once from an older full
  image plus about 170 files captured from the running phone. Nobody has produced it from `make`. The practical
  base for development is therefore the published image plus `make-images.py`. AOSP `android-11.0.0_r46` with
  the files from the release's `aosp-changes/` overlaid (lunch `aosp_superman`) builds the HALs and libraries
  that you then put into the image through an overlay.
- **The readable device tree source for everything that ships.** The kernel repository builds the shipped tree
  from `qcom-msm8226-nokia-superman-shipped.dts`, a decompiled file. Venus, the cameras, the CMA region, the
  address placeholders and the command line still have to be folded back into `qcom-msm8226-nokia-superman.dts`.
- **Three kernel files where the repository is ahead of the shipped tree** (Wi-Fi scan guard, audio DSP manual
  boot, battery-current driver): the shipped modules are built from separate copies in this repository.
- **The development ledger** (about 17,000 lines of experiments and hashes) and the prebuilt modules and
  libraries (`*/prebuilt/`, ignored by git).

Out-of-tree modules are plain kbuild `M=` builds against the kernel output tree; each module folder has a
`Makefile`. Their vermagic must match the running kernel. The DTB is appended to the zImage inside boot.img;
LK copies its `/chosen/bootargs` and appends the boot header's command line, so the command line appears twice
at runtime and the last value wins.

## 6. Hash checks ("contracts")

The system refuses to bring up parts of itself when files or the kernel differ from what was qualified. This
catches accidents, and it will stop you after almost any change unless the contracts are updated.
`build/make-images.py` and `build/update-contracts.py` do that for you; this is what they maintain:

| File on the phone | Pins | Read by |
|---|---|---|
| `/vendor/etc/superman-native-boot.contract` | sha256 of `boot.img`, display phase | native display startup |
| `/vendor/etc/superman/audio-boot.contract` | boot.img, **runtime FDT**, kernel config, kernel build notes | audio, battery, modem |
| `/vendor/etc/superman-native-components.sha256` | about 120 files, including `/system/build.prop` | native display startup, modem |
| `venus-kernels.txt`, `camera-kernels.txt` | allowed kernel build notes | Venus and camera loaders |

- Changing the kernel **or its command line** changes the runtime FDT (`/sys/firmware/fdt`).
- When something silently stays off, look here first: `getprop | grep vendor.superman`, and
  `SUPERMAN_NATIVE_STARTUP FAILED stage=component_hashes` on the text console means a manifest mismatch.
- Editing a file directly on a running phone needs its manifest line updated in the same step.
- The Windows setup tool pins the sha256 of every image it writes.

A switch that turns these checks off for development does not exist yet. Adding one means editing shipped
scripts, which needs a phone to test.

## 7. First boot on a phone

Nothing proprietary and nothing per-phone ships. On first boot `firmware-setup/superman-firmware.sh` mounts
Windows Phone's MainOS read-only, builds the firmware set (allowlisted by hash: only the test phone's firmware
version is known), decrypts the modem images through TrustZone, reads the Wi-Fi/Bluetooth addresses from DPP,
and `provision/` copies the phone's own modem file systems. Other firmware versions report "firmware version
not supported yet": adding one means adding its hashes to `firmware-setup/firmware-allowlist.txt`.

## 8. Working on the phone

- adb is root-capable (`adb root` or `su 0`). SELinux is permissive.
- State of the main services: `getprop | grep vendor.superman` (`native_result`, `audio_core.state`,
  `modem.state`, `venus.state`, ...). Logs of each start script are under `/data/vendor/`.
- Try a kernel with `fastboot boot` before writing it. Audio and the modem will stay off in such a test boot
  unless the contracts match; that is expected.
- 32-bit userspace limits: toybox `losetup -o` and `dd seek` fail above 2 GiB, `mksh` arithmetic is 32-bit
  (count in sectors), toybox `grep` fails on pipes and `/proc/mounts`, toybox `umount` frees loop devices
  (use `umount -D`).
- `adb` reads stdin: in scripts, give every call `</dev/null`.
- Never pipe `dumpsys media.audio_flinger` into `grep -m1`/`head`: the audio HAL dies on the closed pipe. Dump
  to a file first.
- `svc power stayon true` survives reboots; reset it (`settings put global stay_on_while_plugged_in 0`).
- The microSD pre-init logger rewrites its log on the boot partition at every start, so that partition's hash
  changes per boot.

## 9. Testing without a phone

`windows-setup/tests/` runs the Windows installer's flows on Linux with PowerShell 7: a disk image carrying a
real partition table stands in for the phone, a file for the SD card, a script for adb. `gpttest.ps1` checks the
partition planner against tables produced by `superman-gpt` on the test phone. These tests replace the raw disk
layer, so they do not prove the Windows side.

## 10. Known gaps, roughly by value

1. Deep sleep (the phone never suspends).
2. Calls, SMS and mobile data on a real network; earpiece and headset audio routing.
3. A build of the system image from source, and a readable device tree source for everything that ships (section 5).
4. A way to disable the hash checks for development (section 6).
5. Camera autofocus and LED flash; GPS position fix; battery percentage accuracy.
6. Support for more firmware versions (section 7) and a second test phone.
7. The Windows setup tool on real hardware.

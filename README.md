# Android 11 for the Nokia Lumia 730 (superman)

AOSP 11 device tree for the Nokia Lumia 730 Dual SIM (RM-1040, codename `superman`, Qualcomm MSM8226), running
on a mainline-based Linux 6.16 kernel with Mesa freedreno. Android installs on the phone's own storage; running
it from a microSD card and keeping Windows Phone is optional.

> **Alpha.** Expect bugs. Everything here was tested on **one phone**. A wrong step can damage the phone's boot
> setup. You do this at your own risk.

> **AI disclosure.** This port was brought up with heavy use of generative AI coding assistants (OpenAI Codex and
> Anthropic Claude). See [AI-assisted development](#ai-assisted-development).

- [Status](#status)
- [Device specifications](#device-specifications)
- [Installing](#installing)
- [Going back to Windows Phone](#going-back-to-windows-phone)
- [What is in this repository](#what-is-in-this-repository)
- [Building and developing](#building-and-developing)
- [Reporting problems](#reporting-problems)
- [Credits](#credits)
- [AI-assisted development](#ai-assisted-development)
- [Licence](#licence)

## Status

State of the alpha of 2026-11-11, verified on the test phone unless the row says otherwise.

| Feature | Description | Working state |
|---|---|---|
| Boot | From the internal eMMC and from a microSD card | Working |
| Display panel | 4.7-inch AMOLED, 720 × 1280, native KMS, brightness control | Working |
| GPU | Adreno 305 with Mesa freedreno, GLES 3.0 | Working |
| Touch | Synaptics RMI4 | Working |
| Side buttons | Power, Volume Up, Volume Down | Working |
| eMMC | Internal storage with DMA, reads of about 80 MB/s | Working |
| SD Card slot | microSD with DMA, reads of about 40 MB/s | Working |
| Wireless | Wi-Fi 2.4 GHz (WCN3620) | Partially working (scanning for other networks is refused while connected; it caused freezes) |
| Bluetooth | Bluetooth, including Bluetooth audio | Working |
| Audio | Loudspeaker and built-in microphone | Working |
| Audio routing | Earpiece, headphone jack, headset microphone | Not working (not implemented) |
| Video | Hardware H.264 decode up to 1080p, H.264 encode, screen recording (Venus) | Working |
| Cameras | Rear and front: full-resolution photos, 720p video | Partially working (no autofocus; photo preview runs at about 10 to 12 fps) |
| Camera flash | Rear LED flash and torch | Not working |
| Cellular | Modem starts, the SIM is recognised | Partially working (calls, SMS and mobile data are implemented but untested on a real network) |
| Dual SIM | Second SIM slot | Untested (the slot is detected) |
| Battery | Charging, charging status, battery current | Partially working (the percentage may be inaccurate) |
| Sensors | Light and proximity | Working |
| Sensors | Accelerometer, screen rotation | Working |
| Sensors | Magnetometer / compass | Partially working (raw, uncalibrated values only; no heading) |
| GPS | QMI LOC receiver | Partially working (sees satellites; no position fix achieved yet) |
| NFC | NXP NCI controller | Working |
| Vibration | PM8226 vibration motor | Working |
| USB | adb (root), device mode | Working |
| USB OTG | Host mode | Not working - the phone's hardware cannot power the USB port |
| Deep sleep | Suspend to RAM | Not working (the phone never suspends, so battery life is short) |
| FM radio | | Not working (not implemented) |
| Return to Windows Phone | Flashing the stock FFU with WPinternals | Working |
| Windows setup tool | Guided installer for Windows 10 and 11 | Untested (passes its tests on Linux against stand-ins; not yet run on Windows against a real phone) |

Other things to know:

- This is a debug build: adb root, SELinux permissive, kernel messages on screen during boot.
- Only the firmware version of the test phone is supported for now (checked by hash). On other versions Android
  still starts, but audio, Wi-Fi and the modem stay off.
- Other Lumia 730 variants are untested. The Lumia 735 has a different chip and is **not** supported.

## Device specifications

The test phone. Values were read on the device where possible.

| Component | Details |
|---|---|
| Model | Nokia Lumia 730 Dual SIM, type **RM-1040**, codename superman |
| Product code | 059W5H0 (hardware ID 3001) |
| Chipset | Qualcomm Snapdragon 400 **MSM8226** |
| CPU | 4 × ARM Cortex-A7 (32-bit) |
| GPU | Adreno 305 |
| Modem | 3G, no LTE |
| Memory | 1 GB (Linux sees 972,544 kB) |
| Storage | 8 GB eMMC (chip name 8WMB3R, 7.28 GiB, HS200) |
| microSD | Slot present; cards run in UHS-I DDR50 mode |
| Display | 4.7-inch AMOLED, 720 × 1280, MIPI DSI |
| Touch | Synaptics RMI4 (I2C) |
| Power | Qualcomm **PM8226** PMIC (charger, RTC, vibration motor) |
| Wi-Fi / Bluetooth | Qualcomm **WCN3620** (2.4 GHz) |
| Sensors | Kionix KX022 accelerometer, AKM AK09911 magnetometer, Avago APDS-9930 light and proximity. No gyroscope |
| NFC | NXP NCI controller (I2C) |
| Cameras | Rear and front, both MIPI CCS sensors; rear with autofocus actuator and LED flash |
| USB | USB 2.0, micro-B, device mode only |
| Stock OS | Windows Phone 8.1 / Windows 10 Mobile |

Software in this port:

| Component | Version |
|---|---|
| Android | AOSP 11, tag `android-11.0.0_r46` (`RD2A.211001.002`) |
| Kernel | Linux 6.16-rc4, mainline-based |
| Graphics | Mesa freedreno, drm_hwcomposer, GBM gralloc |
| Bootloader | lk2nd ("lk1st", Mainline4Lumia), started from the Windows boot manager through a boot shim |

## Installing

Images, the Windows setup tool and the full installation guide are in the
[releases](https://github.com/doletrandat/android_device_nokia_superman/releases). This repository holds source
only: no images, no firmware, no per-phone data.

In short, you need:

- a Lumia 730 with its Windows Phone partitions intact (the phone builds its own firmware set from them on
  first boot);
- a Windows PC with [WPinternals](https://github.com/ReneLergner/WPinternals) to unlock the bootloader, and the
  Lumia USB drivers;
- **the stock FFU of your phone**, saved somewhere safe. It is the way back to Windows Phone.
  [LumiaDB](https://lumiadb.com) has the FFUs and an unlock guide.

Then run `Start-Here.cmd` from the release's `Lumia730-Android-Setup` folder and choose
"Install Android on the phone". The first start takes about 3 minutes.

## Going back to Windows Phone

Flash the stock FFU with WPinternals (Flash, then "Flash original FFU"). This removes Android, the boot shim and
the bootloader unlock; the phone's calibration and addresses are kept. This path was tested with Android
installed on the internal storage.

If Android only lives on a microSD card, Windows Phone is still installed: run `superman-bootmenu windows` as
root in Android and restart.

## What is in this repository

The tree belongs at `device/nokia/superman` in an AOSP 11 checkout.

| Folder | Contents |
|---|---|
| `graphics/` | Native KMS display path, panel driver, gralloc, Mesa patches |
| `audio/` | Codec, ALSA HAL, Bluetooth audio policy |
| `media/venus/` | Hardware video: kernel patches, loader, Codec2 HAL config, test tools |
| `camera/` | GPU ISP daemon, V4L2 feed module, torch HAL |
| `modem/`, `provision/` | Modem start, RIL, rmtfs, per-phone modem seeds |
| `gnss/`, `wifi/`, `thermal/` | GPS HAL, Wi-Fi driver copy, thermal HAL |
| `power/`, `memory/` | Battery current and gauge, charger watchdog, cpufreq, cpuidle, zram |
| `firmware-setup/` | First-boot firmware build from Windows Phone, boot-menu tool |
| `internal-install/` | Install to the eMMC from a running microSD Android |
| `windows-setup/` | Guided Windows installer and its tests |
| `build/` | Kernel build, boot image, contract and image tools |
| `diagnostics/`, `scripts/` | Bring-up tools and boot image helpers |

Related sources:

| What | Where |
|---|---|
| Kernel | [`doletrandat/linux`](https://github.com/doletrandat/linux), branch `lumia-superman-6.16-rc4` |
| Exact shipped kernel tree and config | release `source/linux-6.16-rc4-superman.tar.xz` |
| Edited AOSP files (110, against `android-11.0.0_r46`) | release `source/modules-and-aosp-changes.tar.xz` |

## Building and developing

Start with [DEVELOPING.md](DEVELOPING.md): boot chain, storage layouts, the tools in `build/`, the hash checks
that guard the system, and the known gaps.

Be aware that the released system image is **not yet the output of a plain AOSP build** of this tree. The
practical route today is the published image plus `build/make-images.py`, which replaces files, the kernel or
the command line and updates every hash for you.

## Reporting problems

Open an issue and include:

- the phone's type and product code (label under the battery);
- the release you installed, and whether it runs from internal storage or microSD;
- `adb shell getprop | grep vendor.superman`, and for firmware problems the hashes that the first-boot setup
  logged.

Reports from phones with another firmware version or product code are especially useful.

## Credits

This port stands on the work of many people. "Used" means code that ships or is built from; "reference" means
it was read to understand the hardware or a protocol.

| Project | Authors | Role here |
|---|---|---|
| [lk2nd](https://github.com/msm8916-mainline/lk2nd) | msm8916-mainline contributors | Used: the Android bootloader, through the Mainline4Lumia fork |
| [Mainline4Lumia](https://github.com/Mainline4Lumia) | Mainline4Lumia community | Used: the lk2nd "lk1st" Lumia branch, LumiaQuickStart (boot shim installation), and the Lumia 730 device tree that the kernel's is based on |
| Little Kernel (LK) | Travis Geiselbrecht; Qualcomm / The Linux Foundation | Used: the base of lk2nd |
| [boot-shim](https://github.com/imbushuo/boot-shim) | imbushuo | Used: the UEFI shim that starts LK from the Windows boot manager (installed by LumiaQuickStart, not redistributed here) |
| [WPinternals](https://github.com/ReneLergner/WPinternals) | René Lergner | Used: bootloader unlock, Mass Storage Mode, FFU restore |
| [LumiaDB](https://lumiadb.com) | | Used: stock FFUs and the unlock guide |
| [Linux kernel](https://kernel.org) | Kernel developers, in particular the Qualcomm MSM8226 and Lumia device-tree contributors | Used: the kernel and the MSM8226 / Microsoft Lumia device trees |
| wcn36xx | Eugene Krasnikov and contributors | Used: Wi-Fi driver (patched copy in `wifi/`) |
| qcom_bam_dmux | Stephan Gerhold | Used: modem data driver |
| Venus, CAMSS, CCS, ntfs3 drivers | Linaro, Qualcomm, Intel, Paragon Software and contributors | Used: hardware video, camera pipeline, camera sensors, reading Windows Phone partitions |
| [Mesa](https://mesa3d.org) freedreno | Rob Clark and the Mesa developers | Used: the GPU driver |
| [libdrm](https://gitlab.freedesktop.org/mesa/drm), [drm_hwcomposer](https://gitlab.freedesktop.org/drm-hwcomposer/drm-hwcomposer), minigbm | freedesktop.org, Linaro, Google / ChromiumOS | Used: display stack |
| gbm_gralloc | Rob Herring (Linaro), Chia-I Wu, LunarG | Used: the base of `graphics/gralloc` |
| v4l2_codec2 | ChromiumOS / AOSP | Used: Codec2 HAL for hardware video (patched) |
| tinyalsa | tinyalsa contributors | Used: audio HAL |
| [rmtfs](https://github.com/linux-msm/rmtfs), [qrtr](https://github.com/linux-msm/qrtr) | Linaro, Sony Mobile | Used: modem remote file system and QRTR library (`modem/rmtfs`) |
| [libqmi](https://gitlab.freedesktop.org/mobile-broadband/libqmi) | libqmi contributors | Reference: QMI message layouts for the RIL (with the Qualcomm Gobi API) |
| [WOA-Project Lumia-Drivers](https://github.com/WOA-Project/Lumia-Drivers) | WOA Project | Reference: hardware identifiers and driver packages for superman. The status table above follows the layout of the [LumiaWOA status page](https://woa-project.github.io/LumiaWOA/status/) |
| [comma.ai MSM8996 kernel](https://github.com/commaai/android_kernel_comma_msm8996) | Qualcomm, comma.ai | Reference: `qpnp-power-on.c` for reading reset reasons |
| Stock Nokia / Microsoft firmware | | Reference: UEFI and ACPI tables read on the test phone (panel sequences, power sequence). Nothing from it is redistributed |
| [postmarketOS](https://postmarketos.org) | postmarketOS community | Reference: mainline knowledge for MSM8226 phones |
| [AOSP](https://source.android.com) | Google and contributors | Used: Android itself |

If your work is used here and is missing or described wrongly, please open an issue.

## AI-assisted development

Most of the code, scripts and documentation in this repository were written with generative AI coding
assistants (OpenAI Codex and Anthropic Claude), directed and tested on a real phone by the maintainer.

What that means for you:

- Statements in the status table were checked on hardware. Anything not checked is marked "Untested".
- The code may contain mistakes that a human author would not make, and comments or READMEs in subfolders may
  describe experiments that were later dropped. When in doubt, trust the code and the measurements.
- Code taken from other projects keeps its original copyright notices and licences; the AI tools did not change
  who wrote it.

Review and corrections are welcome.

## Licence

Apache-2.0 unless a file says otherwise (see [LICENSE](LICENSE)). Kernel module sources and kernel patches are
GPL-2.0. Vendored code keeps its own licence (`modem/rmtfs/upstream/LICENSE`, `modem/rmtfs/qrtr/LICENSE`). The
release's `LICENSES.md` lists every shipped component with its licence and source.

Nokia and Lumia are trademarks of their owners. This project is not affiliated with Nokia, Microsoft, Qualcomm
or Google.

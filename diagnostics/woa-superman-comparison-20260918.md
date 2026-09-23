# WOA Superman package comparison

Date: 2026-09-18
Scope: read-only comparison; no image, source, or device write performed.

## Source identity

- Repository: `WOA-Project/Lumia-Drivers`
- Path: `components/QC8X26/DEVICE.SOC_QC8X26.SUPERMAN`
- Commit inspected: `b5af93f99ea800c28551727df55e57a07ba61a82`
- Superman package files: 86
- Package groups: Audio 10, Camera 49, Graphics 16, Subsystems 7, Touch 2

## Superman identifiers extracted from Windows INFs

| Function | Identifier |
|---|---|
| Panel | `ACPI\\NOKIA_PANEL` |
| Touch | `ACPI\\NOKIA_TOUCH` |
| Qualcomm graphics | `ACPI\\QCOM246F` |
| Rear camera | `ACPI\\QCOM2436` |
| Front camera | `ACPI\\QCOM2437` |
| WCNSS | `ACPI\\QCOM242A` |
| ADSP | `ACPI\\QCOM242B` |
| AMSS/modem | `ACPI\\QCOM242C` |
| Audio calibration | `SLM1\\QCOM242D` |

## Relevant WOA payloads and SHA-256

| File | SHA-256 |
|---|---|
| `Drivers/Graphics/qcvss8626.mbn` | `695679442a143c6955e8ea1c03e1a327bf8826e479e26f240785b18b53d7bba0` |
| `Drivers/Subsystems/qcadsp8626.mbn` | `4d672c211e90c8a56c1066b976cec631fb952aece1457db26d02846e2f2743da` |
| `Drivers/Subsystems/qcwcnss8626.mbn` | `10ab2dc96f9ec8389b9eaa61c877855c058da2f34993d9cd3d64f4799af1e0c1` |
| `Drivers/Subsystems/qcdsp1v28926.mbn` | `71f1956572b952f63397a9581968bc634a992cfbeb8b967e5594d845e732fdcb` |
| `Drivers/Subsystems/qcdsp28926.mbn` | `30d46ec6be52098f31ba8953847fc8972d45bb3b87bf9f3f144bacbea265f94f` |

## Comparison result

No same-named `8626`, `8974`, `qcvss`, `qcadsp`, `qcwcnss`, `qcdsp`, `.mbn`, `.acdb`, `.dcc`, or `.dfc` payload was found in the checked-in `device/nokia/superman` tree or the built `out/target/product/superman` product tree. The source tree contains only the kernel/device configuration and diagnostic tooling; the product tree contains `libEGL_swiftshader.so`, `gralloc.default.so`, and related generic graphics artifacts.

Existing V65 evidence shows the kernel reached the GPU service launch stage, but the captured product was using the generic graphics path. This does not establish that the WOA `qcvss8626.mbn` is compatible with Android or that its absence is the cause of the black screen.

## Important provenance anomaly

The WOA subsystem INF packages `8626` files together with `qcdsp1v28926.mbn` and `qcdsp28926.mbn`, and also contains APQ override references to `qcdsp1v28026.mbn` and `qcdsp28026.mbn`. The latter names do not appear as package files. These entries are therefore treated as Windows-package metadata/possible compatibility remnants, not as Android deployment candidates.

## Assessment

- Confirmed: the WOA package supplies Superman-specific Windows panel, touch, camera, graphics, DSP, WLAN, and audio identities.
- Confirmed: the current AOSP product has no matched WOA proprietary payloads and falls back to generic SwiftShader/default gralloc artifacts.
- Unverified: whether any WOA `.mbn` is byte-identical to a preserved Lumia firmware payload.
- Unverified: whether the current black screen is caused by missing Qualcomm graphics firmware, an incompatible kernel/userspace graphics stack, panel initialization, or the previously observed userspace crash path.

## Preserved-artifact comparison

The following read-only dumps were then supplied and inspected:

- `/run/media/doletrandat/New Volume/EFIESP`
- `/run/media/doletrandat/New Volume/MainOS`

The MainOS dump contains the expected Superman Windows payloads, including `qcvss8626.mbn`, `qcadsp8626.mbn`, `qcwcnss8626.mbn`, `qcdsp1v28626.mbn`, `qcdsp28626.mbn`, `qcdxkm8626.sys`, `qcsubsys8626.sys`, `oempanel.sys`, camera drivers, camera calibration, and audio calibration.

## Hash comparison against the supplied MainOS

Across the 86 unique WOA package filenames: 13 were exact SHA-256 matches, 31 had the same basename but different contents, and 42 were not present under that basename. The exact matches are seven camera calibration files (`01029801.dcc`, `01029802.dcc`, `01029803.dcc`, `0103BB00.dcc`, `0103BB01.dcc`, `01066401.dcc`, `01066402.dcc`), `05000400.dfc`, five audio calibration files (`Bluetooth_cal_8626.acdb`, `Codec_cal_8626.acdb`, `General_cal_8626.acdb`, `Global_cal_8626.acdb`, `Hdmi_cal_8626.acdb`), and the corresponding duplicate copies of those files where present.

The key firmware comparisons were all mismatches:

| Payload | WOA SHA-256 | Supplied MainOS SHA-256 |
|---|---|---|
| `qcvss8626.mbn` | `695679442a143c6955e8ea1c03e1a327bf8826e479e26f240785b18b53d7bba0` | `990d240c4a99486a9a3d24d8ffdaff87e529ea7b92fbd80e31f54f4693594ead` |
| `qcadsp8626.mbn` | `4d672c211e90c8a56c1066b976cec631fb952aece1457db26d02846e2f2743da` | `b8b989a2512c210193c40cb7e674cffafa32497e5ebfa6bcdf828a9400fdf897` |
| `qcwcnss8626.mbn` | `10ab2dc96f9ec8389b9eaa61c877855c058da2f34993d9cd3d64f4799af1e0c1` | `0368e54386f64d33b319a425da41dea32650542f4132b0e3c4368e9108bb2a05` |

The supplied stock dump uses `qcdsp1v28626.mbn` and `qcdsp28626.mbn`, not the WOA package's `qcdsp1v28926.mbn` and `qcdsp28926.mbn`. This explains the earlier `8926` anomaly: those WOA names are not the exact Superman dump names.

The stock Windows graphics stack also differs: the dump contains `qcdx11um8626.dll` and `qcdx11compiler8626.dll`, while the WOA package contains `qcdx9um8974.dll`, `qca3xxcompiler8974.DLL`, and related `8974` user-mode components. The WOA package is therefore not a byte-identical copy of this stock MainOS graphics stack.

## Updated assessment

- Confirmed: the supplied MainOS is a valuable Superman-specific reference and contains the expected Qualcomm/Windows hardware stack.
- Confirmed: several WOA camera-calibration and audio-calibration files are byte-identical to this MainOS dump.
- Confirmed: the WOA graphics and DSP/WLAN firmware blobs are not byte-identical to this MainOS dump.
- Confirmed: the stock Superman dump uses an `8626` DSP naming family, not the WOA package's `8926` DSP filenames.
- Unverified: whether the WOA firmware revisions are compatible with Android or with this specific stock firmware revision.
- Action: do not copy WOA `.mbn`, graphics binaries, or calibration mismatches into Android. Use the supplied MainOS as provenance/reference evidence and continue with source-only Android display/firmware-path analysis.

## Windows registry versus Android device-tree comparison

The WOA subsystem INF exposes these Windows loader reservations:

| Windows function | WOA reservation/configuration |
|---|---|
| ADSP | `0x01400000` reservation, `0x00100000` alignment |
| AMSS | address `0x08000000`, reservation `0x04f00000`, alignment `0x00400000` |
| WCNSS | `0x00600000` reservation, `0x00100000` alignment |
| MBA | `0x00500000` reservation |
| ADSP image | `qcadsp8626.mbn` |
| WCN image | `qcwcnss8626.mbn` |
| AMSS images | WOA package names `qcdsp1v28926.mbn` and `qcdsp28926.mbn` |

The Android device tree has a materially different active/disabled boundary:

- `adsp_region` is `0x0dc00000`/`0x01900000` (25.6 MiB), and the ADSP remoteproc consumes it, but the ADSP node is `status = "disabled"`.
- The generic `mpss_region` is `0x08000000`/`0x05100000` (81 MiB) and `mba_region` is `0x0d100000`/`0x00100000`; both modem-related regions are disabled.
- The Android tree defines WCNSS SMEM state plumbing but no enabled WCNSS remoteproc/firmware loader in the Superman-specific DTS path.
- The MDSS display subsystem is `status = "disabled"`; its MDP and DSI nodes are under that disabled subsystem.
- The Adreno 305.18 GPU node is also `status = "disabled"`.
- Android instead reserves a simple framebuffer at `0x03200000` of 8 MiB and uses the generic SwiftShader/default-gralloc product path.

This is the strongest source-level result so far: the current Android image is intentionally a software/simple-framebuffer bring-up configuration, while the stock Windows system has a hardware graphics and Qualcomm subsystem loader. It explains why the WOA package cannot be treated as a drop-in Android driver set, and it identifies the Android display/GPU enablement boundary that must be investigated before blaming a missing WOA firmware blob.

It is not yet proof that enabling MDSS/GPU or ADSP would fix the black screen. The panel node/initialization sequence and Android-compatible Qualcomm userspace/HAL are still incomplete, and the prior userspace crash evidence remains independent.

## Review-only diff prepared

The proposed source diff is recorded, but not applied, at:

`device/nokia/superman/diagnostics/review-only-mdss-gpu-enable-20260918.patch`

It enables only the already-described `&mdss` and `&gpu` nodes in the Superman
DTS. A dry-run against the current kernel source snapshot passed. The kernel
source tree, Android product configuration, boot image, firmware, and device
were not modified.

The companion review note explains why panel/DSI, ADSP, WCNSS, MPSS/AMSS/MBA,
Android HALs, and userspace graphics libraries are intentionally excluded.
The patch is not expected to produce a visible display by itself because the
current Superman DTS still lacks a panel/DSI endpoint and the product still
selects SwiftShader/default gralloc.

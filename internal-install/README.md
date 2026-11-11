# Installing the alpha to the internal storage (experimental)

This copies the Android you are running from the SD card onto the Lumia 730's internal storage, in the space of
Windows Phone's **Data** partition. Windows Phone stops working: its Data partition is erased. Every other partition
is kept exactly as it was: firmware, calibration, DPP with your Wi-Fi/Bluetooth addresses, EFIESP with the boot menu,
and MainOS. That is what makes the way back possible.

| Partition | Size | Content |
|---|---|---|
| `cache` (was Data) | 64 MiB | boot partition (`boot.img`; the name the Android bootloader looks for) |
| `android_system` | 2 GiB | system |
| `android_data` | about 2.7 GiB | your apps and files (the SD card version has about 1 GB) |

## Before you start

- The phone runs the **alpha from the SD card**, set up and booted at least once, with adb working (`adb devices`).
- Battery at 40 % or more. Keep the phone on the charger.
- About **5 GB free on the PC** for the backups. Keep the backup folder: it is the easy way back to Windows Phone.
- **Download your phone's stock FFU** too.

## Install (Linux)

    ./install-internal.sh install

The script:

1. checks that the phone is a Lumia 730 running the unmodified alpha from the SD card, and that its internal
   storage has the Windows Phone layout;
2. saves to `backup-<date>/`: the partition table (both copies), EFIESP and the whole Windows Phone Data partition
   (`--no-data-backup` skips Data; then only an FFU flash brings Windows Phone back);
3. asks you to type `INSTALL`;
4. writes the new partition table, then the boot partition, system and an empty data partition. Every step is
   checked by reading it back. If anything fails it stops; you can run it again.

Then power the phone off, **take the SD card out** and power it on. The first start takes about 3 minutes (firmware
setup, as on the first SD boot).

An Android SD card in the slot always boots first, so the card stays your rescue system.

## Back to Windows Phone

- **Flash your stock FFU with WPinternals** ("Flash" -> "Flash original FFU"). Tested on the test phone with Android
  installed internally: start the flash in WPinternals, then restart the phone (hold Power about 10 s); WPinternals
  catches it in bootloader mode before the boot menu. This also removes the boot shim, the Android bootloader and
  the bootloader unlock, and Windows Phone starts factory-fresh.
- **With the Data backup (untested):** boot the SD card, then run `./install-internal.sh restore-windows backup-<date>`.
  Then run `superman-bootmenu windows` and restart. This keeps the boot shim and your Windows Phone data.

## Files

`superman-gpt` (partition table tool and 64-bit loop devices, static ARM), `superman-install-internal.sh` (runs on
the phone), `boot-internal.img` (the release boot image with the internal partition list), two fstab files,
`superman-bootmenu` and `install-internal.sh` (PC side). Sources: `superman-gpt.c` and the scripts themselves.

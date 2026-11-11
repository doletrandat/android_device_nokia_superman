Android 11 for the Nokia Lumia 730 (superman) - guided setup for Windows   (ALPHA)
================================================================================

Double-click  Start-Here.cmd  and follow along. Windows asks once for Administrator rights.
Every step gets its own clean screen, and nothing is written before you say so.

What you need
  - Nokia Lumia 730 (RM-1040; not the 735), bootloader unlocked with WPinternals
  - WPinternals on this PC          github.com/ReneLergner/WPinternals (Releases)
  - the Lumia USB drivers           they come with Windows Device Recovery Tool (WDRT). WDRT can no longer
                                    download firmware, but installing it still brings the drivers.
                                    lumiadb.com links an offline installer ("WDRT" in the top bar).
  - the stock FFU of your phone     your way back to Windows Phone. Get it from lumiadb.com:
                                    Lumia 730 Dual SIM (RM-1040) -> the FFU for your product code (on the
                                    label under the battery, "CODE: 059W..."). If yours is not listed,
                                    LumiaDB says any RM-1040 FFU will do.
  - Windows 10 or 11
  - only for the microSD option: a card of 4 GB or more (it is erased) and a card reader

  The "Download" tab inside WPinternals no longer works: Microsoft switched those servers off in 2024.
  The files for unlocking the bootloader (FFU, emergency files, donor FFU) and a step-by-step unlock guide
  are on lumiadb.com as well.

The menu
  1  Install Android on the phone  (the normal way; no microSD card needed)
       - finds the phone's EFIESP volume by itself (phone in WPinternals Mass Storage Mode) and asks you to confirm
       - installs the boot shim (downloads Mainline4Lumia LumiaQuickStart, one fixed version, about 1 MB)
       - installs the Android bootloader and makes it the default boot entry (boot menu backup in backups\)
       - backs up the partition table and the boot partition; can also save a full copy of Windows Phone's
         Data partition (optional; about 5 GB)
       - writes Android into the space of Windows Phone's Data partition, checks every byte, then writes the
         new partition table. Windows Phone no longer starts afterwards; the way back is the stock FFU.
       - tells you what to expect on the first start
  2  Try Android from a microSD card  (optional: Windows Phone stays on the phone)
       - prepares the phone as above, then writes the card from files\superman-alpha-sdcard.img.gz and reads
         it back to check every byte
  3  Go back to Windows Phone
       A  fresh Windows Phone: step-by-step guide for WPinternals "Flash original FFU" (tested, always works)
       B  [EXPERIMENTAL] Windows Phone as you left it, from a backup of this tool: writes the saved Data partition and the
          old partition table back (phone in Mass Storage Mode) and makes Windows Phone the default again.
          Needs the optional full copy of the Data partition made during 1.
          EXPERIMENTAL: passed its tests on the PC, not yet tried on a real phone. Keep the stock FFU.
       C  Android only on a microSD card: how to start Windows Phone again
  4  What do I need, and where do I get it?  The list above, and it can open lumiadb.com for you.
  5  Is this PC ready?  Looks for the Lumia USB drivers, WPinternals, a stock FFU, internet and free space.
     Cautions only: it never stops you. It also runs at the start of 1 and 2.
  6  Check this kit's files
  7  For tinkerers: move an Android that already runs from the SD card onto the phone, over adb
     (not needed after 1; downloads Google's adb if it is missing)

Safety
  - Nothing is written before a clear question. Installing on the phone needs the word INSTALL, erasing an
    SD card needs the word ERASE, restoring a backup needs the word RESTORE.
  - The PC's own disks and the phone are never offered as an SD card.
  - Every file of the kit is checked against its SHA-256 before it is used.
  - Each run writes a log to logs\. If something goes wrong, keep that file.

If Windows shows "You need to format the disk" at any time: click Cancel. Never format.

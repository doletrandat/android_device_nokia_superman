# First-boot per-phone setup (CC52-CC56)

- `superman-firmware.sh`, `00-superman-firmware.rc`, `firmware-allowlist.txt` (CC53/CC54b): builds this phone's
  firmware store from Windows Phone MainOS (read-only, ntfs3) and DPP, TrustZone-decrypts MBA/MPSS
  (`../modem/pf29-mba.c`, `../modem/pf29-mpss.c`), binds it over /vendor/firmware/qcom/superman, and loads the
  Wi-Fi/BT addresses from DPP into the live device tree (`superman-dpp-addr/`).
- `ntfs3/Makefile`: builds the kernel's fs/ntfs3 as an out-of-tree module (copy the Makefile into a copy of fs/ntfs3).
- `usb-disk/` (CC56): f_mass_storage as a module (Makefile for drivers/usb/gadget/function sources) and
  `superman-usb-disk.sh`, which shows the internal eMMC to a PC as a read-only USB disk.
- Modem seeds (CC52): `../provision/`.

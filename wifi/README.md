Superman WCN3620 associated scan guard

Both hardware-offloaded and software scans while associated stalled this board with its stock firmware. On Nokia Superman / MSM8226 / WCN3620 only, associated scan requests return EBUSY before entering firmware scan or software fallback. Disconnected scanning and ordinary association remain unchanged. Power saving and the existing PMC exit-error fix are preserved.

This is a conservative workaround. Connected roaming scans are unavailable; disconnect before scanning another AP. No successful scan completion is invented.

The complete source snapshot is in wcn36xx. Build externally against the exact qualified kernel output using the PF30 wifi-freeze/connected-scan-busy/BUILD.json command and environment; the kernel itself remains unchanged. prebuilt/wifi-pf30/ABI.json verifies all 114 imports through equality with the directly provider-verified candidate. SOURCE.json identifies inputs and bounded runtime qualification. The optional complete-phone product profile includes superman-wifi.mk.

Original failing scan candidates and private network captures remain in the PF30 artifact directory. Long-term reliability remains unqualified.

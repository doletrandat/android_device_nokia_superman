# PF31 audio source and qualified transport

Real mono48kS16LE ALSA playback, Q6ASM/ADM/AFE and a stereo-to-mono Android HAL.
Hardware streams, standby/reopen and startup passed. The GPIO13 correction
produced physically confirmed speaker sound. Normal microphone recording, repeated duplex reopening and cold-boot cleanup
are qualified; carrier voice-call audio remains unfinished.

The Nokia AUDD PEP requires GPIO1 source4/VIN3, DIVCLK1 factor2 and RPM L25
1.775V. ../board-power.dtsi adds four properties to the qualified boot DT.
The codec module owns only its regulator vote and named GPIO states, preserves
shared S4/divider, and restores original register state. Native L25 direct
writes were rejected by hardware and are never used as a fallback.

External builds use existing qualified vmlinux/config, -j20, actual ELF CRCs
and installed PDR provider identities. Kernel/zImage/ramdisk are unchanged.
The PCM callback lifetime is drained before freeing stream state. Uncertain
DMA/power teardown retains ownership: never force-unload.

Startup is one bounded attempt per boot. CORE starts post-fs-data; playback
validates boot/provider hashes before loading its owned modules. The normal
Android audio service starts on readiness or bounded failure. The microphone route is advertised through the normal Android HAL. Full
cold-boot qualification is tracked in both ledgers.

The selected SD system is updated using the hash/readback/rollback installer
in .codex-restart/artifacts/pf31-phone-audio-power-20261031. product.mk is an
addon for the PF24 transport product, not a replacement complete phone image.

TX1 interface FIFO configuration belongs to the common codec lease. It is
enabled before either AFE starts, remains configured while either direction
is open, and restores its exact snapshot after the last direction closes.
Capture still releases its SLIM source, microphone bias and DMIC/DEC clocks on
standby. This corrects intermittent silent capture when reopening during
playback; 50 hardware reopenings passed. Cold09 through cold12 also passed
normal Android capture, playback reopening and standby cleanup.

Cold09 boot9285180f-965e-4237-a793-76fd102acfd4 qualified automatic startup,
33s normal playback with Internet,15 normal Android capture reopenings and
exact final codec/TX FIFO/power release. Selected PCM e33875e5430c22e91e4883fc70e0eab40b005bae77d522fca360101b861c5095.

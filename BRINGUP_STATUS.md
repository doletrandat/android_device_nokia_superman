# Lumia 730 bring-up checkpoint — 2026-11-03

This branch records the current source work. The selected SD installation is
qualified separately by exact binary hashes, boot IDs and file readbacks. A
checkout is not a complete flashable phone image. Generated modules, shared
libraries, downloaded source archives, stock firmware and device data are local
inputs and are not redistributed here.

Working phone foundations include responsive display/touch, Wi-Fi with connected
scanning deliberately refused to avoid observed freezes, automatic modem/RMTFS
startup with RAM-only writes, SIM2 discovery, physically confirmed speaker output,
normal Android microphone recording and duplex reopen/standby cleanup. One-minute
screen timeout and measured signed battery current through Health are applied.
Deep suspend remains guarded. Charging and actual battery SOC remain unresolved.

SMS request/indication handling is installed and passed a cold-boot framework
check. The stock routing table is preserved; an earlier status-report route
mutation was rejected. Real carrier SMS, calls and voice-call audio are unqualified.
The inserted SIM has not registered, and its active-service status is unknown.

The single-context IPv4 mobile-data backend is implemented and installed with
rollback and exact file readbacks. Host sanitizer tests, native ARM tests,
physical asynchronous WDS status/binding/client release and a 75-second modem/SMS
soak passed. A fresh manually selected Android boot started it automatically.
Framework data-list verification is still in progress: the first standalone
helper could not bind because it lacked an Android app-process registration.
No successful carrier data session is claimed.

Graphics work retained here includes historical candidates and diagnostics.
P149 remains the accepted Mesa baseline. P150/P151 and Mesa 26 are rejected or
inactive; narrow pixel correctness is not a gameplay-performance qualification.
The shared sources for other AOSP projects are outside this repository.

Local PROGRESS_LEDGER.md and CRASH_BUILD_LEDGER.md retain detailed experiment,
rollback, failure and deployment evidence. Public commits preserve source work
without inventing historical commit dates or turning pending checks into success.

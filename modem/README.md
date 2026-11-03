# Superman modem bring-up

PF29/PF30, active work on the Lumia 730/MSM8226. Original firmware authentication,
RAM-only remote filesystem writes and QMUX control have physical evidence. The
selected SD profile now includes a real Android Radio HAL: the framework loads
SIM2 and reads its identity/files. Basic call-control commands are implemented;
actual calls, carrier SMS/mobile data, charging/SOC and safe suspend remain
unqualified. Speaker and microphone were separately integrated and qualified in PF31.
This is not yet a complete phone.

## Selected firmware and memory

Apply `superman-stock-mba-ssd.patch`, then `superman-stock-mpss-ssd.patch` to the
qualified SCM source. The patches reproduce the built source and use normal
serialized Qualcomm SSD services: metadata 7/6, MBA 7/7, ELF segments 7/8. Exact
board and full input digests are required; pointers, fragments, resources, cleanup
and signed output hashes are checked. No keys, keystore, fuses or authentication
bypass are used. MBA boots through PBL. All 19 nonempty MPSS code segments match
the original signed hashes after decryption; every other byte is unchanged.

The selected kernel/module ABI is unchanged by the latest DT work. The unmodified
external MSS driver has `auto_boot=false`. The DT equivalent is
`superman-modem-manual.dtsi`: MPSS/MBA reservations and the stock RFS0 data window
at **0x0fd80000 + 0x180000**. This window comes from the phone's own original UEFI
ACPI QCOM2423 resources, matched against the current read-only UEFI hash. The
quarantine is split without releasing its other bytes. Original RFS0 contents
were preserved before reassignment and before the first service write.

The earlier 1 MiB high-DDR trial was undersized; the 2 MiB high-DDR trial allocated
successfully but caused a modem exception on its first read. Both are superseded,
with failed captures retained. The stock mapping passes allocation, large reads
and RAM-shadow writes, the bounded 150-second modem test and subsequent control
and SIM tests. Initial modem start without RMTFS had stalled after about 40 seconds.

## Filesystem and startup

The reused Linux MSM RMTFS service has restricted adapters:

- Four fixed regular SD files; unexpected paths, sizes, symlinks and raw devices
  are rejected. Seed files are opened read-only. All modem writes remain in RAM.
- Only the standard `/dev/qcom_rmtfs_mem1` at the qualified stock address/size.
  There is no `/dev/mem`, UIO or arbitrary-address fallback.

The four original MODEM_FSG/FS1/FS2/FSC partitions were acquired read-only with
size/hash/readback checks. Private immutable host/SD baselines are retained; no
original NV/eMMC write occurred. Firmware, NV and RAM bytes must never be included
in this source directory or public commits. RAM-only mode is diagnostic and does
not provide persistent modem updates across restarts.

`superman-modem-start.sh` and `superman-modem.rc` are installed on the selected SD
system. The helper checks boot/FDT/config/kernel notes, component hashes, exact SD
CID/root/data devices, stock RFS0 and the private baseline manifest. It starts only
after Android boot and audio CORE readiness. A per-boot lock permits one attempt.
It starts RMTFS, manually starts MSS, checks QMUX and monitors both processes.
Operational auto recovery is disabled. The modem supervisor does not activate RF or a SIM session; the separate Radio HAL controls normal radio power and SIM provisioning.
`transport_ready` means working modem control transport, not cellular service.

The helper's `--check` is read-only. An incorrect CID is rejected without state
changes. The manual supervisor cycle reaches transport readiness; intentionally
terminating its owned RMTFS daemon causes orderly modem stop/unload, an expected
`failed` property, and rejection of a same-boot retry. Android/audio and all seed
hashes survive. The corrected supervisor passes automatic startup and 25 steady samples (about
120 seconds) on each of two fresh Android boots. Results are under
`modem-startup-fix/automatic-boot/` and `automatic-boot-02/`. The first automatic
trial exposed a /vendor -> /system/vendor path-alias bug: it stopped a healthy
daemon and waited in cleanup. Both comparisons now use the canonical binary path;
unexpected executable identity cannot cause an indefinite wait. Its failed
capture is retained. The corrected init service also passed eight healthy
monitor periods before the deliberate daemon-exit cleanup test.

Generated modem hashes/private seed contract and the independently acquired boot
contract are supplied by the qualified SD deployment. Merely selecting the Soong
targets does not qualify a generic image. Full generic Soong/image builds and
SELinux enforcing remain unqualified.

## Control transport and SIM evidence

`superman-qrtr-probe` performs bounded QRTR discovery and read-only DMS/UIM queries
when advertised. This firmware exposes its main cellular services through legacy
SMD **DATA5_CNTL QMUX**, so their absence from QRTR was not evidence of their absence
from the modem. `superman-qmux-probe` uses standard RPMSG control/character APIs,
selects only the main modem, owns/releases its endpoint and allocated QMI clients,
and validates framing, transactions and TLV/card bounds. The default queries only
CTL versions, DMS revision/mode, NAS status for both client subscriptions and UIM
card status. It requests no identity, PIN, SIM file write, call or SMS operation.

Explicit root diagnostic flags have additional effects:

- `--at-check` and `--at-check-data4`: fixed AT status queries.
- `--radio-power-test`: temporary CFUN enable, bounded AT compatibility queries,
  then restoration of the acquired CMEE and original CFUN=0.
- `--radio-status-test` / `--radio-registration-test`: bounded 20/90-second RF-on
  tests followed by CFUN=0 restoration. These are never called by startup.
- `--activate-sim2`: explicitly activate the inactive secondary GSM provisioning
  session using the physically present slot-2 application's bounded AID.
- `--deactivate-sim2`: restricted normal session-deactivation request. Both tested
  encodings returned QMI error 3 on this firmware. Do not claim it is a successful
  cleanup path; orderly MSS shutdown/reload clears the owned session instead.

The user inserted an unqualified SIM in slot 2. It is detected and accepts secondary
GSM session activation (index 0100). The application reaches state 4, PIN1 disabled,
personalization state READY. Neither subscription registered during the bounded
RF-on test; carrier availability/SIM service and remaining provisioning are
unresolved. No unlock code was guessed or submitted. RF was restored off and the
modem stopped normally afterward; a cold MSS start shows both session indexes ffff.

DATA1/DATA4 answer AT and CFUN. CPIN/CREG still return ERROR and CGMR timed out,
including with extended errors enabled. The Android reference AT RIL is therefore
not qualified or applied. Current AOSPM qrild targets AIDL/QRTR and its old branch
is documented as unfinished; it is not a drop-in Android 11/QMUX backend.

## Android Radio HAL (PF30)

`superman-radio.mk` selects the qualified cellular profile. `libsuperman-ril`
uses the shared QMUX codec and serialized transport; unmodified AOSP `rild` and
`libril` expose HIDL Radio 1.1 through the device VINTF fragment. The service
registers in class `hal` before PhoneFactory starts, reporting radio unavailable
until the guarded modem supervisor reaches `transport_ready`. It does not retry
a failed modem automatically. Physical **SIM2** is Android logical **slot1** in
this single active SIM profile. Physical slot index 1 is distinct from logical subscription index 0: NAS and VOICE bind to primary, and primary GW session 0 provisions the physical SIM2 application. SIM-status decoding and read-only SIM I/O use that logical session independently of the physical slot. This is not a dual-SIM Android implementation.

Native callbacks and the real Android framework read card/application status,
IMSI/ICCID, read-only SIM files, baseband/device identity, signal and registration,
radio capability, preferred mode, network-selection mode and actual current calls.
The SIM2 READY mapping is constrained to the active provisioning index, safe PIN
state and personalization READY, and was independently verified by successful
SIM-file reads. Dial, answer and hang-up builders are implemented and malformed
inputs are tested without sending a call. Voice audio and actual call control
remain unverified. PIN submission and SIM writes are unsupported.

Cold-modem testing exposed valid 12-byte CTL SYNC indications during 13-byte DMS
response waits. The shared parser now classifies the actual service header before
checking its length, so these indications are skipped without falsely reporting
the radio unavailable. The exact acquired packet is a sanitizer regression fixture.
The earlier automatic radio-drop captures are retained. Network preference changes request power-cycle duration, not permanent NV storage. The earlier secondary-routing profile reported GSM only. The primary-routing native test reports GSM/WCDMA auto mode and reads physical SIM2 through primary session 0; automatic init qualification is recorded in the ledgers. Carrier registration remains unproved with the unqualified SIM.

Startup verifies priority/data/voice assignment with the standby mode preserved. If a change is needed, it uses bounded normal RF-off, the three supported assignment TLVs, actual readback and restoration of the original effective RF mode; errors attempt rollback and exit the one-shot service cleanly. Fresh firmware can report DMS SHUTDOWN mode 5: if initialization is needed, use normal low-power mode 1, never issue a shutdown request. An already selected subscription leaves RF off until Android requests power on.

The unmodified BAM-DMUX driver is installed and creates eight raw-IP channels at modem startup. This is a kernel transport foundation; Android data calls and carrier IP connectivity remain incomplete.

Do not run a QMUX diagnostic against DATA5_CNTL while the Radio HAL owns it.
For owned service tests, use verified PID/executable identity and normal SIGTERM;
Android 11 `ctl.stop` uses SIGKILL and bypasses endpoint cleanup. A killed process
can leave RPMSG metadata until exact-owned cleanup or modem teardown. Same-boot
cold-modem diagnostics are explicit bounded tests that preserve the original
per-boot ownership record, not an operational recovery loop.

PF30 evidence is in `.codex-restart/artifacts/pf30-android-radio-20261031/`.
Run `ril/tests/run.sh` for the actual codec/model/SIM-read/call-builder sanitizer
tests. Native and framework tests separately establish real hardware behavior.

## Validation and recovery

MBA helper: 28 ASan/UBSan host cases. MPSS helper: 33 cases; TZ/output hashes are
mocked there, separately from the physical 19-signed-hash result. RMTFS actual
adapters and actual protocol handlers/codec pass sanitizer and native ARM32 tests.
The pre-fix valid-handle device-error query fails the regression test. QMUX embedded
`--self-test` covers framing, transaction/TLV/card bounds and restricted builders.
Run `rmtfs/tests/run.sh` with a host compiler for the actual-handler tests.

All 36 selected module files / 2924 imports match the actual linked kernel; the
incompatible preserved legacy MSM module is excluded. Temporary SSD/SMEM/RFS
snapshot modules are separate diagnostics. No graphics algorithms, tuning, Mesa
or CPU governor policy was changed.

Evidence is in `.codex-restart/artifacts/pf29-complete-phone-20261030/`. Preserve
failed/incomplete captures and distinguish source/build/install from device results.
Stop the supervised modem before rollback. Roll back qualification metadata and startup path fix first, then startup, protocol-02,
protocol, stockmap, 2m, initial RMTFS, plaintext, segment helper, manual modem DT,
first SSD helper and finally PF28. This kernel refuses ordinary stop from CRASHED:
remove RMTFS, recover once solely to allow orderly stop, then unload normally.
Never force-unload a crashed modem or create operational recovery loops.

## PF33 Android SMS interfaces

The selected Radio backend implements GSM/WCDMA SEND_SMS/EXPECT_MORE, SMSC query,
SMS acknowledgements and memory-availability reporting. It demultiplexes owned
WMS indications while requests are waiting and drains at most16 available frames
once per second between normal status polls. An eight-event queue has one Android
acknowledgement in flight; duplicate queued events are suppressed. Invalid frames,
oversized PDUs and queue overflow never receive a fabricated success ACK.

Startup binds the primary logical subscription after its SIM application is ready,
checks the existing host-transfer routes, and enables only its client notification
and primary-client settings. It leaves all routes and SIM storage behavior intact.
The initial experiment could enable delivery-report transfer, but clearing it did
not read back; that mutation was rejected and removed. A fresh modem boot is
required to clear its temporary RAM flag before cold qualification. Status-report
handling is implemented when supplied by the firmware, but enabling delivery
receipts and actual carrier send/receive remain unqualified. SMSC writes, SIM SMS
writes/deletes, IMS and CDMA SMS are unsupported.

Actual helper/Radio-callback/socket-transport tests pass ASan/UBSan, including65536
malformed bodies, reentrant ACK, queue overflow and startup-time indication cases.
The ARM equivalents and real modem client startup plus75s Radio status soak pass.
The installed backend passes a public SmsManager query through the normal framework
and Radio HAL. The current modem reports SMS service flags0 and QMI52/device-not-ready;
carrier service is not established. No actual SMS/call was sent or synthetic SMS
injected into Android. Source/build/readback/rollback records are in PF33 artifacts;
automatic cold startup passed on boot 3fe20227-dfdc-43b6-8dbe-8e0c9a6caa37, including original report flag0 after reset, public SmsManager roundtrip, measured current, normal duplex audio/standby and Wi-Fi. Carrier SMS remains unqualified.

## PF34 mobile data

The Radio HAL implements SETUP_DATA_CALL, DEACTIVATE_DATA_CALL and DATA_CALL_LIST
for one owned IPv4 context on the physical BAM-DMUX channel 0 (wwan0). IPv4v6
requests may negotiate IPv4; IPv6-only, concurrent APNs and IMS are unsupported.
No persistent modem profiles are written. Android owns routing and DNS from the
actual modem settings returned by the HAL; missing addresses never become a
fabricated connected result.

StartNetwork is asynchronous, with a 30-second deadline and cancellation. SMS
indications and acknowledgments continue while it is pending. Exact owned handles,
client IDs and interface configuration are cleaned on stop, cancellation, airplane
mode and shutdown. Failed cleanup retains ownership and blocks a conflicting new
session until cleanup succeeds. Foreign interface configuration is never erased.

Host sanitizer and native ARM tests cover setup, malformed replies, cancellation
including during settings retrieval, airplane mode, SMS interleaving and partial
configuration cleanup. Physical WDS binding, asynchronous status and client release,
and a 75-second modem/SMS soak passed. Installation and framework qualification are
recorded in PF34 STATUS and the handoff ledgers. Carrier traffic remains unqualified:
the inserted SIM has not registered. No external call or message was sent by tests.

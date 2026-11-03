# Inactive P33 buffer-addressing candidate

These patches are review artifacts, not product-selected source changes. Neither
patch has been applied to the active kernel or gralloc. No native display startup
property is selected, and P32's MDSS parent remains disabled.

`buffer-addressing.kernel.patch` applies to the selected cached P26 kernel source
at `/home/doletrandat/.cache/superman-r37-smp-source-20260924`, not the older
`aosp11/kernel/lumia-superman` checkout. It separates legacy platform eligibility
used to reserve VRAM from initialized GPU MMU eligibility used by regular GEM and
PRIME imports. It retains contiguous backing for scanout/stolen BOs when the
MSM8226 platform has no display IOMMU, including failure when no pool exists.
The historical A2xx exception is retained; other hardware has not been tested.

`buffer-addressing.gralloc.patch` applies from the AOSP root. Only an explicit
future `ro.superman.native_scanout=true` adds SCANOUT to HW_FB allocations.
Such allocations must be 720x1280 BGRA with a valid 32-bit pixel stride and
linear modifier. Other usages and the unset/default property retain the current
P30 routing. A failed native allocation has no software or tiled fallback.

ARM32 object compilation and actual-function sanitized host tests pass. Neither
patch has a linked-module/HAL ABI check or hardware acceptance. These patches do
not preserve inherited MDP/DSI/PLL state, register a display owner, perform KMS
commits, provide vblank or retire scanout buffers.

The existing nominal 16 MiB allocator manages 4095 pages (one page excluded).
Four 4 MiB BOs exceed that space. Actual BO sizes, existing users such as fbdev,
largest contiguous availability and simultaneous targets must be measured at
startup. Do not borrow the protected simplefb reservation or substitute GPU
IOVAs for physical display addresses.

Evidence and runtime findings:
`aosp11/.codex-restart/artifacts/r37-p33-scanout-buffer-contract-20261007/REPORT.md`.
P30 has a newly captured launcher ANR and subsequent GPU lockups; their cause
remains unresolved and must be reviewed before a native activation experiment.

## P34 continuation

The P33 contracts are integrated in an isolated, compiled native-display source candidate. See [P34 source and limits](p34/README.md). Active kernel/product sources remain unchanged; nothing is deployed or hardware-validated by P34.

P35 adds unselected headless probe/flip startup profiles and boot-hash sealing: see `p35/README.md`. Prepared overlays remain unsealed; no boot/image/deployment or UI release.

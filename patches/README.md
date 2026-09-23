# Reproducible r37 source changes

The device repository has Git metadata; the surrounding AOSP projects and
kernel source currently do not. These patches preserve external source edits
without inventing an upstream checkout history. They are already applied in
this workspace. Do not apply them twice.

- `r37-userspace-compat.patch`: Settings missing-USB-service handling and
  regression test cases, SoundPicker permission request, and manifest build
  dependencies. Paths are relative to the AOSP root.
- `r37-smp-diagnostics.patch`: existing SCM/release/secondary-online markers.
  Paths are relative to `kernel/lumia-superman`. This records diagnostics
  already in source, not a new functional CPU startup fix or a shipped kernel.

Both patches passed `patch --dry-run -R -p1` from their respective roots on
2026-09-23. On a matching pre-change source tree, use a forward dry run before
applying. Preserve unrelated local changes.

Diagnostics were recovered from Git tree
`f396f8c3048463e5e5f42de322798a6a1a8ddc8c` and committed as `fe17103`.
The diagnostic directory's disappearance does not mean its source was lost:
the unreachable tree retained it. The exact deletion mechanism is unproven.
Recovered packaging scripts are tracked alongside the source.

The product kernel and r37 touch module hashes remain unchanged. CPU1-3
startup remains unresolved. Validate an uninstrumented replacement-kernel
control on hardware before promoting marker builds; previous replacements
failed early. No automatic CPU reset/online retries are added.

See `handoff/evidence/r37-compat-applied-20260923/applied.md` at the AOSP root
for checks and build limitations. Full system/SD packaging and device tests
are separate from these source/component validations.

## CPU control and clock candidate

`scripts/build-cpu-control.sh` builds an isolated kernel-only control from the
preserved uninstrumented snapshot and captured r37 config. The verifier checks
all preserved module import CRCs and creates a separate boot-only artifact.
`scripts/compare-cpu-control.py` classifies every difference in its decompressed
Image versus the hash-pinned working Image. Product prebuilts are untouched.

`r37-cpu-clock.config` is a staged follow-up, not selected by the control or
product: it enables the missing APCS clock provider. Its olddefconfig result
has no other functional changes. Test the rebuilt control on hardware before
building/promoting the clock or SMP-marker candidates. Full details are in
`handoff/evidence/r37-cpu-control-20260923/README.md` at the AOSP root.

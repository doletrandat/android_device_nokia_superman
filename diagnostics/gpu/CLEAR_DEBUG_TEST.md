# Private Freedreno clear debugging

Explicit userdata diagnostic; not selected in PRODUCT_PACKAGES. `--run default`,
`--run gmem` and `--run nogmem` set process-local `FD_MESA_DEBUG` to `bstat`,
`bstat,nobypass` and `bstat,nogmem` before Mesa loads. Statistics verify paths.
Two720x48private FBOs/unsharedcontexts,24alternating iterations, exactRGBA checks.
Readbacks serialize work; passing does not validate UI, asynchronous reuse or
physical presentation.20s watchdog cannot guarantee interruption of kernel stalls.
Run only with live provenance, memory and recent-fault guards; stop on any failure.
Hardware evidence: /home/doletrandat/Superman/history/desktop/p69-freedreno-clear-debug-20261013/REPORT.md.

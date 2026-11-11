# A305B command-processor phase diagnostic

P74 adds `FD_MESA_DEBUG=progress` to this retained Mesa20.3.4 source. The
ordinary driver default remains off. Its diagnostic image opts in through
`ro.superman.gpu_progress=1`, only with `ro.product.device=superman`, GPU ID305
and chip ID0x03000512. A release log confirms that image opt-in at screen
creation. This is an execution diagnostic, not a GPU hang fix.

An enabled A305 batch allocates one private writable4096-byte WC BO, clears
it, writes a versioned CPU header and marks it for the existing crash dump.
CP_MEM_WRITE records bracket setup, each tile's prep/restore/renderprep/draw/
resolve, and finalization. Sysmem and nondraw batches use four records.
There are at most504 token/complement slots. The header identifies capped
traces; allocation/map failure leaves the original untraced submission path.
The ordinary relocation/submit reference retains the BO after the local
reference is released. No context/screen ABI field is added.

These markers mean the CP reached corresponding commands. They do not prove
shader/resolve completion, an exact stalled instruction, or atomic sampling.
The GPU can queue memory writes, and the existing fault dump is not atomic
with execution. The offline decoder reports partial/inconsistent pairs or
noncontiguous records and never promotes them to a completed trace. Records
add GPU commands, memory writes and one BO per enabled batch; they can change
timing or the command-buffer layout and may mask or expose a failure. No
additional wait, cache flush, display import/post or pixel readback is added.

The kernel's existing hang/fault dump retains this marked BO together with
its owning submit. No capture button is required. A visual glitch without a
kernel fault may not retain any trace. Do not trigger a fault to obtain one.
The existing limited dump lifetime still applies; preserve the original dump
before expiry without clearing it. Decode its BOs with the retained kernel
ASCII85 decoder, then run `decode-progress.py` on that inventory. Do not replay
the captured command stream.

Source, baseline/candidate ARM32 builds, bounds/lifetime/product-gate tests,
decoder and private-phone validation are retained in
`/home/doletrandat/Superman/history/desktop/p73-gpu-submission-audit-20261015`.
P74 packaging is retained in
`/home/doletrandat/Superman/history/desktop/p74-gpu-progress-20261015`.

# Lumia 730 zram swap

The phone has 878 MB usable RAM and ran with no swap: the r37 6.16 kernel was
built with CONFIG_ZRAM and CONFIG_ZSMALLOC unset. Under app switching kswapd and
kcompactd took ~12% CPU and lmkd killed cached apps on PSI ("device is not
responding"), which slowed app starts and stole time from UI frames.

`src/` builds mm/zsmalloc.c and drivers/block/zram (lzo, lzo-rle, zstd
backends; all three compressors are already in the kernel) as external modules
against the running kernel, with the external-module recipe used by
power/charger. The only source change drops the NR_ZSPAGES vmstat counter,
which exists only when zsmalloc is configured into the kernel
(`src/zsmalloc-no-vmstat.diff`). zsmalloc pages use the generic
__SetPageMovable interface, so compaction migrates them as usual.

The rc creates a 450 MiB LZO-RLE zram swap at post-fs-data, sets swappiness
100 and page-cluster 0 (no swap read-ahead, as usual for zram).

CC9 A/B on the normal boot (same app sequence, warm): Calendar/Clock/Gallery/
Files reopen 595/866/1048/1225 ms without swap vs 298/423/723/599 ms with
zram; Settings cold start 5.1/2.5/2.7 s vs 2.5/2.3/2.3 s; kswapd pages scanned
36k vs 19k; compression 2.9-3.7:1.
Evidence: handoff/evidence/cc9-zram-20261104.

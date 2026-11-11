# Out-of-tree zsmalloc + zram (lzo, lzo-rle, zstd) for the r37 6.16 kernel, which has CONFIG_ZRAM unset.
# Build dir = this Makefile + mm/zsmalloc.c (with zsmalloc-no-vmstat.diff), mm/zpdesc.h
# and drivers/block/zram/{zcomp,zram_drv,backend_*}.{c,h} copied from the kernel source.
ccflags-y += -DCONFIG_ZSMALLOC_CHAIN_SIZE=8 -DCONFIG_ZRAM_BACKEND_LZO=1 \
	-DCONFIG_ZRAM_BACKEND_ZSTD=1 -DCONFIG_ZRAM_DEF_COMP=\"lzo-rle\"
obj-m += zsmalloc.o zram.o
zram-y := zcomp.o zram_drv.o backend_lzorle.o backend_lzo.o backend_zstd.o

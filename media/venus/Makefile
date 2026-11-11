# Out-of-tree Venus (with an MSM8226 entry) plus the V4L2 helpers the r37 kernel
# does not build (VIDEOBUF2_DMA_CONTIG, V4L2_MEM2MEM_DEV).
obj-m += videobuf2-dma-contig.o v4l2-mem2mem.o venus-core.o venus-dec.o venus-enc.o
venus-core-objs := core.o helpers.o firmware.o hfi_venus.o hfi_msgs.o hfi_cmds.o hfi.o \
	hfi_parser.o pm_helpers.o dbgfs.o hfi_platform.o hfi_platform_v4.o \
	hfi_platform_v6.o hfi_plat_bufs_v6.o
venus-dec-objs := vdec.o vdec_ctrls.o
venus-enc-objs := venc.o venc_ctrls.o

// SPDX-License-Identifier: GPL-2.0-only
/* DSP-owned memory is released only after a confirmed unmap response. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/apr.h>
#include <linux/of.h>
#include <linux/dma-mapping.h>
#include <sound/pcm.h>
#include "q6asm.h"
extern const struct bus_type aprbus;
static struct platform_device *pcm;
static struct audio_client *client;
static void *backing;
static dma_addr_t address;
static u64 dma_mask=DMA_BIT_MASK(32);
static bool passed,backing_retained;
module_param(passed,bool,0444);
module_param(backing_retained,bool,0444);
static int __init memory_start(void)
{
 struct device *asmdev;int ret,closed;
 if(!of_machine_is_compatible("nokia,superman")||!of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 asmdev=bus_find_device_by_name(&aprbus,NULL,"aprsvc::4:7");if(!asmdev)return -ENODEV;
 if(!asmdev->driver||strcmp(asmdev->driver->name,"qcom-q6asm")){put_device(asmdev);return -ENODEV;}
 pcm=platform_device_alloc("superman-q6mem-test",PLATFORM_DEVID_NONE);if(!pcm){put_device(asmdev);return -ENOMEM;}
 pcm->dev.parent=asmdev;pcm->dev.dma_mask=&dma_mask;pcm->dev.coherent_dma_mask=dma_mask;
 ret=platform_device_add(pcm);put_device(asmdev);if(ret){platform_device_put(pcm);return ret;}
 backing=dma_alloc_coherent(&pcm->dev,8192,&address,GFP_KERNEL);
 if(!backing){ret=-ENOMEM;goto device;}
 client=q6asm_audio_client_alloc(&pcm->dev,NULL,NULL,7,LEGACY_PCM_MODE);
 if(IS_ERR(client)){ret=PTR_ERR(client);client=NULL;goto memory;}
 ret=q6asm_open_write(client,1,FORMAT_LINEAR_PCM,0,16,false);
 if(ret)goto close;
 ret=q6asm_map_memory_regions(SNDRV_PCM_STREAM_PLAYBACK,client,address,4096,2);
 pr_info("SUPERMAN_Q6MEM map address=%pad bytes=8192 ret=%d\n",&address,ret);
 if(ret)goto retain;
 ret=q6asm_unmap_memory_regions(SNDRV_PCM_STREAM_PLAYBACK,client);
 pr_info("SUPERMAN_Q6MEM unmap ret=%d\n",ret);
 if(ret)goto retain;
close:
 closed=q6asm_cmd(client,1,CMD_CLOSE);
 pr_info("SUPERMAN_Q6MEM close ret=%d\n",closed);
 if(!ret)ret=closed;
 q6asm_audio_client_free(client);client=NULL;
memory:
 dma_free_coherent(&pcm->dev,8192,backing,address);backing=NULL;
 if(!ret){passed=true;pr_info("SUPERMAN_Q6MEM map/unmap/backing-release PASS\n");return 0;}
device:
 platform_device_unregister(pcm);pcm=NULL;return ret;
retain:
 /* A failed RPC may have executed remotely. Keep its backing and providers. */
 backing_retained=true;__module_get(THIS_MODULE);
 pr_err("SUPERMAN_Q6MEM uncertain DSP ownership ret=%d; backing retained, module pinned\n",ret);
 return 0;
}
static void __exit memory_stop(void) {if(pcm)platform_device_unregister(pcm);}
module_init(memory_start);module_exit(memory_stop);
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("Superman real Q6ASM owned shared-memory map/unmap check");

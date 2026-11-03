// SPDX-License-Identifier: GPL-2.0-only
/* One-off recovery for the exact owned test module on boot ff59c00f. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>
#include <sound/pcm.h>
#include "q6asm.h"
/* Exact ELF .bss layout and live sysfs section addresses captured before load. */
struct owned_state {struct platform_device *pcm;dma_addr_t address;void *backing;struct audio_client *client;bool passed,backing_retained;};
static int __init recover(void)
{
 struct module *owner=(void *)0xbf41c040UL;
 struct owned_state *s=(void *)0xbf41c000UL;
 struct audio_client *borrowed;int ret;
 BUILD_BUG_ON(offsetof(struct owned_state,backing_retained)!=17);
 if(!of_machine_is_compatible("nokia,superman")||!of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 if(strcmp(module_name(owner),"q6mem_smoke")||owner->state!=MODULE_STATE_LIVE||
    !within_module((unsigned long)s,owner)||!within_module((unsigned long)s+17,owner)||
    !s->backing_retained||s->passed||!s->pcm||!s->client||!s->backing||s->address!=0x3c042000)
  return -EINVAL;
 if(strcmp(dev_name(&s->pcm->dev),"superman-q6mem-test"))return -EINVAL;
 if(!try_module_get(owner))return -ENODEV;
 borrowed=q6asm_audio_client_alloc(&s->pcm->dev,NULL,NULL,7,LEGACY_PCM_MODE);
 if(IS_ERR(borrowed)){ret=PTR_ERR(borrowed);goto put;}
 if(borrowed!=s->client){q6asm_audio_client_free(borrowed);ret=-EINVAL;goto put;}
 ret=q6asm_unmap_memory_regions(SNDRV_PCM_STREAM_PLAYBACK,borrowed);
 pr_info("SUPERMAN_Q6MEM_RECOVER confirmed_unmap ret=%d\n",ret);
 if(ret<0){q6asm_audio_client_free(borrowed);goto put;}
 ret=q6asm_cmd(borrowed,1,CMD_CLOSE);
 pr_info("SUPERMAN_Q6MEM_RECOVER close ret=%d\n",ret);
 if(ret){q6asm_audio_client_free(borrowed);goto put;}
 q6asm_audio_client_free(borrowed);
 q6asm_audio_client_free(s->client);s->client=NULL;
 dma_free_coherent(&s->pcm->dev,8192,s->backing,s->address);s->backing=NULL;
 s->passed=true;s->backing_retained=false;
 pr_info("SUPERMAN_Q6MEM_RECOVER backing released after unmap; dropping owned pin\n");
 module_put(owner); /* Original self-pin, after the confirmed DSP release. */
 ret=0;
put:module_put(owner);return ret;
}
static void __exit done(void) {}
module_init(recover);module_exit(done);
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("Exact PF26 owned-test memory recovery, not a production driver");

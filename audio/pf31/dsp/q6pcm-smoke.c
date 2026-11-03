// SPDX-License-Identifier: GPL-2.0-only
/* Real DSP PCM session lifecycle; deliberately does not route to a transducer. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/apr.h>
#include <linux/of.h>
#include "q6asm.h"
extern const struct bus_type aprbus;
static struct platform_device *pcm;
static int __init pcm_start(void)
{
 struct device *asmdev;struct audio_client *ac;int ret,closed;
 if(!of_machine_is_compatible("nokia,superman")||!of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 asmdev=bus_find_device_by_name(&aprbus,NULL,"aprsvc::4:7");
 if(!asmdev)return -ENODEV;
 if(!asmdev->driver||strcmp(asmdev->driver->name,"qcom-q6asm")){put_device(asmdev);return -ENODEV;}
 pcm=platform_device_alloc("superman-q6pcm-test",PLATFORM_DEVID_NONE);if(!pcm){put_device(asmdev);return -ENOMEM;}
 pcm->dev.parent=asmdev;
 ret=platform_device_add(pcm);put_device(asmdev);
 if(ret){platform_device_put(pcm);return ret;}
 ac=q6asm_audio_client_alloc(&pcm->dev,NULL,NULL,7,LEGACY_PCM_MODE);
 if(IS_ERR(ac)){ret=PTR_ERR(ac);goto device;}
 ret=q6asm_open_write(ac,1,FORMAT_LINEAR_PCM,0,16,false);
 pr_info("SUPERMAN_Q6PCM open_write session=8 ret=%d\n",ret);
 if(!ret){ret=q6asm_media_format_block_multi_ch_pcm(ac,1,48000,2,NULL,16);pr_info("SUPERMAN_Q6PCM format=48000/2/16 ret=%d\n",ret);}
 closed=q6asm_cmd(ac,1,CMD_CLOSE);
 pr_info("SUPERMAN_Q6PCM close session=8 ret=%d\n",closed);
 if(!ret)ret=closed;
 q6asm_audio_client_free(ac);
 if(!ret){pr_info("SUPERMAN_Q6PCM session lifecycle PASS; no physical route\n");return 0;}
device:
 platform_device_unregister(pcm);pcm=NULL;return ret;
}
static void __exit pcm_stop(void) {if(pcm)platform_device_unregister(pcm);}
module_init(pcm_start);module_exit(pcm_stop);
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("Superman real Q6ASM PCM session lifecycle check");

// SPDX-License-Identifier: GPL-2.0-only
/* Actual PCM DMA -> ASM -> ADM -> SLIM AFE; analogue outputs remain disabled. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/apr.h>
#include <linux/of.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <sound/pcm.h>
#include <dt-bindings/sound/qcom,q6afe.h>
#include "q6asm.h"
#include "q6adm.h"
#include "q6afe.h"
#include "superman-codec.h"
#include <linux/slimbus.h>
extern const struct bus_type slimbus_bus;
static struct device *codec_dev;
static bool sink_attempt;
extern const struct bus_type aprbus;
static struct platform_device *asm_child,*adm_child,*afe_child;
static struct audio_client *client;
static struct q6afe_port *afe_port;
static struct q6copp *copp;
static void *backing;
static dma_addr_t address;
static u64 dma_mask=DMA_BIT_MASK(32);
static bool opened,mapped,map_uncertain,port_attempt,passed,retained,pinned;
static unsigned long done_mask;
static atomic_t errors=ATOMIC_INIT(0);
static DECLARE_COMPLETION(writes_done);
static DECLARE_COMPLETION(close_done);
static DEFINE_MUTEX(pipe_lock);
module_param(passed,bool,0444);
module_param(retained,bool,0444);
static void event(u32 opcode,u32 token,void *payload,void *priv)
{
 if(opcode==ASM_CLIENT_EVENT_CMD_CLOSE_DONE){complete(&close_done);return;}
 if(opcode==ASM_CLIENT_EVENT_DATA_WRITE_DONE) {
  u32 *data=payload;unsigned index=token&ASM_WRITE_TOKEN_MASK;
  if(index>1||data[0]!=lower_32_bits(address+index*4096)||data[1]!=upper_32_bits(address+index*4096))atomic_inc(&errors);
  else set_bit(index,&done_mask);
  pr_info("SUPERMAN_Q6SPEAKER write_done index=%u address=%08x:%08x\n",index,data[1],data[0]);
  if(READ_ONCE(done_mask)==3||atomic_read(&errors))complete(&writes_done);
 }
}
static struct platform_device *child(int service,const char *driver,const char *name)
{
 char id[32];struct device *parent;struct platform_device *p;int ret;
 snprintf(id,sizeof(id),"aprsvc::4:%x",service);
 parent=bus_find_device_by_name(&aprbus,NULL,id);if(!parent)return ERR_PTR(-ENODEV);
 if(!parent->driver||strcmp(parent->driver->name,driver)){put_device(parent);return ERR_PTR(-ENODEV);}
 p=platform_device_alloc(name,PLATFORM_DEVID_NONE);if(!p){put_device(parent);return ERR_PTR(-ENOMEM);}
 p->dev.parent=parent;
 if(service==7){p->dev.dma_mask=&dma_mask;p->dev.coherent_dma_mask=dma_mask;}
 ret=platform_device_add(p);put_device(parent);
 if(ret){platform_device_put(p);return ERR_PTR(ret);}
 return p;
}
static int cleanup(void)
{
 int ret;
 if(opened){ret=q6asm_cmd(client,1,CMD_CLOSE);pr_info("SUPERMAN_Q6SPEAKER close=%d\n",ret);if(ret)return ret;if(!wait_for_completion_timeout(&close_done,msecs_to_jiffies(100)))return -ETIMEDOUT;opened=false;}
 if(map_uncertain)return -EAGAIN;
 if(mapped){ret=q6asm_unmap_memory_regions(SNDRV_PCM_STREAM_PLAYBACK,client);pr_info("SUPERMAN_Q6SPEAKER unmap=%d\n",ret);if(ret<0)return ret;mapped=false;}
 if(copp){ret=q6adm_close(&adm_child->dev,copp);pr_info("SUPERMAN_Q6SPEAKER adm_close=%d\n",ret);if(ret<0)return ret;copp=NULL;}
 if(sink_attempt){ret=superman_wcd9302_rx_stop(codec_dev);pr_info("SUPERMAN_Q6SPEAKER sink_stop=%d\n",ret);if(ret)return ret;sink_attempt=false;}
 if(codec_dev){put_device(codec_dev);codec_dev=NULL;}
 if(port_attempt){ret=q6afe_port_stop(afe_port);pr_info("SUPERMAN_Q6SPEAKER afe_stop=%d\n",ret);if(ret)return ret;port_attempt=false;}
 if(afe_port){q6afe_port_put(afe_port);afe_port=NULL;}
 if(client){q6asm_audio_client_free(client);client=NULL;}
 if(backing){dma_free_coherent(&asm_child->dev,8192,backing,address);backing=NULL;}
 if(asm_child){platform_device_unregister(asm_child);asm_child=NULL;}
 if(adm_child){platform_device_unregister(adm_child);adm_child=NULL;}
 if(afe_child){platform_device_unregister(afe_child);afe_child=NULL;}
 retained=false;return 0;
}
static int release_owned(const char *value,const struct kernel_param *kp)
{
 bool yes;int ret;
 ret=kstrtobool(value,&yes);if(ret||!yes)return -EINVAL;
 mutex_lock(&pipe_lock);ret=cleanup();if(!ret&&pinned){pinned=false;module_put(THIS_MODULE);}mutex_unlock(&pipe_lock);return ret;
}
static const struct kernel_param_ops release_ops={.set=release_owned};
module_param_cb(release,&release_ops,NULL,0200);
static int __init start(void)
{
 struct route_payload route={.num_copps=1,.session_id=8,.port_id={SLIMBUS_2_RX}};
 struct q6afe_slim_cfg cfg={.sample_rate=48000,.bit_width=16,.data_format=0,.num_channels=1,.ch_mapping={139}};
 int ret=0,released;unsigned i;
 if(!of_machine_is_compatible("nokia,superman")||!of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 mutex_lock(&pipe_lock);
 asm_child=child(7,"qcom-q6asm","superman-q6speaker-asm");if(IS_ERR(asm_child)){ret=PTR_ERR(asm_child);asm_child=NULL;goto out;}
 adm_child=child(8,"qcom-q6adm","superman-q6speaker-adm");if(IS_ERR(adm_child)){ret=PTR_ERR(adm_child);adm_child=NULL;goto out;}
 afe_child=child(4,"qcom-q6afe","superman-q6speaker-afe");if(IS_ERR(afe_child)){ret=PTR_ERR(afe_child);afe_child=NULL;goto out;}
 backing=dma_alloc_coherent(&asm_child->dev,8192,&address,GFP_KERNEL);if(!backing){ret=-ENOMEM;goto out;}
 for(i=0;i<8192/sizeof(s16);i++)((s16 *)backing)[i]=(i&31)-16;
 client=q6asm_audio_client_alloc(&asm_child->dev,event,NULL,7,LEGACY_PCM_MODE);if(IS_ERR(client)){ret=PTR_ERR(client);client=NULL;goto out;}
 ret=q6asm_open_write(client,1,FORMAT_LINEAR_PCM,0,16,false);pr_info("SUPERMAN_Q6SPEAKER open=%d\n",ret);if(ret)goto out;opened=true;
 ret=q6asm_media_format_block_multi_ch_pcm(client,1,48000,1,NULL,16);if(ret)goto out;
 map_uncertain=true;ret=q6asm_map_memory_regions(SNDRV_PCM_STREAM_PLAYBACK,client,address,4096,2);
 pr_info("SUPERMAN_Q6SPEAKER map=%d\n",ret);if(ret<0)goto out;map_uncertain=false;mapped=true;
 afe_port=q6afe_port_get_from_id(&afe_child->dev,SLIMBUS_2_RX);if(IS_ERR(afe_port)){ret=PTR_ERR(afe_port);afe_port=NULL;goto out;}
 q6afe_slim_port_prepare(afe_port,&cfg);port_attempt=true;
 ret=q6afe_port_start(afe_port);pr_info("SUPERMAN_Q6SPEAKER afe_start=%d\n",ret);if(ret)goto out;
 codec_dev=bus_find_device_by_name(&slimbus_bus,NULL,"217:e0:1:0");
 if(!codec_dev){ret=-ENODEV;goto out;}
 sink_attempt=true;ret=superman_wcd9302_rx_start(codec_dev,16,139);
 pr_info("SUPERMAN_Q6SPEAKER sink_start=%d\n",ret);if(ret)goto out;
 copp=q6adm_open(&adm_child->dev,SLIMBUS_2_RX,ADM_PATH_PLAYBACK,48000,1,NULL_COPP_TOPOLOGY,LEGACY_PCM_MODE,16,0,0);
 if(IS_ERR_OR_NULL(copp)){ret=copp?PTR_ERR(copp):-ENODEV;copp=NULL;pr_info("SUPERMAN_Q6SPEAKER adm_open=%d\n",ret);goto out;}
 route.copp_idx[0]=q6adm_get_copp_id(copp);
 ret=q6adm_matrix_map(&adm_child->dev,ADM_PATH_PLAYBACK,route,LEGACY_PCM_MODE);
 pr_info("SUPERMAN_Q6SPEAKER matrix_map=%d\n",ret);if(ret<0)goto out;
 ret=q6asm_run(client,1,0,0,0);pr_info("SUPERMAN_Q6SPEAKER run=%d\n",ret);if(ret)goto out;
 for(i=0;i<2;i++){ret=q6asm_write_async(client,1,4096,0,0,0);pr_info("SUPERMAN_Q6SPEAKER write index=%u ret=%d\n",i,ret);if(ret<0)goto out;}
 if(!wait_for_completion_timeout(&writes_done,msecs_to_jiffies(1500))){ret=-ETIMEDOUT;goto out;}
 if(atomic_read(&errors)||done_mask!=3){ret=-EIO;goto out;}
 passed=true;ret=0;
out:
 released=cleanup();
 if(released){passed=false;retained=true;pinned=true;__module_get(THIS_MODULE);pr_err("SUPERMAN_Q6SPEAKER cleanup=%d backing/resources retained; release callback available\n",released);mutex_unlock(&pipe_lock);return 0;}
 if(!ret&&passed)pr_info("SUPERMAN_Q6SPEAKER two actual DSP DMA completions PASS, analogue route disabled\n");
 mutex_unlock(&pipe_lock);return ret;
}
static void __exit stop(void) {}
module_init(start);module_exit(stop);
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("Superman real DSP PCM DMA/ASM/ADM/SLIM AFE pipeline check");

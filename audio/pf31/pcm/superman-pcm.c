// SPDX-License-Identifier: GPL-2.0-only
/* Real ALSA mono playback through the Nokia differential speaker route. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/apr.h>
#include <linux/of.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <sound/pcm.h>
#include <sound/core.h>
#include <sound/pcm_params.h>
#include <linux/workqueue.h>
#include <dt-bindings/sound/qcom,q6afe.h>
#include "q6asm.h"
#include "q6adm.h"
#include "q6afe.h"
#include "superman-codec.h"
#include <linux/slimbus.h>
extern const struct bus_type slimbus_bus;
static struct device *codec_dev;
static bool sink_attempt,amp_attempt,common_owned;
#define PERIOD_BYTES 960
#define PERIODS 8
#define RING_BYTES (PERIOD_BYTES*PERIODS)
#define DMA_BYTES PAGE_ALIGN(RING_BYTES)
extern const struct bus_type aprbus;
static struct platform_device *asm_child,*adm_child,*afe_child;
static struct audio_client *client;
static struct q6afe_port *afe_port;
static struct q6copp *copp;
static void *backing;
static dma_addr_t address;
static u64 dma_mask=DMA_BIT_MASK(32);
static bool opened,mapped,map_uncertain,port_attempt,retained,pinned;
static DECLARE_COMPLETION(close_done);
static DECLARE_COMPLETION(eos_done);
static DEFINE_MUTEX(pipe_lock);

module_param(retained,bool,0444);

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

#include "common-resource.h"
static int cleanup(void)
{
 int ret;
 /* Mute the physical speaker before any DSP shutdown can fail. */
 if(amp_attempt){ret=superman_speaker_amp_enable(false);if(ret)return ret;amp_attempt=false;}
 if(opened){ret=q6asm_cmd(client,1,CMD_CLOSE);pr_info("SUPERMAN_PCM close=%d\n",ret);if(ret)return ret;if(!wait_for_completion_timeout(&close_done,msecs_to_jiffies(100)))return -ETIMEDOUT;opened=false;}
 if(map_uncertain)return -EAGAIN;
 if(mapped){ret=q6asm_unmap_memory_regions(SNDRV_PCM_STREAM_PLAYBACK,client);pr_info("SUPERMAN_PCM unmap=%d\n",ret);if(ret<0)return ret;mapped=false;}
 if(copp){ret=q6adm_close(&adm_child->dev,copp);pr_info("SUPERMAN_PCM adm_close=%d\n",ret);if(ret<0)return ret;copp=NULL;}
 /* Stop the DSP consumer before disconnecting its live SLIMbus sink. */
 if(port_attempt){ret=q6afe_port_stop(afe_port);pr_info("SUPERMAN_PCM afe_stop=%d\n",ret);if(ret)return ret;port_attempt=false;}
 if(sink_attempt){ret=superman_wcd9302_rx_stop(codec_dev);pr_info("SUPERMAN_PCM sink_stop=%d\n",ret);if(ret)return ret;sink_attempt=false;}
 if(common_owned){ret=common_put(1);if(ret)return ret;common_owned=false;codec_dev=NULL;}
 if(afe_port){q6afe_port_put(afe_port);afe_port=NULL;}
 if(client){q6asm_audio_client_free_sync(client);client=NULL;}
 retained=false;return 0;
}
static struct snd_card *card;
static struct snd_pcm_substream *playback;
static DEFINE_SPINLOCK(stream_lock);
static bool running,prepared,inflight,need_queue,notify_period,failed;
static unsigned next_index,position;
static unsigned long periods_completed,stream_errors,start_requests,run_acks,worker_calls,submitted,last_worker_flags,run_at,worker_at,stopped_at;
static int last_write_ret;
module_param(last_worker_flags,ulong,0400);module_param(run_at,ulong,0400);module_param(worker_at,ulong,0400);module_param(stopped_at,ulong,0400);module_param(last_write_ret,int,0400);
module_param(start_requests,ulong,0400);module_param(run_acks,ulong,0400);module_param(worker_calls,ulong,0400);module_param(submitted,ulong,0400);
module_param(periods_completed,ulong,0400);
module_param(stream_errors,ulong,0400);
static void period_worker(struct work_struct *work);
static DECLARE_WORK(period_work,period_worker);

/* One DSP buffer at a time; a completion must name that exact ring slot. */
static int completed_period(u32 token,u32 low,u32 high)
{
 unsigned index=token&ASM_WRITE_TOKEN_MASK;
 if(!inflight||index!=next_index||index>=PERIODS||
    low!=lower_32_bits(address+index*PERIOD_BYTES)||
    high!=upper_32_bits(address+index*PERIOD_BYTES))return -EPROTO;
 inflight=false;next_index=(index+1)%PERIODS;
 position=next_index*PERIOD_BYTES;periods_completed++;return 0;
}
static void event(u32 opcode,u32 token,void *payload,void *priv)
{
 unsigned long flags;u32 *data=payload;
 if(opcode==ASM_CLIENT_EVENT_CMD_CLOSE_DONE){complete(&close_done);return;}
 if(opcode==ASM_CLIENT_EVENT_CMD_EOS_DONE){complete(&eos_done);return;}
 spin_lock_irqsave(&stream_lock,flags);
 if(opcode==ASM_CLIENT_EVENT_CMD_RUN_DONE){run_acks++;run_at=jiffies;}
 if(!running)goto out;
 if(opcode==ASM_CLIENT_EVENT_CMD_RUN_DONE){
  if(!data||data[1]){stream_errors++;failed=true;running=false;}
  else need_queue=true;
 }else if(opcode==ASM_CLIENT_EVENT_DATA_WRITE_DONE){
  if(!data||completed_period(token,data[0],data[1])){stream_errors++;failed=true;running=false;}
  else {notify_period=true;need_queue=true;}
 }else goto out;
 schedule_work(&period_work);
out:spin_unlock_irqrestore(&stream_lock,flags);
}
static void period_worker(struct work_struct *work)
{
 struct snd_pcm_substream *s;unsigned long flags;bool notify,error,queue;int ret;
 spin_lock_irqsave(&stream_lock,flags);worker_calls++;s=playback;notify=notify_period;notify_period=false;
 last_worker_flags=(s?1:0)|(running?2:0)|(need_queue?4:0)|(inflight?8:0);worker_at=jiffies;
 error=failed;failed=false;spin_unlock_irqrestore(&stream_lock,flags);
 if(!s)return;
 if(error){snd_pcm_stop_xrun(s);return;}
 if(notify)snd_pcm_period_elapsed(s);
 spin_lock_irqsave(&stream_lock,flags);
 queue=running&&need_queue&&!inflight;
 if(queue){need_queue=false;inflight=true;}
 spin_unlock_irqrestore(&stream_lock,flags);
 if(!queue)return;
 ret=q6asm_write_async(client,1,PERIOD_BYTES,0,0,0);
 last_write_ret=ret;
 if(!ret)submitted++;
 if(ret<0){
  spin_lock_irqsave(&stream_lock,flags);stream_errors++;running=false;inflight=false;
  spin_unlock_irqrestore(&stream_lock,flags);snd_pcm_stop_xrun(s);
 }
}
static void stop_queuing(void)
{
 unsigned long flags;spin_lock_irqsave(&stream_lock,flags);
 running=false;need_queue=false;notify_period=false;
 spin_unlock_irqrestore(&stream_lock,flags);cancel_work_sync(&period_work);
}
static int close_pipe(void)
{
 int ret;
 stop_queuing();prepared=false;ret=cleanup();
 /* q6asm_audio_client_free drains callbacks before backing or owner release. */
 cancel_work_sync(&period_work);
 if(ret){retained=true;if(common_owned)common_blocked|=1;return ret;}
 if(pinned){pinned=false;module_put(THIS_MODULE);}
 return 0;
}
static void free_dma(void)
{
 if(backing){dma_free_coherent(&asm_child->dev,DMA_BYTES,backing,address);backing=NULL;}
}
static int open_pipe(void)
{
 struct route_payload route={.num_copps=1,.session_id=8,.port_id={SLIMBUS_2_RX}};
 struct q6afe_slim_cfg cfg={.sample_rate=48000,.bit_width=16,.data_format=0,.num_channels=1,.ch_mapping={139}};
 int ret;
 if(!backing)return -EINVAL;
 __module_get(THIS_MODULE);pinned=true;
 reinit_completion(&close_done);reinit_completion(&eos_done);
 client=q6asm_audio_client_alloc(&asm_child->dev,event,NULL,7,LEGACY_PCM_MODE);
 if(IS_ERR(client)){ret=PTR_ERR(client);client=NULL;return ret;}
 ret=q6asm_open_write(client,1,FORMAT_LINEAR_PCM,0,16,false);if(ret)return ret;opened=true;
 ret=q6asm_media_format_block_multi_ch_pcm(client,1,48000,1,NULL,16);if(ret)return ret;
 map_uncertain=true;ret=q6asm_map_memory_regions(SNDRV_PCM_STREAM_PLAYBACK,client,address,PERIOD_BYTES,PERIODS);
 if(ret<0)return ret;
 map_uncertain=false;mapped=true;
 afe_port=q6afe_port_get_from_id(&afe_child->dev,SLIMBUS_2_RX);
 if(IS_ERR(afe_port)){ret=PTR_ERR(afe_port);afe_port=NULL;return ret;}
 q6afe_slim_port_prepare(afe_port,&cfg);
 ret=common_get(1,&codec_dev,&common_owned);if(ret)return ret;
 sink_attempt=true;ret=superman_wcd9302_rx_start(codec_dev,16,139);if(ret)return ret;
 port_attempt=true;ret=q6afe_port_start(afe_port);if(ret)return ret;
 copp=q6adm_open(&adm_child->dev,SLIMBUS_2_RX,ADM_PATH_PLAYBACK,48000,1,NULL_COPP_TOPOLOGY,LEGACY_PCM_MODE,16,0,0);
 if(IS_ERR_OR_NULL(copp)){ret=copp?PTR_ERR(copp):-ENODEV;copp=NULL;return ret;}
 route.copp_idx[0]=q6adm_get_copp_id(copp);ret=q6adm_matrix_map(&adm_child->dev,ADM_PATH_PLAYBACK,route,LEGACY_PCM_MODE);if(ret<0)return ret;
 amp_attempt=true;ret=superman_speaker_amp_enable(true);if(ret)return ret;
 prepared=true;return 0;
}
static const struct snd_pcm_hardware hardware={
 .info=SNDRV_PCM_INFO_INTERLEAVED|SNDRV_PCM_INFO_BLOCK_TRANSFER|SNDRV_PCM_INFO_BATCH,
 .formats=SNDRV_PCM_FMTBIT_S16_LE,.rates=SNDRV_PCM_RATE_48000,
 .rate_min=48000,.rate_max=48000,.channels_min=1,.channels_max=1,
 .buffer_bytes_max=RING_BYTES,.period_bytes_min=PERIOD_BYTES,.period_bytes_max=PERIOD_BYTES,
 .periods_min=PERIODS,.periods_max=PERIODS,
};
static int pcm_open(struct snd_pcm_substream *s)
{
 unsigned long flags;int ret;
 mutex_lock(&pipe_lock);
 if(retained||playback){ret=-EBUSY;goto out;}
 s->runtime->hw=hardware;
 ret=snd_pcm_hw_constraint_integer(s->runtime,SNDRV_PCM_HW_PARAM_PERIODS);if(ret<0)goto out;
 ret=0;
 spin_lock_irqsave(&stream_lock,flags);playback=s;spin_unlock_irqrestore(&stream_lock,flags);
out:mutex_unlock(&pipe_lock);return ret;
}
static void clear_runtime(struct snd_pcm_substream *s)
{
 s->runtime->dma_area=NULL;s->runtime->dma_addr=0;s->runtime->dma_bytes=0;
}
static int pcm_hw_free(struct snd_pcm_substream *s)
{
 int ret;mutex_lock(&pipe_lock);ret=close_pipe();
 if(!ret){free_dma();clear_runtime(s);}mutex_unlock(&pipe_lock);return ret;
}
static int pcm_close(struct snd_pcm_substream *s)
{
 unsigned long flags;int ret;
 mutex_lock(&pipe_lock);spin_lock_irqsave(&stream_lock,flags);running=false;playback=NULL;
 spin_unlock_irqrestore(&stream_lock,flags);ret=close_pipe();
 if(!ret)free_dma();
 clear_runtime(s);mutex_unlock(&pipe_lock);return ret;
}
static int pcm_hw_params(struct snd_pcm_substream *s,struct snd_pcm_hw_params *p)
{
 int ret=0;mutex_lock(&pipe_lock);
 if(backing||retained){ret=-EBUSY;goto out;}
 if(params_rate(p)!=48000||params_channels(p)!=1||params_format(p)!=SNDRV_PCM_FORMAT_S16_LE||
    params_period_bytes(p)!=PERIOD_BYTES||params_buffer_bytes(p)!=RING_BYTES){ret=-EINVAL;goto out;}
 backing=dma_alloc_coherent(&asm_child->dev,DMA_BYTES,&address,GFP_KERNEL);
 if(!backing){ret=-ENOMEM;goto out;}
 memset(backing,0,DMA_BYTES);s->runtime->dma_area=backing;s->runtime->dma_addr=address;s->runtime->dma_bytes=RING_BYTES;
out:mutex_unlock(&pipe_lock);return ret;
}
static int pcm_prepare(struct snd_pcm_substream *s)
{
 unsigned long flags;int ret,cleanup_ret;mutex_lock(&pipe_lock);
 ret=close_pipe();if(ret)goto out;
 spin_lock_irqsave(&stream_lock,flags);next_index=position=0;inflight=failed=false;
 spin_unlock_irqrestore(&stream_lock,flags);
 ret=open_pipe();if(ret){cleanup_ret=close_pipe();if(cleanup_ret)pr_err("SUPERMAN_PCM setup=%d cleanup=%d retained\n",ret,cleanup_ret);}
out:mutex_unlock(&pipe_lock);return ret;
}
static int pcm_trigger(struct snd_pcm_substream *s,int command)
{
 unsigned long flags;int ret;
 switch(command){
 case SNDRV_PCM_TRIGGER_START:
  start_requests++;
  if(!READ_ONCE(prepared))return -EPIPE;
  spin_lock_irqsave(&stream_lock,flags);running=true;spin_unlock_irqrestore(&stream_lock,flags);
  ret=q6asm_run_nowait(client,1,0,0,0);
  if(ret<0){spin_lock_irqsave(&stream_lock,flags);running=false;spin_unlock_irqrestore(&stream_lock,flags);}
  return ret;
 case SNDRV_PCM_TRIGGER_STOP:
  stopped_at=jiffies;
  spin_lock_irqsave(&stream_lock,flags);running=false;need_queue=false;spin_unlock_irqrestore(&stream_lock,flags);
  return opened?q6asm_cmd_nowait(client,1,CMD_EOS):0;
 default:return -EINVAL;
 }
}
static int pcm_sync_stop(struct snd_pcm_substream *s)
{
 int ret;mutex_lock(&pipe_lock);ret=close_pipe();mutex_unlock(&pipe_lock);return ret;
}
static snd_pcm_uframes_t pcm_pointer(struct snd_pcm_substream *s)
{
 return bytes_to_frames(s->runtime,READ_ONCE(position));
}
static const struct snd_pcm_ops pcm_ops={
 .open=pcm_open,.close=pcm_close,.ioctl=snd_pcm_lib_ioctl,.hw_params=pcm_hw_params,
 .hw_free=pcm_hw_free,.prepare=pcm_prepare,.trigger=pcm_trigger,.sync_stop=pcm_sync_stop,.pointer=pcm_pointer,
};
static int release_owned(const char *value,const struct kernel_param *kp)
{
 bool yes;int ret=kstrtobool(value,&yes);if(ret||!yes)return -EINVAL;
 mutex_lock(&pipe_lock);
 if(playback)ret=-EBUSY;
 else {ret=close_pipe();if(!ret)free_dma();}
 mutex_unlock(&pipe_lock);return ret;
}
static const struct kernel_param_ops release_ops={.set=release_owned};
module_param_cb(release,&release_ops,NULL,0200);
#include "mic-pcm.h"
static void free_children(void)
{
 if(afe_child){platform_device_unregister(afe_child);afe_child=NULL;}
 if(adm_child){platform_device_unregister(adm_child);adm_child=NULL;}
 if(asm_child){platform_device_unregister(asm_child);asm_child=NULL;}
}
static int __init start(void)
{
 struct snd_pcm *pcm;int ret;
 if(!of_machine_is_compatible("nokia,superman")||!of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 asm_child=child(7,"qcom-q6asm","superman-pcm-asm");if(IS_ERR(asm_child)){ret=PTR_ERR(asm_child);asm_child=NULL;goto fail;}
 adm_child=child(8,"qcom-q6adm","superman-pcm-adm");if(IS_ERR(adm_child)){ret=PTR_ERR(adm_child);adm_child=NULL;goto fail;}
 afe_child=child(4,"qcom-q6afe","superman-pcm-afe");if(IS_ERR(afe_child)){ret=PTR_ERR(afe_child);afe_child=NULL;goto fail;}
 ret=snd_card_new(&asm_child->dev,-1,"SupermanAudio",THIS_MODULE,0,&card);if(ret)goto fail;
 strscpy(card->driver,"SupermanQ6",sizeof(card->driver));strscpy(card->shortname,"SupermanAudio",sizeof(card->shortname));
 strscpy(card->longname,"Lumia 730 Q6 WCD9302 differential speaker",sizeof(card->longname));
 ret=snd_pcm_new(card,"Speaker and microphone",0,1,1,&pcm);if(ret)goto card_fail;
 snd_pcm_set_ops(pcm,SNDRV_PCM_STREAM_PLAYBACK,&pcm_ops);snd_pcm_set_ops(pcm,SNDRV_PCM_STREAM_CAPTURE,&mic_pcm_ops);strscpy(pcm->name,"WCD9302 speaker",sizeof(pcm->name));
 ret=snd_card_register(card);if(ret)goto card_fail;
 pr_info("SUPERMAN_PCM real playback card ready; mono48k16, physical DMIC2 capture, hardware idle until prepare\n");return 0;
card_fail:snd_card_free(card);card=NULL;
fail:free_children();return ret;
}
static void __exit stop(void)
{
 snd_card_free(card);cancel_work_sync(&period_work);cancel_work_sync(&mic_period_work);free_children();
}
module_init(start);module_exit(stop);
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("Lumia 730 hardware-backed ALSA Q6 differential speaker playback");

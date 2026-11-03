#define MIC_PERIOD_BYTES 960
#define MIC_PERIODS 4
#define MIC_RING_BYTES (MIC_PERIOD_BYTES*MIC_PERIODS)
#define MIC_DMA_BYTES PAGE_ALIGN(MIC_RING_BYTES)
/* Real mono48k DMIC2 capture; common supply is held until both directions stop. */
#include <linux/regmap.h>
#include <linux/delay.h>
#include "slimbus.h"
#include "codec-sequence.h"
#include "mic-program.h"
static struct audio_client *mic_client;
static struct q6afe_port *mic_afe_port;
static struct q6copp *mic_copp;
static struct device *mic_pgd,*mic_ifd,*mic_codec_dev;
static void *mic_backing;
static dma_addr_t mic_address;
static bool mic_opened,mic_mapped,mic_map_uncertain,mic_port_attempt,mic_tx_owned,mic_connect_attempt,mic_common_owned,mic_retained,mic_pinned;
static u8 mic_saved[4];
static struct regmap *mic_codec_map;
static struct codec_saved mic_mic_saved[CODEC_SAVED_MAX];
static unsigned mic_mic_saved_count;
static bool mic_mic_owned;
module_param(mic_retained,bool,0444);
static DECLARE_COMPLETION(mic_close_done);
static DECLARE_COMPLETION(mic_eos_done);
static void mic_event(u32 opcode,u32 token,void *payload,void *priv);
static int mic_read_byte(struct device *dev,u16 reg,u8 *v){return slim_read(to_slim_device(dev),0x800+reg,1,v);}
static int mic_write_byte(struct device *dev,u16 reg,u8 v)
{
 u8 check;int ret=slim_write(to_slim_device(dev),0x800+reg,1,&v);if(ret)return ret;
 ret=mic_read_byte(dev,reg,&check);return ret?:check==v?0:-EIO;
}
static int mic_tx_message(bool connect)
{
 u8 bytes[]={0,154};struct slim_device *s=to_slim_device(mic_pgd);
 struct slim_val_inf msg={.num_bytes=connect?2:1,.wbuf=bytes};
 DEFINE_SLIM_LDEST_TXN(txn,SLIM_MSG_MC_DISCONNECT_PORT,5,s->laddr,&msg);
 if(connect){txn.mc=SLIM_MSG_MC_CONNECT_SOURCE;txn.rl=6;}
 return slim_do_transfer(s->ctrl,&txn);
}
static int mic_tx_setup(void)
{
 static const u8 expected[]={1,0,3,1,0x21};u8 id[5],v;unsigned i;int ret;
 mic_pgd=bus_find_device_by_name(&slimbus_bus,NULL,"217:e0:1:0");mic_ifd=bus_find_device_by_name(&slimbus_bus,NULL,"217:e0:0:0");if(!mic_pgd||!mic_ifd)return -ENODEV;
 for(i=0;i<ARRAY_SIZE(id);i++){ret=mic_read_byte(mic_pgd,4+i,&id[i]);if(ret)return ret;}
 if(memcmp(id,expected,sizeof(id)))return -ENODEV;
 ret=mic_read_byte(mic_pgd,0x386,&v);if(ret)return ret;if(v!=5)return -EBUSY;
 ret=mic_read_byte(mic_pgd,0x30f,&v);if(ret)return ret;if(!(v&4))return -EBUSY;
 ret=mic_read_byte(mic_pgd,0x3a3,&mic_saved[0]);if(ret)return ret;
 ret=mic_read_byte(mic_ifd,0x050,&mic_saved[1]);if(ret)return ret;
 ret=mic_read_byte(mic_ifd,0x100,&mic_saved[2]);if(ret)return ret;
 ret=mic_read_byte(mic_ifd,0x101,&mic_saved[3]);if(ret)return ret;
 if(!common_tx_configured||(mic_saved[0]&0x0f)||mic_saved[1]!=5||mic_saved[2]!=1||mic_saved[3])return -EBUSY;
 mic_tx_owned=true;
 mic_codec_map=dev_get_regmap(mic_pgd,NULL);if(!mic_codec_map)return -ENODEV;
 ret=mic_read_byte(mic_pgd,0x12b,&v);if(ret)return ret;if(v&0x80)return -EBUSY;
 ret=mic_read_byte(mic_pgd,0x30a,&v);if(ret)return ret;if(v&1)return -EBUSY;
 ret=mic_read_byte(mic_pgd,0x304,&v);if(ret)return ret;if(v&1)return -EBUSY;
 ret=codec_snapshot(mic_codec_map,mic_program,ARRAY_SIZE(mic_program),mic_mic_saved,&mic_mic_saved_count);if(ret)return ret;
 mic_mic_owned=true;ret=codec_apply(mic_codec_map,mic_program,ARRAY_SIZE(mic_program));if(ret)return ret;
 /* TX1 selects DEC1 (8), 16-bit words (2), never the RX mixer loopback. */
 ret=mic_write_byte(mic_pgd,0x3a3,(mic_saved[0]&~0x3f)|0x28);if(ret)return ret;
 /* The common lease owns the interface FIFO; capture owns only this source. */
 mic_connect_attempt=true;return mic_tx_message(true);
}
static int mic_tx_restore(void)
{
 int ret;if(!mic_tx_owned)return 0;
 if(mic_connect_attempt){ret=mic_tx_message(false);if(ret)return ret;mic_connect_attempt=false;}
 ret=mic_write_byte(mic_pgd,0x3a3,mic_saved[0]);if(ret)return ret;
 if(mic_mic_owned){
  ret=regmap_update_bits(mic_codec_map,0x222,1,1);if(ret)return ret;
  ret=regmap_update_bits(mic_codec_map,0x304,1,0);if(ret)return ret;
  ret=regmap_update_bits(mic_codec_map,0x12b,0x80,0);if(ret)return ret;
  ret=codec_restore(mic_codec_map,mic_mic_saved,mic_mic_saved_count);if(ret)return ret;
  mic_mic_owned=false;mic_mic_saved_count=0;
 }
 mic_tx_owned=false;return 0;
}
static int mic_cleanup(void)
{
 int ret;
 if(mic_opened){ret=q6asm_cmd(mic_client,1,CMD_CLOSE);if(ret)return ret;if(!wait_for_completion_timeout(&mic_close_done,msecs_to_jiffies(100)))return -ETIMEDOUT;mic_opened=false;}
 if(mic_map_uncertain)return -EAGAIN;
 if(mic_mapped){ret=q6asm_unmap_memory_regions(SNDRV_PCM_STREAM_CAPTURE,mic_client);if(ret<0)return ret;mic_mapped=false;}
 if(mic_copp){ret=q6adm_close(&adm_child->dev,mic_copp);if(ret<0)return ret;mic_copp=NULL;}
 if(mic_port_attempt){ret=q6afe_port_stop(mic_afe_port);if(ret)return ret;mic_port_attempt=false;}
 ret=mic_tx_restore();if(ret)return ret;
 if(mic_common_owned){ret=common_put(2);if(ret)return ret;mic_common_owned=false;mic_codec_dev=NULL;}
 if(mic_afe_port){q6afe_port_put(mic_afe_port);mic_afe_port=NULL;}
 if(mic_client){q6asm_audio_client_free_sync(mic_client);mic_client=NULL;}
 if(mic_ifd){put_device(mic_ifd);mic_ifd=NULL;}if(mic_pgd){put_device(mic_pgd);mic_pgd=NULL;}
 mic_retained=false;return 0;
}
static struct snd_pcm_substream *mic_playback;
static bool mic_running,mic_prepared,mic_inflight,mic_need_queue,mic_notify_period,mic_failed;
static unsigned mic_next_index,mic_position;
static unsigned long mic_periods_completed,mic_stream_errors,mic_start_requests,mic_run_acks,mic_worker_calls,mic_submitted,mic_last_worker_flags,mic_run_at,mic_worker_at,mic_stopped_at;
static int mic_last_write_ret;
module_param(mic_last_worker_flags,ulong,0400);module_param(mic_run_at,ulong,0400);module_param(mic_worker_at,ulong,0400);module_param(mic_stopped_at,ulong,0400);module_param(mic_last_write_ret,int,0400);
module_param(mic_start_requests,ulong,0400);module_param(mic_run_acks,ulong,0400);module_param(mic_worker_calls,ulong,0400);module_param(mic_submitted,ulong,0400);
module_param(mic_periods_completed,ulong,0400);
module_param(mic_stream_errors,ulong,0400);
static void mic_period_worker(struct work_struct *work);
static DECLARE_WORK(mic_period_work,mic_period_worker);

/* One DSP buffer at a time; a completion must name that exact ring slot. */
static int mic_completed_period(u32 token,u32 low,u32 high)
{
 unsigned index=token&ASM_WRITE_TOKEN_MASK;
 if(!mic_inflight||index!=mic_next_index||index>=MIC_PERIODS||
    low!=lower_32_bits(mic_address+index*MIC_PERIOD_BYTES)||
    high!=upper_32_bits(mic_address+index*MIC_PERIOD_BYTES))return -EPROTO;
 mic_inflight=false;mic_next_index=(index+1)%MIC_PERIODS;
 mic_position=mic_next_index*MIC_PERIOD_BYTES;mic_periods_completed++;return 0;
}
static void mic_event(u32 opcode,u32 token,void *payload,void *priv)
{
 unsigned long flags;u32 *data=payload;
 if(opcode==ASM_CLIENT_EVENT_CMD_CLOSE_DONE){complete(&mic_close_done);return;}
 if(opcode==ASM_CLIENT_EVENT_CMD_EOS_DONE){complete(&mic_eos_done);return;}
 spin_lock_irqsave(&stream_lock,flags);
 if(opcode==ASM_CLIENT_EVENT_CMD_RUN_DONE){mic_run_acks++;mic_run_at=jiffies;}
 if(!mic_running)goto out;
 if(opcode==ASM_CLIENT_EVENT_CMD_RUN_DONE){
  if(!data||data[1]){mic_stream_errors++;mic_failed=true;mic_running=false;}
  else mic_need_queue=true;
 }else if(opcode==ASM_CLIENT_EVENT_DATA_READ_DONE){
  if(!data||data[0]||mic_completed_period(token,data[1],data[2])){mic_stream_errors++;mic_failed=true;mic_running=false;}
  else {mic_notify_period=true;mic_need_queue=true;}
 }else goto out;
 schedule_work(&mic_period_work);
out:spin_unlock_irqrestore(&stream_lock,flags);
}
static void mic_period_worker(struct work_struct *work)
{
 struct snd_pcm_substream *s;unsigned long flags;bool notify,error,queue;int ret;
 spin_lock_irqsave(&stream_lock,flags);mic_worker_calls++;s=mic_playback;notify=mic_notify_period;mic_notify_period=false;
 mic_last_worker_flags=(s?1:0)|(mic_running?2:0)|(mic_need_queue?4:0)|(mic_inflight?8:0);mic_worker_at=jiffies;
 error=mic_failed;mic_failed=false;spin_unlock_irqrestore(&stream_lock,flags);
 if(!s)return;
 if(error){snd_pcm_stop_xrun(s);return;}
 if(notify)snd_pcm_period_elapsed(s);
 spin_lock_irqsave(&stream_lock,flags);
 queue=mic_running&&mic_need_queue&&!mic_inflight;
 if(queue){mic_need_queue=false;mic_inflight=true;}
 spin_unlock_irqrestore(&stream_lock,flags);
 if(!queue)return;
 ret=q6asm_read(mic_client,1);
 mic_last_write_ret=ret;
 if(!ret)mic_submitted++;
 if(ret<0){
  spin_lock_irqsave(&stream_lock,flags);mic_stream_errors++;mic_running=false;mic_inflight=false;
  spin_unlock_irqrestore(&stream_lock,flags);snd_pcm_stop_xrun(s);
 }
}
static void mic_stop_queuing(void)
{
 unsigned long flags;spin_lock_irqsave(&stream_lock,flags);
 mic_running=false;mic_need_queue=false;mic_notify_period=false;
 spin_unlock_irqrestore(&stream_lock,flags);cancel_work_sync(&mic_period_work);
}
static int mic_close_pipe(void)
{
 int ret;
 mic_stop_queuing();mic_prepared=false;ret=mic_cleanup();
 /* q6asm_audio_client_free drains callbacks before mic_backing or owner release. */
 cancel_work_sync(&mic_period_work);
 if(ret){mic_retained=true;if(mic_common_owned)common_blocked|=2;return ret;}
 if(mic_pinned){mic_pinned=false;module_put(THIS_MODULE);}
 return 0;
}
static void mic_free_dma(void)
{
 if(mic_backing){dma_free_coherent(&asm_child->dev,MIC_DMA_BYTES,mic_backing,mic_address);mic_backing=NULL;}
}
static int mic_open_pipe(void)
{
 struct q6afe_slim_cfg cfg={.sample_rate=48000,.bit_width=16,.data_format=0,.num_channels=1,.ch_mapping={154}};
 struct route_payload route={.num_copps=1,.session_id=7,.port_id={SLIMBUS_2_TX}};
 int ret;
 if(!mic_backing)return -EINVAL;
 __module_get(THIS_MODULE);mic_pinned=true;reinit_completion(&mic_close_done);
 mic_client=q6asm_audio_client_alloc(&asm_child->dev,mic_event,NULL,6,LEGACY_PCM_MODE);
 if(IS_ERR(mic_client)){ret=PTR_ERR(mic_client);mic_client=NULL;return ret;}
 ret=q6asm_open_read(mic_client,1,FORMAT_LINEAR_PCM,16);if(ret)return ret;mic_opened=true;
 ret=q6asm_enc_cfg_blk_pcm_format_support(mic_client,1,48000,1,16);if(ret)return ret;
 mic_map_uncertain=true;ret=q6asm_map_memory_regions(SNDRV_PCM_STREAM_CAPTURE,mic_client,mic_address,MIC_PERIOD_BYTES,MIC_PERIODS);if(ret<0)return ret;mic_map_uncertain=false;mic_mapped=true;
 ret=common_get(2,&mic_codec_dev,&mic_common_owned);if(ret)return ret;
 ret=mic_tx_setup();if(ret)return ret;
 mic_afe_port=q6afe_port_get_from_id(&afe_child->dev,SLIMBUS_2_TX);if(IS_ERR(mic_afe_port)){ret=PTR_ERR(mic_afe_port);mic_afe_port=NULL;return ret;}
 q6afe_slim_port_prepare(mic_afe_port,&cfg);mic_port_attempt=true;ret=q6afe_port_start(mic_afe_port);if(ret)return ret;
 mic_copp=q6adm_open(&adm_child->dev,SLIMBUS_2_TX,ADM_PATH_LIVE_REC,48000,1,NULL_COPP_TOPOLOGY,LEGACY_PCM_MODE,16,0,0);
 if(IS_ERR_OR_NULL(mic_copp)){ret=mic_copp?PTR_ERR(mic_copp):-ENODEV;mic_copp=NULL;return ret;}
 route.copp_idx[0]=q6adm_get_copp_id(mic_copp);ret=q6adm_matrix_map(&adm_child->dev,ADM_PATH_LIVE_REC,route,LEGACY_PCM_MODE);if(ret<0)return ret;
 mic_prepared=true;return 0;
}
static const struct snd_pcm_hardware mic_hardware={
 .info=SNDRV_PCM_INFO_INTERLEAVED|SNDRV_PCM_INFO_BLOCK_TRANSFER|SNDRV_PCM_INFO_BATCH,
 .formats=SNDRV_PCM_FMTBIT_S16_LE,.rates=SNDRV_PCM_RATE_48000,
 .rate_min=48000,.rate_max=48000,.channels_min=1,.channels_max=1,
 .buffer_bytes_max=MIC_RING_BYTES,.period_bytes_min=MIC_PERIOD_BYTES,.period_bytes_max=MIC_PERIOD_BYTES,
 .periods_min=MIC_PERIODS,.periods_max=MIC_PERIODS,
};
static int mic_pcm_open(struct snd_pcm_substream *s)
{
 unsigned long flags;int ret;
 mutex_lock(&pipe_lock);
 if(mic_retained||mic_playback){ret=-EBUSY;goto out;}
 s->runtime->hw=mic_hardware;
 ret=snd_pcm_hw_constraint_integer(s->runtime,SNDRV_PCM_HW_PARAM_PERIODS);if(ret<0)goto out;
 ret=0;
 spin_lock_irqsave(&stream_lock,flags);mic_playback=s;spin_unlock_irqrestore(&stream_lock,flags);
out:mutex_unlock(&pipe_lock);return ret;
}
static void mic_clear_runtime(struct snd_pcm_substream *s)
{
 s->runtime->dma_area=NULL;s->runtime->dma_addr=0;s->runtime->dma_bytes=0;
}
static int mic_pcm_hw_free(struct snd_pcm_substream *s)
{
 int ret;mutex_lock(&pipe_lock);ret=mic_close_pipe();
 if(!ret){mic_free_dma();mic_clear_runtime(s);}mutex_unlock(&pipe_lock);return ret;
}
static int mic_pcm_close(struct snd_pcm_substream *s)
{
 unsigned long flags;int ret;
 mutex_lock(&pipe_lock);spin_lock_irqsave(&stream_lock,flags);mic_running=false;mic_playback=NULL;
 spin_unlock_irqrestore(&stream_lock,flags);ret=mic_close_pipe();
 if(!ret)mic_free_dma();
 mic_clear_runtime(s);mutex_unlock(&pipe_lock);return ret;
}
static int mic_pcm_hw_params(struct snd_pcm_substream *s,struct snd_pcm_hw_params *p)
{
 int ret=0;mutex_lock(&pipe_lock);
 if(mic_backing||mic_retained){ret=-EBUSY;goto out;}
 if(params_rate(p)!=48000||params_channels(p)!=1||params_format(p)!=SNDRV_PCM_FORMAT_S16_LE||
    params_period_bytes(p)!=MIC_PERIOD_BYTES||params_buffer_bytes(p)!=MIC_RING_BYTES){ret=-EINVAL;goto out;}
 mic_backing=dma_alloc_coherent(&asm_child->dev,MIC_DMA_BYTES,&mic_address,GFP_KERNEL);
 if(!mic_backing){ret=-ENOMEM;goto out;}
 memset(mic_backing,0,MIC_DMA_BYTES);s->runtime->dma_area=mic_backing;s->runtime->dma_addr=mic_address;s->runtime->dma_bytes=MIC_RING_BYTES;
out:mutex_unlock(&pipe_lock);return ret;
}
static int mic_pcm_prepare(struct snd_pcm_substream *s)
{
 unsigned long flags;int ret,cleanup_ret;mutex_lock(&pipe_lock);
 ret=mic_close_pipe();if(ret)goto out;
 spin_lock_irqsave(&stream_lock,flags);mic_next_index=mic_position=0;mic_inflight=mic_failed=false;
 spin_unlock_irqrestore(&stream_lock,flags);
 ret=mic_open_pipe();if(ret){cleanup_ret=mic_close_pipe();if(cleanup_ret)pr_err("SUPERMAN_MIC_PCM setup=%d mic_cleanup=%d mic_retained\n",ret,cleanup_ret);}
out:mutex_unlock(&pipe_lock);return ret;
}
static int mic_pcm_trigger(struct snd_pcm_substream *s,int command)
{
 unsigned long flags;int ret;
 switch(command){
 case SNDRV_PCM_TRIGGER_START:
  mic_start_requests++;
  if(!READ_ONCE(mic_prepared))return -EPIPE;
  spin_lock_irqsave(&stream_lock,flags);mic_running=true;spin_unlock_irqrestore(&stream_lock,flags);
  ret=q6asm_run_nowait(mic_client,1,0,0,0);
  if(ret<0){spin_lock_irqsave(&stream_lock,flags);mic_running=false;spin_unlock_irqrestore(&stream_lock,flags);}
  return ret;
 case SNDRV_PCM_TRIGGER_STOP:
  mic_stopped_at=jiffies;
  spin_lock_irqsave(&stream_lock,flags);mic_running=false;mic_need_queue=false;spin_unlock_irqrestore(&stream_lock,flags);
  return 0;
 default:return -EINVAL;
 }
}
static int mic_pcm_sync_stop(struct snd_pcm_substream *s)
{
 int ret;mutex_lock(&pipe_lock);ret=mic_close_pipe();mutex_unlock(&pipe_lock);return ret;
}
static snd_pcm_uframes_t mic_pcm_pointer(struct snd_pcm_substream *s)
{
 return bytes_to_frames(s->runtime,READ_ONCE(mic_position));
}
static const struct snd_pcm_ops mic_pcm_ops={
 .open=mic_pcm_open,.close=mic_pcm_close,.ioctl=snd_pcm_lib_ioctl,.hw_params=mic_pcm_hw_params,
 .hw_free=mic_pcm_hw_free,.prepare=mic_pcm_prepare,.trigger=mic_pcm_trigger,.sync_stop=mic_pcm_sync_stop,.pointer=mic_pcm_pointer,
};

static int mic_release_owned(const char *value,const struct kernel_param *kp)
{
 bool yes;int ret=kstrtobool(value,&yes);if(ret||!yes)return -EINVAL;
 mutex_lock(&pipe_lock);
 if(mic_playback)ret=-EBUSY;
 else {ret=mic_close_pipe();if(!ret)mic_free_dma();}
 mutex_unlock(&pipe_lock);return ret;
}
static const struct kernel_param_ops mic_release_ops={.set=mic_release_owned};
module_param_cb(mic_release,&mic_release_ops,NULL,0200);

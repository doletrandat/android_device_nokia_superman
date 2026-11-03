// SPDX-License-Identifier: GPL-2.0-only
/* Bounded real PCM tone through the Nokia differential speaker route. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/apr.h>
#include <linux/of.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <sound/pcm.h>
#include <dt-bindings/sound/qcom,q6afe.h>
#include "q6asm.h"
#include "q6adm.h"
#include "q6afe.h"
#include "superman-codec.h"
#include <linux/slimbus.h>
extern const struct bus_type slimbus_bus;
static struct device *codec_dev;
static bool sink_attempt,lineout_attempt,amp_attempt,power_attempt;
#define PERIOD_BYTES 49152
#define TONE_PERIODS 8
#define TONE_DONE_MASK ((1UL<<TONE_PERIODS)-1)
#define DMA_BYTES (TONE_PERIODS*PERIOD_BYTES)
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
static DECLARE_COMPLETION(eos_done);
static DEFINE_MUTEX(pipe_lock);
module_param(passed,bool,0444);
module_param(retained,bool,0444);
static void event(u32 opcode,u32 token,void *payload,void *priv)
{
 if(opcode==ASM_CLIENT_EVENT_CMD_EOS_DONE){complete(&eos_done);return;}
 if(opcode==ASM_CLIENT_EVENT_CMD_CLOSE_DONE){complete(&close_done);return;}
 if(opcode==ASM_CLIENT_EVENT_DATA_WRITE_DONE) {
  u32 *data=payload;unsigned index=token&ASM_WRITE_TOKEN_MASK;
  if(index>=TONE_PERIODS||data[0]!=lower_32_bits(address+index*PERIOD_BYTES)||data[1]!=upper_32_bits(address+index*PERIOD_BYTES))atomic_inc(&errors);
  else set_bit(index,&done_mask);
  pr_info("SUPERMAN_SPEAKER_TONE write_done index=%u address=%08x:%08x\n",index,data[1],data[0]);
  if(READ_ONCE(done_mask)==TONE_DONE_MASK||atomic_read(&errors))complete(&writes_done);
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
static int signal_status(const char *phase)
{
 struct slim_device *pgd=to_slim_device(codec_dev),*ifd;
 struct slim_eaddr ea=pgd->e_addr;
 u8 irq[4],source,cfg,multi,line1,line2,clocks[4];int ret;
 ea.dev_index=0;ifd=slim_get_device(pgd->ctrl,&ea);if(IS_ERR(ifd))return PTR_ERR(ifd);
 ret=slim_read(ifd,0x834,4,irq);if(ret)goto out;
 ret=slim_read(ifd,0x860,1,&source);if(ret)goto out;
 ret=slim_read(ifd,0x840,1,&cfg);if(ret)goto out;
 ret=slim_read(ifd,0x980,1,&multi);if(ret)goto out;
 ret=slim_read(pgd,0x9d0,1,&line1);if(ret)goto out;
 ret=slim_read(pgd,0x9d4,1,&line2);if(ret)goto out;
 ret=slim_read(pgd,0xb0c,4,clocks);if(ret)goto out;
 pr_info("SUPERMAN_SIGNAL phase=%s IFD_IRQ=%02x:%02x:%02x:%02x RX0_SRC=%02x CFG=%02x MULTI=%02x LINE_STATUS=%02x:%02x CLK30c30d30e30f=%02x:%02x:%02x:%02x read_only=true\n",phase,irq[0],irq[1],irq[2],irq[3],source,cfg,multi,line1,line2,clocks[0],clocks[1],clocks[2],clocks[3]);
out:put_device(&ifd->dev);return ret;
}
static int cleanup(void)
{
 int ret;
 /* Mute the physical speaker before any DSP shutdown can fail. */
 if(amp_attempt){ret=superman_speaker_amp_enable(false);if(ret)return ret;amp_attempt=false;}
 if(opened){ret=q6asm_cmd(client,1,CMD_CLOSE);pr_info("SUPERMAN_SPEAKER_TONE close=%d\n",ret);if(ret)return ret;if(!wait_for_completion_timeout(&close_done,msecs_to_jiffies(100)))return -ETIMEDOUT;opened=false;}
 if(map_uncertain)return -EAGAIN;
 if(mapped){ret=q6asm_unmap_memory_regions(SNDRV_PCM_STREAM_PLAYBACK,client);pr_info("SUPERMAN_SPEAKER_TONE unmap=%d\n",ret);if(ret<0)return ret;mapped=false;}
 if(copp){ret=q6adm_close(&adm_child->dev,copp);pr_info("SUPERMAN_SPEAKER_TONE adm_close=%d\n",ret);if(ret<0)return ret;copp=NULL;}
 if(sink_attempt){ret=superman_wcd9302_rx_stop(codec_dev);pr_info("SUPERMAN_SPEAKER_TONE sink_stop=%d\n",ret);if(ret)return ret;sink_attempt=false;}
 if(port_attempt){ret=q6afe_port_stop(afe_port);pr_info("SUPERMAN_SPEAKER_TONE afe_stop=%d\n",ret);if(ret)return ret;port_attempt=false;}
 if(lineout_attempt){ret=superman_wcd9302_lineout_stop(codec_dev);if(ret)return ret;lineout_attempt=false;}
 if(power_attempt){ret=superman_audio_board_power_enable(false);if(ret)return ret;power_attempt=false;}
 if(codec_dev){put_device(codec_dev);codec_dev=NULL;}
 if(afe_port){q6afe_port_put(afe_port);afe_port=NULL;}
 if(client){q6asm_audio_client_free(client);client=NULL;}
 if(backing){dma_free_coherent(&asm_child->dev,DMA_BYTES,backing,address);backing=NULL;}
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
 asm_child=child(7,"qcom-q6asm","superman-speaker-tone-asm");if(IS_ERR(asm_child)){ret=PTR_ERR(asm_child);asm_child=NULL;goto out;}
 adm_child=child(8,"qcom-q6adm","superman-speaker-tone-adm");if(IS_ERR(adm_child)){ret=PTR_ERR(adm_child);adm_child=NULL;goto out;}
 afe_child=child(4,"qcom-q6afe","superman-speaker-tone-afe");if(IS_ERR(afe_child)){ret=PTR_ERR(afe_child);afe_child=NULL;goto out;}
 backing=dma_alloc_coherent(&asm_child->dev,DMA_BYTES,&address,GFP_KERNEL);if(!backing){ret=-ENOMEM;goto out;}
 /* Zero-mean 1kHz triangle, -18dBFS peak; codec digital attenuation -12dB plus analogue gain code3. */
 for(i=0;i<DMA_BYTES/sizeof(s16);i++) {
  unsigned phase=i%48;
  ((s16 *)backing)[i]=(phase<24?(int)phase-12:36-(int)phase)*341;
 }
 client=q6asm_audio_client_alloc(&asm_child->dev,event,NULL,7,LEGACY_PCM_MODE);if(IS_ERR(client)){ret=PTR_ERR(client);client=NULL;goto out;}
 ret=q6asm_open_write(client,1,FORMAT_LINEAR_PCM,0,16,false);pr_info("SUPERMAN_SPEAKER_TONE open=%d\n",ret);if(ret)goto out;opened=true;
 ret=q6asm_media_format_block_multi_ch_pcm(client,1,48000,1,NULL,16);if(ret)goto out;
 map_uncertain=true;ret=q6asm_map_memory_regions(SNDRV_PCM_STREAM_PLAYBACK,client,address,PERIOD_BYTES,TONE_PERIODS);
 pr_info("SUPERMAN_SPEAKER_TONE map=%d\n",ret);if(ret<0)goto out;map_uncertain=false;mapped=true;
 afe_port=q6afe_port_get_from_id(&afe_child->dev,SLIMBUS_2_RX);if(IS_ERR(afe_port)){ret=PTR_ERR(afe_port);afe_port=NULL;goto out;}
 q6afe_slim_port_prepare(afe_port,&cfg);
 codec_dev=bus_find_device_by_name(&slimbus_bus,NULL,"217:e0:1:0");
 if(!codec_dev){ret=-ENODEV;goto out;}
 power_attempt=true;ret=superman_audio_board_power_enable(true);if(ret)goto out;
 lineout_attempt=true;ret=superman_wcd9302_lineout_start(codec_dev);
 pr_info("SUPERMAN_SPEAKER_TONE lineout_start=%d\n",ret);if(ret)goto out;
 sink_attempt=true;ret=superman_wcd9302_rx_start(codec_dev,16,139);
 pr_info("SUPERMAN_SPEAKER_TONE sink_start=%d\n",ret);if(ret)goto out;
 /* Connect the sink before the manager activates its isochronous channel. */
 port_attempt=true;ret=q6afe_port_start(afe_port);pr_info("SUPERMAN_SPEAKER_TONE afe_start=%d\n",ret);if(ret)goto out;
 copp=q6adm_open(&adm_child->dev,SLIMBUS_2_RX,ADM_PATH_PLAYBACK,48000,1,NULL_COPP_TOPOLOGY,LEGACY_PCM_MODE,16,0,0);
 if(IS_ERR_OR_NULL(copp)){ret=copp?PTR_ERR(copp):-ENODEV;copp=NULL;pr_info("SUPERMAN_SPEAKER_TONE adm_open=%d\n",ret);goto out;}
 route.copp_idx[0]=q6adm_get_copp_id(copp);
 ret=q6adm_matrix_map(&adm_child->dev,ADM_PATH_PLAYBACK,route,LEGACY_PCM_MODE);
 pr_info("SUPERMAN_SPEAKER_TONE matrix_map=%d\n",ret);if(ret<0)goto out;
 amp_attempt=true;ret=superman_speaker_amp_enable(true);
 pr_info("SUPERMAN_SPEAKER_TONE amp_enable=%d\n",ret);if(ret)goto out;
 ret=signal_status("before_run");if(ret)goto out;
 ret=q6asm_run(client,1,0,0,0);pr_info("SUPERMAN_SPEAKER_TONE run=%d\n",ret);if(ret)goto out;
 for(i=0;i<TONE_PERIODS;i++){ret=q6asm_write_async(client,1,PERIOD_BYTES,0,0,0);pr_info("SUPERMAN_SPEAKER_TONE write index=%u ret=%d\n",i,ret);if(ret<0)goto out;}
 msleep(100);ret=signal_status("streaming_100ms");if(ret)goto out;
 if(!wait_for_completion_timeout(&writes_done,msecs_to_jiffies(6500))){ret=-ETIMEDOUT;goto out;}
 if(atomic_read(&errors)||done_mask!=TONE_DONE_MASK){ret=-EIO;goto out;}
 ret=q6asm_cmd_nowait(client,1,CMD_EOS);if(ret<0)goto out;
 if(!wait_for_completion_timeout(&eos_done,msecs_to_jiffies(1500))){ret=-ETIMEDOUT;goto out;}
 pr_info("SUPERMAN_SPEAKER_TONE rendered EOS confirmed\n");
 ret=signal_status("rendered_EOS");if(ret)goto out;
 passed=true;ret=0;
out:
 released=cleanup();
 if(released){passed=false;retained=true;pinned=true;__module_get(THIS_MODULE);pr_err("SUPERMAN_SPEAKER_TONE cleanup=%d backing/resources retained; release callback available\n",released);mutex_unlock(&pipe_lock);return 0;}
 if(!ret&&passed)pr_info("SUPERMAN_SPEAKER_TONE eight actual DSP DMA completions and differential route restored; audible result needs user confirmation\n");
 mutex_unlock(&pipe_lock);return ret;
}
static void __exit stop(void) {}
module_init(start);module_exit(stop);
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("Superman bounded quiet speaker PCM tone and exact restoration");

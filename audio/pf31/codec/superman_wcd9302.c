// SPDX-License-Identifier: GPL-2.0-only
/* Real WCD9302 register access and ASoC component; no unimplemented DAI. */
#include <linux/module.h>
#include <linux/slimbus.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include "codec-sequence.h"
#include "lineout-program.h"
#include <sound/soc.h>
#include "slimbus.h"
#include "superman-codec.h"
struct codec {
 struct regmap *map;
 struct slim_device *sl, *ifd;
 struct mutex route_lock;
 u8 rx_port, saved_cfg, saved_multi;
 bool rx_owned, connect_attempt;
 bool lineout_owned;
 unsigned saved_count;
 struct codec_saved saved[CODEC_SAVED_MAX];
};
static int port_message(struct codec *c, bool connect, u8 channel)
{
 u8 bytes[]={c->rx_port,channel};
 struct slim_val_inf msg={.num_bytes=connect?2:1,.wbuf=bytes};
 DEFINE_SLIM_LDEST_TXN(txn, SLIM_MSG_MC_DISCONNECT_PORT, 5, c->sl->laddr, &msg);
 if(connect){txn.mc=SLIM_MSG_MC_CONNECT_SINK;txn.rl=6;}
 return slim_do_transfer(c->sl->ctrl,&txn);
}
static int restore_rx(struct codec *c)
{
 unsigned int cfg=0x40+c->rx_port-16, multi=0x180+4*(c->rx_port-16);
 u8 check;int ret;
 if(!c->rx_owned)return 0;
 if(c->connect_attempt){ret=port_message(c,false,0);if(ret)return ret;c->connect_attempt=false;}
 ret=slim_write(c->ifd,0x800+cfg,1,&c->saved_cfg);if(ret)return ret;
 ret=slim_write(c->ifd,0x800+multi,1,&c->saved_multi);if(ret)return ret;
 ret=slim_read(c->ifd,0x800+cfg,1,&check);if(ret)return ret;if(check!=c->saved_cfg)return -EIO;
 ret=slim_read(c->ifd,0x800+multi,1,&check);if(ret)return ret;if(check!=c->saved_multi)return -EIO;
 c->rx_owned=false;put_device(&c->ifd->dev);c->ifd=NULL;module_put(THIS_MODULE);
 return 0;
}
int superman_wcd9302_rx_stop(struct device *dev)
{
 struct codec *c=dev_get_drvdata(dev);int ret;
 if(!c || !dev->driver || dev->driver->owner!=THIS_MODULE)return -ENODEV;
 mutex_lock(&c->route_lock);ret=restore_rx(c);mutex_unlock(&c->route_lock);return ret;
}
EXPORT_SYMBOL_GPL(superman_wcd9302_rx_stop);
int superman_wcd9302_rx_start(struct device *dev,unsigned int port,unsigned int channel)
{
 struct codec *c=dev_get_drvdata(dev);struct slim_eaddr ea;u8 value,check;int ret,cleanup;
 if(!c || !dev->driver || dev->driver->owner!=THIS_MODULE || !superman_slim_pio_ready())return -ENODEV;
 if(port<16 || port>20 || channel>255)return -EINVAL;
 mutex_lock(&c->route_lock);
 if(c->rx_owned){ret=-EBUSY;goto out;}
 ea=c->sl->e_addr;ea.dev_index=0;c->ifd=slim_get_device(c->sl->ctrl,&ea);
 if(IS_ERR(c->ifd)){ret=PTR_ERR(c->ifd);c->ifd=NULL;goto out;}
 ret=slim_get_logical_addr(c->ifd);if(ret)goto put;
 ret=slim_read(c->ifd,0x840+port-16,1,&c->saved_cfg);if(ret)goto put;
 ret=slim_read(c->ifd,0x980+4*(port-16),1,&c->saved_multi);if(ret)goto put;
 /* Refuse an already enabled slave FIFO. No analogue codec writes here. */
 if(c->saved_cfg&1){ret=-EBUSY;goto put;}
 c->rx_port=port;c->rx_owned=true;__module_get(THIS_MODULE);
 value=BIT(port-16);ret=slim_write(c->ifd,0x980+4*(port-16),1,&value);if(ret)goto rollback;
 value=5;ret=slim_write(c->ifd,0x840+port-16,1,&value);if(ret)goto rollback;
 ret=slim_read(c->ifd,0x840+port-16,1,&check);if(ret)goto rollback;
 if(check!=5){ret=-EIO;goto rollback;}
 c->connect_attempt=true;ret=port_message(c,true,channel);if(ret)goto rollback;
 dev_info(dev,"RX slave port=%u channel=%u connected; analogue outputs unchanged\n",port,channel);
 goto out;
rollback:
 cleanup=restore_rx(c);if(cleanup)dev_err(dev,"RX rollback incomplete=%d; module retained for rx_stop retry\n",cleanup);
 goto out;
put:put_device(&c->ifd->dev);c->ifd=NULL;
out:mutex_unlock(&c->route_lock);return ret;
}
EXPORT_SYMBOL_GPL(superman_wcd9302_rx_start);
static int stop_lineout(struct codec *c)
{
 int ret,restore;
 if(!c->lineout_owned)return 0;
 ret=superman_speaker_amp_is_off();if(ret)return ret;
 /* Disable PAs before restoring their supplies and clocks. */
 ret=regmap_update_bits(c->map,0x1ca,3,0);if(ret)return ret;
 usleep_range(5000,5100);
 ret=regmap_update_bits(c->map,0x1cf,0xc0,0);if(ret)return ret;
 ret=regmap_update_bits(c->map,0x1d3,0xc0,0);if(ret)return ret;
 restore=codec_restore(c->map,c->saved,c->saved_count);if(restore)return restore;
 c->saved_count=0;c->lineout_owned=false;module_put(THIS_MODULE);return 0;
}
int superman_wcd9302_lineout_stop(struct device *dev)
{
 struct codec *c=dev_get_drvdata(dev);int ret;
 if(!c||!dev->driver||dev->driver->owner!=THIS_MODULE)return -ENODEV;
 mutex_lock(&c->route_lock);ret=stop_lineout(c);mutex_unlock(&c->route_lock);return ret;
}
EXPORT_SYMBOL_GPL(superman_wcd9302_lineout_stop);
int superman_wcd9302_lineout_start(struct device *dev)
{
 struct codec *c=dev_get_drvdata(dev);struct regulator *supply;unsigned value;int ret,cleanup,uv;
 if(!c||!dev->driver||dev->driver->owner!=THIS_MODULE||!superman_slim_pio_ready()||!superman_audio_board_power_ready())return -ENODEV;
 mutex_lock(&c->route_lock);
 if(c->lineout_owned){ret=-EBUSY;goto out;}
 ret=superman_speaker_amp_is_off();if(ret)goto out;
 supply=regulator_get(NULL,"s4");if(IS_ERR(supply)){ret=PTR_ERR(supply);goto out;}
 uv=regulator_get_voltage(supply);ret=regulator_is_enabled(supply);regulator_put(supply);
 /* Acquired Nokia S4 is 2.1V. Never change an external rail for this route. */
 if(ret!=1||uv!=2100000){ret=-ERANGE;goto out;}
 ret=regmap_read(c->map,0x1ca,&value);if(ret)goto out;if(value&3){ret=-EBUSY;goto out;}
 ret=regmap_read(c->map,0x1cf,&value);if(ret)goto out;if(value&0xc0){ret=-EBUSY;goto out;}
 ret=regmap_read(c->map,0x1d3,&value);if(ret)goto out;if(value&0xc0){ret=-EBUSY;goto out;}
 ret=regmap_read(c->map,0x192,&value);if(ret)goto out;if(value&1){ret=-EBUSY;goto out;}
 ret=regmap_read(c->map,0x181,&value);if(ret)goto out;if(value&0x80){ret=-EBUSY;goto out;}
 ret=regmap_read(c->map,0x80,&value);if(ret)goto out;if(value!=0){ret=-EBUSY;goto out;}
 ret=regmap_read(c->map,0x88,&value);if(ret)goto out;if(value!=4){ret=-EBUSY;goto out;}
 ret=codec_snapshot(c->map,lineout_program,ARRAY_SIZE(lineout_program),c->saved,&c->saved_count);if(ret){c->saved_count=0;goto out;}
 c->lineout_owned=true;__module_get(THIS_MODULE);
 ret=codec_apply(c->map,lineout_program,ARRAY_SIZE(lineout_program));
 if(ret){cleanup=stop_lineout(c);if(cleanup)dev_err(dev,"lineout rollback=%d; module/resources retained for stop retry\n",cleanup);goto out;}
 dev_info(dev,"RX3 differential LINEOUT1/2 configured, digital gain -12dB; external amplifier remains off\n");
out:mutex_unlock(&c->route_lock);return ret;
}
EXPORT_SYMBOL_GPL(superman_wcd9302_lineout_start);
static int reg_read(void *context,unsigned int reg,unsigned int *value)
{
 struct slim_device *sl=context;u8 byte;int ret;
 ret=slim_read(sl,0x800+reg,1,&byte);
 if(!ret)*value=byte;
 return ret;
}
static int reg_write(void *context,unsigned int reg,unsigned int value)
{
 struct slim_device *sl=context;u8 byte=value;
 if(reg>=4&&reg<=8)return -EACCES;
 return slim_write(sl,0x800+reg,1,&byte);
}
static const struct regmap_bus codec_bus={.reg_read=reg_read,.reg_write=reg_write};
static const struct regmap_config codec_config={.reg_bits=16,.val_bits=8,.max_register=0x3ff,.cache_type=REGCACHE_NONE};
static int identity(struct codec *c,u8 *id,unsigned int *version)
{
 static const u8 expected[]={1,0,3,1};int ret;
 ret=regmap_bulk_read(c->map,4,id,4);if(ret)return ret;
 ret=regmap_read(c->map,8,version);if(ret)return ret;
 return memcmp(id,expected,4)||*version!=0x21?-ENODEV:0;
}
static ssize_t chip_id_show(struct device *dev,struct device_attribute *attr,char *buf)
{
 struct codec *c=dev_get_drvdata(dev);u8 id[4];unsigned int version;int ret;
 ret=identity(c,id,&version);if(ret)return ret;
 return sysfs_emit(buf,"major=%02x%02x minor=%02x%02x version=%02x\n",id[3],id[2],id[1],id[0],version);
}
static DEVICE_ATTR_RO(chip_id);
static const struct snd_soc_component_driver component={.name="superman-wcd9302"};
static int codec_probe(struct slim_device *sl)
{
 struct codec *c;u8 id[4];unsigned int version;int ret;
 if(!of_machine_is_compatible("nokia,superman")||!of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 ret=slim_get_logical_addr(sl);if(ret)return ret;
 c=devm_kzalloc(&sl->dev,sizeof(*c),GFP_KERNEL);if(!c)return -ENOMEM;
 c->sl=sl;mutex_init(&c->route_lock);
 /* Context is the stable slim_device, never a pointer to a stack pointer. */
 c->map=devm_regmap_init(&sl->dev,&codec_bus,sl,&codec_config);if(IS_ERR(c->map))return PTR_ERR(c->map);
 slim_set_devicedata(sl,c);
 ret=identity(c,id,&version);if(ret)return dev_err_probe(&sl->dev,ret,"WCD9302 identity mismatch/read failure\n");
 ret=devm_snd_soc_register_component(&sl->dev,&component,NULL,0);if(ret)return ret;
 ret=device_create_file(&sl->dev,&dev_attr_chip_id);if(ret)return ret;
 dev_info(&sl->dev,"WCD9302 regmap/ASoC component ready, ID=0103:0001 version=21; no PCM routes\n");
 return 0;
}
static void codec_remove(struct slim_device *sl) {device_remove_file(&sl->dev,&dev_attr_chip_id);}
static const struct slim_device_id ids[]={{.manf_id=0x217,.prod_code=0xe0,.dev_index=1,.instance=0},{}};
MODULE_DEVICE_TABLE(slim,ids);
static struct slim_driver wcd9302_driver={.probe=codec_probe,.remove=codec_remove,.id_table=ids,.driver={.name="superman-wcd9302",.suppress_bind_attrs=true}};
module_slim_driver(wcd9302_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Lumia 730 WCD9302 hardware-backed regmap and ASoC component");

// SPDX-License-Identifier: GPL-2.0-only
/* PM8226 BMS1 capacity; no charger/limit/thermal/counter-reset operations. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/iio/iio.h>
#include <linux/iio/consumer.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <dt-bindings/iio/qcom,spmi-vadc.h>
#include "superman-iadc-current.h"
#include "superman-bms-math.h"
static struct platform_device *adc,*charger;
static struct power_supply *battery;
static struct iio_channel *vbat;
static struct regmap *map;
static DEFINE_MUTEX(gauge_lock);
static bool attribute_created,latch_retained,anchored,valid;
static unsigned int anchor_ocv;
static s64 anchor_cc,last_update;
static int anchor_pc,capacity=-1,last_error;
module_param(latch_retained,bool,0444);
module_param(capacity,int,0444);
module_param(last_error,int,0444);
static int release_locked(void)
{
 unsigned value;int ret=regmap_update_bits(map,0x4042,1,0);
 if(!ret)ret=regmap_read(map,0x4042,&value);
 if(!ret&&(value&1))ret=-EIO;
 return ret;
}
static int bms_read(unsigned *ocv,s64 *cc)
{
 unsigned ctl,check;u8 data[32];u64 raw=0;int ret,cleanup,i;
 if(latch_retained)return -EBUSY;
 device_lock(&charger->dev);
 if(!charger->dev.driver||strcmp(charger->dev.driver->name,"qcom-smbb")){ret=-ENODEV;goto out;}
 ret=regmap_read(map,0x4042,&ctl);if(ret)goto out;
 if(ctl&1){ret=-EBUSY;goto out;}
 ret=regmap_update_bits(map,0x4042,1,1);
 if(!ret){ret=regmap_read(map,0x4042,&check);if(!ret&&!(check&1))ret=-EIO;}
 if(!ret)ret=regmap_bulk_read(map,0x4080,data,sizeof(data));
 cleanup=release_locked();
 if(cleanup){latch_retained=true;__module_get(THIS_MODULE);ret=cleanup;}
 if(!ret){*ocv=data[16]|data[17]<<8;for(i=0;i<5;i++)raw|=(u64)data[10+i]<<(i*8);*cc=gauge_signed36(raw);}
 out:device_unlock(&charger->dev);return ret;
}
static int read_reference(unsigned channel,int *value)
{
 struct iio_channel ref=*vbat;unsigned i;int ret;
 for(i=0;i<ref.indio_dev->num_channels;i++)if(ref.indio_dev->channels[i].channel==channel) {
  ref.channel=&ref.indio_dev->channels[i];ret=iio_read_channel_raw(&ref,value);return ret<0?ret:0;
 }
 return -ENODEV;
}
static int update_capacity(void)
{
 struct superman_iadc_calibration cal;union power_supply_propval val;
 s64 cc,consumed,delta;unsigned ocv;int ret,low,high,uv,pc,result;
 ret=bms_read(&ocv,&cc);if(ret)return ret;
 ret=power_supply_get_property(battery,POWER_SUPPLY_PROP_PRESENT,&val);if(ret)return ret;
 if(!val.intval)return -ENODEV;
 ret=superman_iadc_get_calibration(&adc->dev,&cal);if(ret)return ret;
 if(!anchored||ocv!=anchor_ocv) {
  ret=read_reference(VADC_REF_625MV,&low);if(ret)return ret;
  ret=read_reference(VADC_REF_1250MV,&high);if(ret)return ret;
  ret=gauge_ocv_uv(ocv,low,high,&uv);if(ret)return ret;
  ret=power_supply_get_property(battery,POWER_SUPPLY_PROP_TEMP,&val);if(ret)return ret;
  ret=gauge_ocv_pc(uv,val.intval,&pc);if(ret)return ret;
  /* PON OCV uses accumulated boot CC; later OCV anchors retain their current CC origin. */
  anchor_cc=anchored?cc:0;anchor_ocv=ocv;anchor_pc=pc;anchored=true;
 }
 delta=gauge_signed36((u64)(cc-anchor_cc));
 ret=gauge_charge_uah(delta,cal.gain,cal.offset,cal.rsense_uohm,&consumed);if(ret)return ret;
 result=anchor_pc-div_s64(consumed*100000,gauge_nominal_fcc_uah);
 result=clamp(result,0,100000);capacity=(result+500)/1000;valid=true;
 last_update=ktime_to_ms(ktime_get_boottime());return 0;
}
static void gauge_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(refresh,gauge_work);
static void gauge_work(struct work_struct *work)
{
 int previous;bool changed;(void)work;
 mutex_lock(&gauge_lock);previous=capacity;last_error=update_capacity();changed=!last_error&&previous!=capacity;mutex_unlock(&gauge_lock);
 if(changed)power_supply_changed(battery);
 if(last_error)pr_warn_ratelimited("SUPERMAN_BMS refresh error=%d retained=%d\n",last_error,latch_retained);
 schedule_delayed_work(&refresh,msecs_to_jiffies(5000));
}
static ssize_t capacity_show(struct device *dev,struct device_attribute *attr,char *buf)
{
 int value;(void)attr;
 if(!battery||dev!=&battery->dev)return -ENODEV;
 mutex_lock(&gauge_lock);
 value=valid&&ktime_to_ms(ktime_get_boottime())-last_update<=60000?capacity:-EAGAIN;
 mutex_unlock(&gauge_lock);return value<0?value:sysfs_emit(buf,"%d\n",value);
}
static DEVICE_ATTR_RO(capacity);
static int release_set(const char *text,const struct kernel_param *param)
{
 bool yes;int ret;(void)param;
 ret=kstrtobool(text,&yes);if(ret||!yes)return -EINVAL;
 mutex_lock(&gauge_lock);
 if(!latch_retained){ret=-EINVAL;goto out;}
 ret=release_locked();if(!ret){latch_retained=false;module_put(THIS_MODULE);}
 out:mutex_unlock(&gauge_lock);return ret;
}
static const struct kernel_param_ops release_ops={.set=release_set};
module_param_cb(release,&release_ops,NULL,0200);
static void cleanup(void)
{
 if(attribute_created){device_remove_file(&battery->dev,&dev_attr_capacity);attribute_created=false;}
 if(vbat){iio_channel_release(vbat);vbat=NULL;}
 if(charger){put_device(&charger->dev);charger=NULL;}
 if(adc){put_device(&adc->dev);adc=NULL;}
 if(battery){power_supply_put(battery);battery=NULL;}
}
static struct platform_device *find_device(const char *compatible,unsigned expected)
{
 struct device_node *node;struct platform_device *device;unsigned base;
 node=of_find_compatible_node(NULL,NULL,compatible);if(!node)return NULL;
 if(!of_device_is_available(node)||!node->parent||!of_device_is_compatible(node->parent,"qcom,pm8226")||of_property_read_u32(node,"reg",&base)||base!=expected){of_node_put(node);return NULL;}
 device=of_find_device_by_node(node);of_node_put(node);return device;
}
static int __init gauge_init(void)
{
 unsigned type,subtype;int ret;
 if(!of_machine_is_compatible("nokia,superman")||!of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 adc=find_device("qcom,pm8226-iadc",0x3600);charger=find_device("qcom,pm8226-charger",0x1000);
 if(!adc||!charger){ret=-ENODEV;goto fail;}
 battery=power_supply_get_by_name("smbb-bif");if(!battery){ret=-ENODEV;goto fail;}
 if(!battery->desc||battery->desc->type!=POWER_SUPPLY_TYPE_BATTERY||battery->dev.parent!=&charger->dev){ret=-ENODEV;goto fail;}
 map=dev_get_regmap(charger->dev.parent,NULL);if(!map){ret=-ENODEV;goto fail;}
 ret=regmap_read(map,0x4004,&type);if(ret)goto fail;
 ret=regmap_read(map,0x4005,&subtype);if(ret)goto fail;
 if(type!=0x0d||subtype!=1){ret=-ENODEV;goto fail;}
 vbat=iio_channel_get(&charger->dev,"vbat");if(IS_ERR(vbat)){ret=PTR_ERR(vbat);vbat=NULL;goto fail;}
 ret=update_capacity();if(ret){if(latch_retained){last_error=ret;return 0;}goto fail;}
 ret=device_create_file(&battery->dev,&dev_attr_capacity);if(ret)goto fail;
 attribute_created=true;schedule_delayed_work(&refresh,msecs_to_jiffies(5000));
 pr_info("SUPERMAN_BMS ready capacity=%d stock_profile=BV-T5A measured_CC=1 no_charge_policy=1\n",capacity);return 0;
 fail:cleanup();return ret;
}
static void __exit gauge_exit(void)
{
 cancel_delayed_work_sync(&refresh);cleanup();
}
module_init(gauge_init);
module_exit(gauge_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Lumia730 calibrated BMS1 nominal-profile capacity; charger protections unchanged");

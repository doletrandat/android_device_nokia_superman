// SPDX-License-Identifier: GPL-2.0-only
/* Test the actual DSP SLIMbus AFE port; no analogue codec route or gain writes. */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/apr.h>
#include <linux/of.h>
#include <dt-bindings/sound/qcom,q6afe.h>
#include "q6afe.h"
extern const struct bus_type aprbus;
static struct platform_device *child;
static int __init afe_start(void)
{
 struct device *dev;struct q6afe_port *port;int ret,stopped;
 struct q6afe_slim_cfg cfg={.sample_rate=48000,.bit_width=16,.data_format=0,.num_channels=2,.ch_mapping={144,145}};
 if(!of_machine_is_compatible("nokia,superman")||!of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 dev=bus_find_device_by_name(&aprbus,NULL,"aprsvc::4:4");if(!dev)return -ENODEV;
 if(!dev->driver||strcmp(dev->driver->name,"qcom-q6afe")){put_device(dev);return -ENODEV;}
 child=platform_device_alloc("superman-afe-slim-test",PLATFORM_DEVID_NONE);if(!child){put_device(dev);return -ENOMEM;}
 child->dev.parent=dev;ret=platform_device_add(child);put_device(dev);
 if(ret){platform_device_put(child);return ret;}
 port=q6afe_port_get_from_id(&child->dev,SLIMBUS_0_RX);
 if(IS_ERR(port)){ret=PTR_ERR(port);goto device;}
 q6afe_slim_port_prepare(port,&cfg);
 ret=q6afe_port_start(port);
 pr_info("SUPERMAN_AFE_SLIM start port=4000 channels=144,145 format=48000/2/16 ret=%d\n",ret);
 stopped=q6afe_port_stop(port);
 pr_info("SUPERMAN_AFE_SLIM stop port=4000 ret=%d\n",stopped);
 q6afe_port_put(port);
 if(!ret)ret=stopped;
 if(!ret){pr_info("SUPERMAN_AFE_SLIM start/stop PASS, no physical route\n");return 0;}
device:
 platform_device_unregister(child);child=NULL;return ret;
}
static void __exit afe_stop(void) {if(child)platform_device_unregister(child);}
module_init(afe_start);module_exit(afe_stop);
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("Superman DSP SLIMbus AFE configuration/start/stop check");

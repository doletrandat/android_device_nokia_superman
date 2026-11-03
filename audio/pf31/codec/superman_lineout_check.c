// SPDX-License-Identifier: GPL-2.0-only
/* Muted differential codec setup and exact restore; never enables amplifier. */
#include <linux/module.h>
#include <linux/device.h>
#include <linux/slimbus.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include "superman-codec.h"
extern const struct bus_type slimbus_bus;
static struct device *codec_dev;
static bool passed,retained,pinned,power_attempt;
static int result=-EAGAIN;
static DEFINE_MUTEX(guard);
module_param(passed,bool,0400);
module_param(retained,bool,0400);
module_param(result,int,0400);
static int release(void)
{
 int ret;
 ret=superman_speaker_amp_is_off();if(ret)return ret;
 if(codec_dev){ret=superman_wcd9302_lineout_stop(codec_dev);if(ret)return ret;put_device(codec_dev);codec_dev=NULL;}
 if(power_attempt){ret=superman_audio_board_power_enable(false);if(ret)return ret;power_attempt=false;}
 retained=false;return 0;
}
static int release_owned(const char *text,const struct kernel_param *kp)
{
 bool yes;int ret;(void)kp;ret=kstrtobool(text,&yes);if(ret||!yes)return -EINVAL;
 mutex_lock(&guard);ret=release();if(!ret&&pinned){pinned=false;module_put(THIS_MODULE);}mutex_unlock(&guard);return ret;
}
static const struct kernel_param_ops ops={.set=release_owned};
module_param_cb(release,&ops,NULL,0200);
static int __init start(void)
{
 int cleanup;
 if(!of_machine_is_compatible("nokia,superman")||!of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 mutex_lock(&guard);
 codec_dev=bus_find_device_by_name(&slimbus_bus,NULL,"217:e0:1:0");
 if(!codec_dev){mutex_unlock(&guard);return -ENODEV;}
 power_attempt=true;result=superman_audio_board_power_enable(true);
 if(!result)result=superman_wcd9302_lineout_start(codec_dev);
 if(!result)msleep(100);
 cleanup=release();
 if(cleanup){retained=true;pinned=true;__module_get(THIS_MODULE);}
 passed=!result&&!cleanup;
 pr_info("SUPERMAN_LINEOUT muted_setup=%d restore=%d passed=%d retained=%d amplifier_never_enabled=true\n",result,cleanup,passed,retained);
 mutex_unlock(&guard);return 0;
}
static void __exit stop(void) {}
module_init(start);module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Superman muted WCD9302 differential route and restoration check");

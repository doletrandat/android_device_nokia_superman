/* SPDX-License-Identifier: GPL-2.0-only */
/* Called under pipe_lock. Each direction holds one common codec lease. */
#include "common-tx-port.h"
static struct device *common_dev;
static unsigned common_users,common_blocked;
module_param(common_users,uint,0400);module_param(common_blocked,uint,0400);
static bool common_power_attempt,common_lineout_attempt;
static int common_get(unsigned owner,struct device **dev,bool *owned)
{
 int ret;*dev=NULL;*owned=false;
 if(common_blocked || (common_users&owner))return -EBUSY;
 if(common_users){common_users|=owner;*owned=true;*dev=common_dev;return 0;}
 /* A private diagnostic or an incomplete old owner must not be displaced. */
 if(superman_audio_board_power_ready())return -EBUSY;
 common_users=owner;*owned=true;__module_get(THIS_MODULE);
 common_dev=bus_find_device_by_name(&slimbus_bus,NULL,"217:e0:1:0");
 if(!common_dev)return -ENODEV;
 *dev=common_dev;
 common_power_attempt=true;ret=superman_audio_board_power_enable(true);if(ret)return ret;
 common_lineout_attempt=true;ret=superman_wcd9302_lineout_start(common_dev);if(ret)return ret;
 return common_tx_start();
}
static int common_put(unsigned owner)
{
 int ret;
 if(!(common_users&owner))return -EINVAL;
 if(common_users&~owner){common_users&=~owner;common_blocked&=~owner;return 0;}
 ret=common_tx_stop();if(ret)goto fail;
 if(common_lineout_attempt){ret=superman_wcd9302_lineout_stop(common_dev);if(ret)goto fail;common_lineout_attempt=false;}
 if(common_power_attempt){ret=superman_audio_board_power_enable(false);if(ret)goto fail;common_power_attempt=false;}
 if(common_dev){put_device(common_dev);common_dev=NULL;}
 common_users=common_blocked=0;module_put(THIS_MODULE);return 0;
fail:common_blocked|=owner;return ret;
}

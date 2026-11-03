/* SPDX-License-Identifier: GPL-2.0-only */
/* Configure TX1 before RX starts; retain its FIFO until the last codec lease. */
static struct device *common_tx_ifd;
static u8 common_tx_saved[3];
static bool common_tx_owned,common_tx_configured;
module_param(common_tx_configured,bool,0400);
static int common_tx_read(u16 reg,u8 *v)
{
 return slim_read(to_slim_device(common_tx_ifd),0x800+reg,1,v);
}
static int common_tx_write(u16 reg,u8 v)
{
 u8 check;int ret=slim_write(to_slim_device(common_tx_ifd),0x800+reg,1,&v);
 if(ret)return ret;
 ret=common_tx_read(reg,&check);return ret?:check==v?0:-EIO;
}
static int common_tx_start(void)
{
 int ret;
 if(common_tx_ifd)return -EBUSY;
 common_tx_ifd=bus_find_device_by_name(&slimbus_bus,NULL,"217:e0:0:0");
 if(!common_tx_ifd)return -ENODEV;
 ret=common_tx_read(0x050,&common_tx_saved[0]);if(ret)return ret;
 ret=common_tx_read(0x100,&common_tx_saved[1]);if(ret)return ret;
 ret=common_tx_read(0x101,&common_tx_saved[2]);if(ret)return ret;
 if(common_tx_saved[0]||common_tx_saved[1]||common_tx_saved[2])return -EBUSY;
 common_tx_owned=true;
 ret=common_tx_write(0x100,1);if(ret)return ret;
 ret=common_tx_write(0x101,0);if(ret)return ret;
 ret=common_tx_write(0x050,5);if(ret)return ret;
 common_tx_configured=true;return 0;
}
static int common_tx_stop(void)
{
 int ret;
 if(!common_tx_ifd)return 0;
 if(common_tx_owned){
  ret=common_tx_write(0x050,common_tx_saved[0]);if(ret)return ret;
  ret=common_tx_write(0x100,common_tx_saved[1]);if(ret)return ret;
  ret=common_tx_write(0x101,common_tx_saved[2]);if(ret)return ret;
  common_tx_owned=false;
 }
 common_tx_configured=false;
 put_device(common_tx_ifd);common_tx_ifd=NULL;return 0;
}

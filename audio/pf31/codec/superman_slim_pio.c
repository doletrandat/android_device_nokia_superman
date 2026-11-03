// SPDX-License-Identifier: GPL-2.0-only
/* Lumia 730 NGD1 v1.1 register-mode control. No BAM/DMA/IRQ programming. */
#include <linux/module.h>
#include "superman-codec.h"
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/soc/qcom/qmi.h>
#include "slimbus.h"
#include "slim-qmi-contract.h"
#define NAME "superman-slim-pio"
#define ERR_BITS 0x2e000000U
struct pio_ctrl {
 struct slim_controller ctrl;
 struct device *dev;
 void __iomem *base;
 struct mutex io_lock;
 struct qmi_handle qmi;
 struct sockaddr_qrtr peer;
 struct completion server;
 bool qmi_live, power, enabled, registered;
 bool online;
 u8 address_tid;
};
static struct platform_device *pdev;
static bool ready;

static int new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
 struct pio_ctrl *p = container_of(qmi, struct pio_ctrl, qmi);
 if (service->service != 0x301 || service->version != 1 || service->instance)
  return -EINVAL;
 p->peer.sq_family = AF_QIPCRTR;
 p->peer.sq_node = service->node;
 p->peer.sq_port = service->port;
 WRITE_ONCE(p->online, true);
 complete(&p->server);
 return 0;
}
static void del_server(struct qmi_handle *qmi, struct qmi_service *service)
{
 struct pio_ctrl *p = container_of(qmi, struct pio_ctrl, qmi);
 if (service->node == p->peer.sq_node && service->port == p->peer.sq_port)
  WRITE_ONCE(p->online, false);
}
static const struct qmi_ops qmi_ops = {.new_server = new_server, .del_server = del_server};
static int qmi_request(struct pio_ctrl *p, bool select, bool active)
{
 struct slimbus_select_inst_req_msg_v01 sel = {.instance=0, .mode_valid=1, .mode=SLIMBUS_MODE_MASTER_V01};
 struct slimbus_power_req_msg_v01 power = {.pm_req=active?SLIMBUS_PM_ACTIVE_V01:SLIMBUS_PM_INACTIVE_V01};
 struct slimbus_power_resp_msg_v01 resp = {.resp={.result=U16_MAX,.error=U16_MAX}};
 struct qmi_txn txn;
 int ret;
 if (!READ_ONCE(p->online))
  return -ENETRESET;
 ret = qmi_txn_init(&p->qmi, &txn, slimbus_power_resp_msg_v01_ei, &resp);
 if (ret < 0)
  return ret;
 ret = qmi_send_request(&p->qmi, &p->peer, &txn, select?0x20:0x21, 14,
       select?slimbus_select_inst_req_msg_v01_ei:slimbus_power_req_msg_v01_ei,
       select?(void *)&sel:(void *)&power);
 if (ret) {
  qmi_txn_cancel(&txn);
  return ret;
 }
 ret = qmi_txn_wait(&txn, msecs_to_jiffies(1500));
 if (ret >= 0)
  ret = resp.resp.result || resp.resp.error ? -EREMOTEIO : 0;
 dev_info(p->dev, "QMI op=%02x active=%d result=%u error=%u ret=%d\n",
          select?0x20:0x21, active, resp.resp.result, resp.resp.error, ret);
 return ret;
}
static u32 rd(struct pio_ctrl *p, unsigned offset) {return readl(p->base+offset);}
static void wr(struct pio_ctrl *p, unsigned offset, u32 value) {writel(value,p->base+offset);}
static int receive(struct pio_ctrl *p, u8 *b, unsigned timeout)
{
 unsigned long end=jiffies+msecs_to_jiffies(timeout);
 do {
  u32 stat=rd(p,0x1014),w;
  unsigned len,i;
  if (stat&ERR_BITS) {wr(p,0x1018,stat&ERR_BITS);return -EIO;}
  if (stat&BIT(30)) {
   w=rd(p,0x1070);len=w&31;
   if (len<3) {wr(p,0x1018,BIT(30));return -EPROTO;}
   memcpy(b,&w,4);
   for(i=1;i<DIV_ROUND_UP(len,4);i++) {w=rd(p,0x1070+4*i);memcpy(b+4*i,&w,4);}
   wr(p,0x1018,BIT(30));dev_dbg(p->dev,"RX ack old=%08x now=%08x bytes=%*ph\n",stat,rd(p,0x1014),len,b);return len;
  }
  usleep_range(1000,1500);
 } while(time_before(jiffies,end));
 return -ETIMEDOUT;
}
static int transmit(struct pio_ctrl *p,const u8 *b,unsigned len)
{
 u32 words[8]={},stat;
 unsigned i;
 int ret;
 if(len<3||len>31||(b[0]&31)!=len)return -EINVAL;
 if(!READ_ONCE(p->online))return -ENETRESET;
 stat=rd(p,0x1014);
 if(stat&0xae000000U)return -EBUSY;
 dev_dbg(p->dev,"TX bytes=%*ph before=%08x\n",len,b,stat);
 memcpy(words,b,len);
 for(i=0;i<DIV_ROUND_UP(len,4);i++)wr(p,0x1030+4*i,words[i]);
 ret=readl_poll_timeout(p->base+0x1014,stat,stat&(ERR_BITS|BIT(31)),1000,700000);
 if(ret)return ret;
 wr(p,0x1018,stat&(ERR_BITS|BIT(31)));
 return stat&ERR_BITS?-EIO:0;
}
static bool master_capability(const u8 *b,int n)
{
 return n==7&&(b[0]>>5)==6&&b[1]==0&&b[2]==0xff&&b[3]==0xd9&&b[4]==0xc5&&b[5]==1&&b[6]==1;
}
static int get_laddr(struct slim_controller *ctrl,struct slim_eaddr *ea,u8 *laddr)
{
 struct pio_ctrl *p=container_of(ctrl,struct pio_ctrl,ctrl);
 u8 b[32],q[10]={0x4a,0x0d,0xff};
 int ret,n;unsigned reports=0;
 unsigned long end;
 if(ea->manf_id!=0x217||ea->prod_code!=0xe0||ea->instance||ea->dev_index>1)return -ENODEV;
 mutex_lock(&p->io_lock);
 q[3]=++p->address_tid;memcpy(q+4,ea,6);
 ret=transmit(p,q,sizeof(q));
 if(ret)goto out;
 end=jiffies+msecs_to_jiffies(900);
 do {
  n=receive(p,b,100);
  if(n>0)dev_info(p->dev,"address response tid=%u bytes=%*ph\n",q[3],n,b);
  if(n==-ETIMEDOUT)continue;
  if(n<0){ret=n;goto out;}
  if(master_capability(b,n)) {
   const u8 report[]={0xc7,1,0xff,0xd9,0xc5,1,1};
   if(++reports>2){ret=-EPROTO;goto out;}
   ret=transmit(p,report,sizeof(report));if(ret)goto out;
   msleep(20);
   q[3]=++p->address_tid;
   ret=transmit(p,q,sizeof(q));if(ret)goto out;
   continue;
  }
  if(n==11&&(b[0]>>5)==6&&b[1]==0x0e&&b[3]==q[3]) {
   if(memcmp(b+4,ea,6)||b[10]==0xff){ret=-ENXIO;goto out;}
   *laddr=b[10];ret=0;goto out;
  }
 } while(time_before(jiffies,end));
 ret=-ETIMEDOUT;
 dev_err(p->dev,"address timeout index=%u tid=%u cfg=%08x status=%08x irq=%08x\n",ea->dev_index,q[3],rd(p,0x1000),rd(p,0x1004),rd(p,0x1014));
out:
 mutex_unlock(&p->io_lock);return ret;
}
static int xfer_msg(struct slim_controller *ctrl,struct slim_msg_txn *t)
{
 struct pio_ctrl *p=container_of(ctrl,struct pio_ctrl,ctrl);
 struct slim_val_inf *msg=t->msg;
 u8 q[32]={},b[32];unsigned pos=3,len=t->rl-1;int ret,n;
 unsigned long end;
 bool read=t->mc==SLIM_MSG_MC_REQUEST_VALUE;
 bool port=t->mc==SLIM_MSG_MC_CONNECT_SOURCE || t->mc==SLIM_MSG_MC_CONNECT_SINK || t->mc==SLIM_MSG_MC_DISCONNECT_PORT;
 if(!t->mt && t->dt==SLIM_MSG_DEST_LOGICALADDR && msg && port) {
  bool disconnect=t->mc==SLIM_MSG_MC_DISCONNECT_PORT;
  if(!msg->wbuf || msg->num_bytes!=(disconnect?1:2))return -EINVAL;
  q[1]=disconnect?0x2e:(t->mc==SLIM_MSG_MC_CONNECT_SINK?0x2d:0x2c);
  len=disconnect?6:7;q[0]=0x40|len;q[2]=0xff;q[3]=t->la;
  q[4]=msg->wbuf[0];if(!disconnect)q[5]=msg->wbuf[1];
  mutex_lock(&p->io_lock);
  q[len-1]=++p->address_tid;
  ret=transmit(p,q,len);if(ret)goto out;
  end=jiffies+msecs_to_jiffies(900);
  do {
   n=receive(p,b,100);
   if(n==-ETIMEDOUT)continue;
   if(n<0){ret=n;goto out;}
   if(n>=4 && (b[0]>>5)==6 && b[1]==0x25 && b[3]==q[len-1]) {
    ret=n==5?((b[4]&0x20)?0:-EREMOTEIO):-EPROTO;
    dev_info(p->dev,"port op=%02x LA=%02x port=%u channel=%u ack=%02x ret=%d\n",q[1],t->la,q[4],disconnect?0:q[5],n>=5?b[4]:0,ret);
    goto out;
   }
  } while(time_before(jiffies,end));
  ret=-ETIMEDOUT;goto out;
 }
 if(t->mt||t->dt!=SLIM_MSG_DEST_LOGICALADDR||!msg||
    (!read&&t->mc!=SLIM_MSG_MC_CHANGE_VALUE)||!msg->num_bytes||msg->num_bytes>16||len>31)
  return -EOPNOTSUPP;
 q[0]=len;q[1]=t->mc;q[2]=t->la;
 if(read)q[pos++]=t->tid;
 q[pos++]=t->ec;q[pos++]=t->ec>>8;
 if(msg->wbuf){if(pos+msg->num_bytes>sizeof(q))return -EMSGSIZE;memcpy(q+pos,msg->wbuf,msg->num_bytes);pos+=msg->num_bytes;}
 if(pos!=len)return -EINVAL;
 mutex_lock(&p->io_lock);
 /* The core recycles its caller TID immediately; keep wire replies distinct. */
 if(read)q[3]=++p->address_tid;
 ret=transmit(p,q,len);
 if(ret||!read)goto out;
 end=jiffies+msecs_to_jiffies(900);
 do {
  n=receive(p,b,100);
  if(n==-ETIMEDOUT)continue;
  if(n<0){ret=n;goto out;}
  if(n>=4&&(b[0]>>5)==0&&b[1]==SLIM_MSG_MC_REPLY_VALUE&&b[3]==q[3]) {
   if(n!=msg->num_bytes+4){ret=-EPROTO;goto out;}
   slim_msg_response(ctrl,b+4,t->tid,msg->num_bytes);ret=0;goto out;
  }
 } while(time_before(jiffies,end));
 ret=-ETIMEDOUT;
out:
 mutex_unlock(&p->io_lock);return ret;
}
static int stream_unsupported(struct slim_stream_runtime *rt) {return -EOPNOTSUPP;}
static void cleanup(void *data)
{
 struct pio_ctrl *p=data;int ret;
 ready=false;
 if(p->registered) {slim_unregister_controller(&p->ctrl);pm_runtime_disable(p->dev);idr_destroy(&p->ctrl.tid_idr);ida_destroy(&p->ctrl.laddr_ida);}
 if(p->enabled) {wr(p,0x1000,0);wr(p,0x1018,rd(p,0x1014)&0xff000000U);dev_info(p->dev,"NGD restored cfg=%08x inten=%08x\n",rd(p,0x1000),rd(p,0x1010));}
 if(p->power) {ret=qmi_request(p,false,false);if(ret)dev_err(p->dev,"power release failed: %d\n",ret);}
 if(p->qmi_live)qmi_handle_release(&p->qmi);
}
static int probe(struct platform_device *dev)
{
 struct pio_ctrl *p;struct slim_device *s;struct slim_eaddr ea={.prod_code=0xe0,.manf_id=0x217};
 u8 b[32];int ret,n;unsigned i;
 if(!superman_wcd9302_reset_ready())return -ENODEV;
 p=devm_kzalloc(&dev->dev,sizeof(*p),GFP_KERNEL);if(!p)return -ENOMEM;
 p->dev=&dev->dev;mutex_init(&p->io_lock);init_completion(&p->server);
 p->base=devm_platform_ioremap_resource(dev,0);if(IS_ERR(p->base))return PTR_ERR(p->base);
 ret=devm_add_action_or_reset(&dev->dev,cleanup,p);if(ret)return ret;
 ret=qmi_handle_init(&p->qmi,64,&qmi_ops,NULL);if(ret)return ret;p->qmi_live=true;
 ret=qmi_add_lookup(&p->qmi,0x301,1,0);if(ret)return ret;
 if(!wait_for_completion_timeout(&p->server,msecs_to_jiffies(3000)))return -ETIMEDOUT;
 ret=qmi_request(p,true,false);if(ret)return ret;
 p->power=true;ret=qmi_request(p,false,true);if(ret)return ret;
 if(rd(p,0)!=0x01010000||((rd(p,0x3000)>>8)&3)!=1||rd(p,0x1000)||rd(p,0x1010)||(rd(p,0x1014)&0xfe000000U))return -EBUSY;
 dev_info(p->dev,"NGD initial status=%08x rx_msgq=%08x\n",rd(p,0x1004),rd(p,0x1008));
 p->enabled=true;wr(p,0x1000,1);msleep(30);
 /* LADDR may become valid before the queued capability message is read. */
 n=receive(p,b,(rd(p,0x1004)&2)?100:1200);
 if(n<0&&n!=-ETIMEDOUT)return n;
 if(n>0&&!master_capability(b,n))return -EPROTO;
 if(n==-ETIMEDOUT&&!(rd(p,0x1004)&2))return -ETIMEDOUT;
 if(n>0) {
  const u8 report[]={0xc7,1,0xff,0xd9,0xc5,1,1};
  ret=transmit(p,report,sizeof(report));if(ret)return ret;
  dev_info(p->dev,"host satellite capability reported in response to master\n");
 }
 p->ctrl.dev=p->dev;p->ctrl.get_laddr=get_laddr;p->ctrl.xfer_msg=xfer_msg;
 p->ctrl.enable_stream=stream_unsupported;p->ctrl.disable_stream=stream_unsupported;
 strscpy(p->ctrl.name,NAME,sizeof(p->ctrl.name));
 pm_runtime_set_active(p->dev);pm_runtime_enable(p->dev);
 ret=slim_register_controller(&p->ctrl);if(ret){pm_runtime_disable(p->dev);return ret;}p->registered=true;
 for(i=0;i<2;i++) {
  ea.dev_index=1-i;s=slim_get_device(&p->ctrl,&ea);if(IS_ERR(s))return PTR_ERR(s);
  ret=slim_get_logical_addr(s);if(ret)return ret;
  dev_info(p->dev,"native SLIM endpoint index=%u logical=%02x\n",ea.dev_index,s->laddr);
 }
 platform_set_drvdata(dev,p);ready=true;
 dev_info(p->dev,"kernel SLIM PIO ready; QMI power held, streaming disabled\n");
 return 0;
}
static struct platform_driver driver={.probe=probe,.driver={.name=NAME,.suppress_bind_attrs=true}};
static const struct resource resources[]={DEFINE_RES_MEM(0xfe12f000,0x4000)};
static int __init start(void)
{
 int ret;
 if(!of_machine_is_compatible("nokia,superman")||!of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 ret=platform_driver_register(&driver);if(ret)return ret;
 pdev=platform_device_register_simple(NAME,-1,resources,ARRAY_SIZE(resources));
 if(IS_ERR(pdev)){ret=PTR_ERR(pdev);goto unregister;}
 if(ready)return 0;
 platform_device_unregister(pdev);ret=-ENODEV;
unregister:platform_driver_unregister(&driver);return ret;
}
static void __exit stop(void) {platform_device_unregister(pdev);platform_driver_unregister(&driver);}
bool superman_slim_pio_ready(void) {return READ_ONCE(ready);}
EXPORT_SYMBOL_GPL(superman_slim_pio_ready);
module_init(start);module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Superman MSM8226 NGD1 1.1 kernel SLIMbus PIO control");

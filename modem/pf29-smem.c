// SPDX-License-Identifier: GPL-2.0-only
/* Read-only, exact-board SMEM RAM-table/error-log acquisition. No allocation
 * in SMEM, register writes, storage access or peripheral control. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/soc/qcom/smem.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/unaligned.h>
#define MAX_LOG 65536
static u8 *data;
static size_t length;
static ssize_t read_logs(struct file *f,char __user *buf,size_t n,loff_t *pos)
{
 return simple_read_from_buffer(buf,n,pos,data,length);
}
static const struct file_operations ops={.owner=THIS_MODULE,.read=read_logs,.llseek=default_llseek};
static struct miscdevice output={.minor=MISC_DYNAMIC_MINOR,.name="pf29-smem",.fops=&ops,.mode=0400};
static int __init start(void)
{
 /* Qualcomm SMEM IDs: DIAG_ERR_MESSAGE, USABLE_RAM_PARTITION_TABLE,
  * ERR_CRASH_LOG, SSR_REASON_MSS0. No arbitrary item selection. */
 const u32 items[]={6,402,408,421};unsigned int i;int ret;
 if(!of_machine_is_compatible("nokia,superman")||!of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 data=vzalloc(8+ARRAY_SIZE(items)*(12+MAX_LOG));if(!data)return -ENOMEM;
 memcpy(data,"PF29SMEM",8);length=8;
 for(i=0;i<ARRAY_SIZE(items);i++){
  size_t size=0;void *p=qcom_smem_get(QCOM_SMEM_HOST_ANY,items[i],&size);
  ret=IS_ERR(p)?PTR_ERR(p):size>MAX_LOG?-EOVERFLOW:0;
  put_unaligned_le32(items[i],data+length);
  put_unaligned_le32((u32)ret,data+length+4);
  put_unaligned_le32(ret?0:size,data+length+8);length+=12;
  if(!ret){memcpy(data+length,p,size);length+=size;}
 }
 ret=misc_register(&output);
 if(ret){memzero_explicit(data,length);vfree(data);data=NULL;}
 return ret;
}
static void __exit stop(void)
{
 misc_deregister(&output);memzero_explicit(data,length);vfree(data);
}
module_init(start);module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Superman allowlisted SMEM RAM table/error logs, read-only");

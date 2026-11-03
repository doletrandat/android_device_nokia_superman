// SPDX-License-Identifier: GPL-2.0-only
/* Normal secure-world verification/decryption of the exact stock encrypted MBA.
 * Decrypted bytes are exposed read-only to root; no modem start or storage writes.
 */
#include <linux/module.h>
#include <linux/firmware.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/of.h>
#include <linux/uaccess.h>
extern int qcom_scm_superman_ssd_available(void);
extern int qcom_scm_superman_mba_decrypt(const void *,size_t,void *,size_t *,u32 *);
static int available=-1,result=-EAGAIN;
static unsigned int secure_status=0xffffffff;
static bool decrypt;
static u8 *plain;
static size_t plain_len;
module_param(decrypt,bool,0400);
module_param(available,int,0400);
module_param(result,int,0400);
module_param(secure_status,uint,0400);
static ssize_t read_plain(struct file *f,char __user *buf,size_t size,loff_t *pos)
{
 if(result || !plain)return -ENODATA;
 return simple_read_from_buffer(buf,size,pos,plain,plain_len);
}
static const struct file_operations ops={.owner=THIS_MODULE,.read=read_plain,.llseek=default_llseek};
static struct miscdevice output={.minor=MISC_DYNAMIC_MINOR,.name="pf29-mba",.fops=&ops,.mode=0400};
static int __init start(void)
{
 const struct firmware *fw;struct device *dev;int ret;
 if(!of_machine_is_compatible("nokia,superman")||!of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 available=qcom_scm_superman_ssd_available();
 if(!decrypt)return 0;
 if(available!=3){result=available<0?available:-EOPNOTSUPP;return 0;}
 dev=root_device_register("pf29-mba-firmware");
 if(IS_ERR(dev)){result=PTR_ERR(dev);return 0;}
 ret=request_firmware_direct(&fw,"qcom/superman/qcdsp1v28626.mbn",dev);
 root_device_unregister(dev);
 if(ret){result=ret;return 0;}
 if(fw->size!=238232 || memcmp(fw->data,"\x7f" "ELF\x01\x01\x01",7)){result=-EINVAL;goto release;}
 plain_len=236384;plain=vzalloc(plain_len);
 if(!plain){result=-ENOMEM;goto release;}
 result=qcom_scm_superman_mba_decrypt(fw->data+84,fw->size-84,plain,&plain_len,&secure_status);
 if(result){memzero_explicit(plain,236384);vfree(plain);plain=NULL;goto release;}
 ret=misc_register(&output);
 if(ret){result=ret;memzero_explicit(plain,236384);vfree(plain);plain=NULL;}
release:release_firmware(fw);
 pr_info("PF29_MBA available=%d result=%d secure_status=%x bytes=%zu no_peripheral_boot=1\n",available,result,secure_status,result?0:plain_len);
 return 0;
}
static void __exit stop(void)
{
 if(plain){misc_deregister(&output);memzero_explicit(plain,plain_len);vfree(plain);}
}
module_init(start);module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Superman exact stock MBA normal SSD verification/decryption, no peripheral boot");

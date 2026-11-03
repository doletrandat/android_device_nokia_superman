// SPDX-License-Identifier: GPL-2.0-only
/* Preserve the exact stock RFS0 buffer before assigning it to Linux RMTFS.
 * Read-only snapshot of an existing no-map reservation; no peripheral control. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#define BASE 0x0fd80000
#define SIZE 0x00180000
static u8 *data;
static ssize_t read_snapshot(struct file *f,char __user *buf,size_t n,loff_t *pos)
{ return simple_read_from_buffer(buf,n,pos,data,SIZE); }
static const struct file_operations ops={.owner=THIS_MODULE,.read=read_snapshot,.llseek=default_llseek};
static struct miscdevice output={.minor=MISC_DYNAMIC_MINOR,.name="pf29-rfs-snapshot",.fops=&ops,.mode=0400};
static int __init start(void)
{
 struct device_node *node;struct resource res;void *p;int ret;
 if(!of_machine_is_compatible("nokia,superman")||!of_machine_is_compatible("qcom,msm8226"))return -ENODEV;
 node=of_find_node_by_path("/reserved-memory/diagnostic-quarantine@f500000");
 if(!node)return -ENODEV;
 ret=of_address_to_resource(node,0,&res);
 if(!ret&&!of_property_read_bool(node,"no-map"))ret=-EPERM;
 of_node_put(node);if(ret)return ret;
 if(res.start!=0x0f500000||resource_size(&res)!=0x00b00000||BASE<res.start||BASE+SIZE-1>res.end)return -EPERM;
 data=vzalloc(SIZE);if(!data)return -ENOMEM;
 p=memremap(BASE,SIZE,MEMREMAP_WC);
 if(IS_ERR_OR_NULL(p)){ret=p?PTR_ERR(p):-ENOMEM;goto free;}
 memcpy(data,p,SIZE);memunmap(p);
 ret=misc_register(&output);if(!ret)return 0;
free:memzero_explicit(data,SIZE);vfree(data);data=NULL;return ret;
}
static void __exit stop(void)
{ misc_deregister(&output);memzero_explicit(data,SIZE);vfree(data); }
module_init(start);module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Superman stock RFS0 reserved RAM preservation, read-only");

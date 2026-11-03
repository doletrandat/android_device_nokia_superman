// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include "q6core.h"
static int __init core_smoke_init(void)
{
 struct q6core_svc_api_info info;
 const int ids[] = {3, 4, 7, 8};
 int i, ret;
 if (!q6core_is_adsp_ready()) return -ENODEV;
 pr_info("SUPERMAN_Q6CORE ready=1\n");
 for(i=0;i<ARRAY_SIZE(ids);i++) {
  ret=q6core_get_svc_api_info(ids[i], &info);
  if(ret) {pr_err("SUPERMAN_Q6CORE service=%d error=%d\n",ids[i],ret);return ret;}
  pr_info("SUPERMAN_Q6CORE service=%u api=%#x branch=%#x\n",ids[i],info.api_version,info.api_branch_version);
 }
 return 0;
}
static void __exit core_smoke_exit(void) {}
module_init(core_smoke_init);module_exit(core_smoke_exit);
MODULE_LICENSE("GPL");MODULE_DESCRIPTION("Read-only Q6 core service query smoke test");

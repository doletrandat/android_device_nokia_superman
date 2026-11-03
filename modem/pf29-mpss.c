// SPDX-License-Identifier: GPL-2.0-only
/* Root-only acquisition of exact-stock, hash-verified MPSS plaintext.
 * No modem start or persistent writes. Normal secure-world decryption only.
 */
#include <linux/module.h>
#include <linux/firmware.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/of.h>
extern int qcom_scm_superman_mpss_decrypt(const void *, size_t, void *, u32 *);
static bool decrypt;
static int result = -EAGAIN;
static unsigned int secure_status = 0xffffffff;
static u8 *plain;
static const size_t image_size = 30984015;
module_param(decrypt, bool, 0400);
module_param(result, int, 0400);
module_param(secure_status, uint, 0400);
static ssize_t read_plain(struct file *f, char __user *buf, size_t size, loff_t *pos)
{
 if (result || !plain) return -ENODATA;
 return simple_read_from_buffer(buf, size, pos, plain, image_size);
}
static const struct file_operations ops = {
 .owner = THIS_MODULE, .read = read_plain, .llseek = default_llseek,
};
static struct miscdevice output = {
 .minor = MISC_DYNAMIC_MINOR, .name = "pf29-mpss", .fops = &ops, .mode = 0400,
};
static int __init start(void)
{
 const struct firmware *fw;
 struct device *dev;
 int ret;
 if (!of_machine_is_compatible("nokia,superman") ||
     !of_machine_is_compatible("qcom,msm8226")) return -ENODEV;
 if (!decrypt) return 0;
 dev = root_device_register("pf29-mpss-firmware");
 if (IS_ERR(dev)) return PTR_ERR(dev);
 ret = request_firmware_direct(&fw, "qcom/superman/qcdsp28626.mbn", dev);
 root_device_unregister(dev);
 if (ret) { result = ret; return 0; }
 if (fw->size != image_size) { result = -EINVAL; goto release; }
 plain = vzalloc(image_size);
 if (!plain) { result = -ENOMEM; goto release; }
 result = qcom_scm_superman_mpss_decrypt(fw->data, fw->size, plain, &secure_status);
 if (!result) result = misc_register(&output);
 if (result) { memzero_explicit(plain, image_size); vfree(plain); plain = NULL; }
release:
 release_firmware(fw);
 pr_info("PF29_MPSS result=%d secure_status=%x hash_verified=%d no_peripheral_boot=1\n",
         result, secure_status, !result);
 return 0;
}
static void __exit stop(void)
{
 if (plain) { misc_deregister(&output); memzero_explicit(plain, image_size); vfree(plain); }
}
module_init(start);
module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Superman exact stock MPSS normal SSD ELF-segment decryption");

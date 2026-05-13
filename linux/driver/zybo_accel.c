// SPDX-License-Identifier: GPL-2.0
/*
 * Zybo Z7 AXI-Lite accelerator control driver.
 *
 * This first production-quality milestone replaces ad-hoc devmem access for
 * the already-proven FPGA register block.  The hardware currently exposes:
 *
 *   0x00 VERSION - read-only build/version register
 *   0x04 SCRATCH - read/write AXI-Lite bring-up register
 *
 * The driver is intentionally structured so that later PT2 work can extend it
 * with the full control/status register map, DMA handling, interrupt handling,
 * and a richer user-space API without replacing the basic platform-driver
 * foundation built here.
 */

#include <linux/compat.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "zybo_accel_uapi.h"

#define ZYBO_ACCEL_DRIVER_NAME          "zybo_accel"

/* Current minimal FPGA register map. */
#define ZYBO_ACCEL_REG_VERSION          0x00U
#define ZYBO_ACCEL_REG_SCRATCH          0x04U
#define ZYBO_ACCEL_MIN_MMIO_SIZE        0x08U

/* Current verified hardware value from the first AXI-Lite milestone. */
#define ZYBO_ACCEL_VERSION_V0_1         0x00010000U

/**
 * struct zybo_accel_dev - per-device driver state
 * @dev:             Parent platform device.
 * @regs:            Kernel virtual base address of the AXI-Lite register map.
 * @regs_size:       Size, in bytes, of the MMIO resource from the device tree.
 * @lock:            Serializes current register access and future state changes.
 * @miscdev:         Character-device wrapper that creates /dev/zybo_accel0.
 * @cached_version:  VERSION register sampled during probe.
 */
struct zybo_accel_dev {
	struct device *dev;
	void __iomem *regs;
	resource_size_t regs_size;
	struct mutex lock;
	struct miscdevice miscdev;
	u32 cached_version;
};

static inline u32 zybo_accel_readl(struct zybo_accel_dev *accel, u32 offset)
{
	return readl(accel->regs + offset);
}

static inline void zybo_accel_writel(struct zybo_accel_dev *accel,
					      u32 offset, u32 value)
{
	writel(value, accel->regs + offset);
}

/**
 * zybo_accel_fill_info - populate the public device-information structure
 * @accel: Driver private data.
 * @info:  Structure returned to user space.
 */
static void zybo_accel_fill_info(struct zybo_accel_dev *accel,
					 struct zybo_accel_info *info)
{
	info->abi_version = ZYBO_ACCEL_ABI_VERSION;
	info->hardware_version = accel->cached_version;
	info->register_span = accel->regs_size > 0xffffffffULL ?
		0xffffffffU : (u32)accel->regs_size;
	info->reserved = 0;
}

/**
 * zybo_accel_open - attach a file descriptor to this device instance
 * @inode: VFS inode, unused.
 * @file:  File object opened by user space.
 *
 * misc_open() initially stores a pointer to struct miscdevice in
 * file->private_data.  Replace it with our driver-private structure so that
 * later file operations can access the device directly.
 *
 * Return: 0 on success.
 */
static int zybo_accel_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct zybo_accel_dev *accel;

	(void)inode;

	accel = container_of(miscdev, struct zybo_accel_dev, miscdev);
	file->private_data = accel;

	return 0;
}

/**
 * zybo_accel_release - release an open file descriptor
 * @inode: VFS inode, unused.
 * @file:  File object being released, unused.
 *
 * Return: 0 on success.
 */
static int zybo_accel_release(struct inode *inode, struct file *file)
{
	(void)inode;
	(void)file;

	return 0;
}

/**
 * zybo_accel_ioctl - dispatch user-space control requests
 * @file: File object opened on /dev/zybo_accel0.
 * @cmd:  IOCTL command number.
 * @arg:  User-space pointer encoded as an unsigned long.
 *
 * Return: 0 on success, or a negative errno value on failure.
 */
static long zybo_accel_ioctl(struct file *file, unsigned int cmd,
				     unsigned long arg)
{
	struct zybo_accel_dev *accel = file->private_data;
	void __user *argp = (void __user *)arg;
	struct zybo_accel_info info;
	struct zybo_accel_scratch scratch;
	long ret = 0;

	if (_IOC_TYPE(cmd) != ZYBO_ACCEL_IOCTL_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case ZYBO_ACCEL_IOCTL_GET_INFO:
		zybo_accel_fill_info(accel, &info);
		if (copy_to_user(argp, &info, sizeof(info)))
			ret = -EFAULT;
		break;

	case ZYBO_ACCEL_IOCTL_SCRATCH_READ:
		mutex_lock(&accel->lock);
		scratch.value = zybo_accel_readl(accel, ZYBO_ACCEL_REG_SCRATCH);
		mutex_unlock(&accel->lock);

		if (copy_to_user(argp, &scratch, sizeof(scratch)))
			ret = -EFAULT;
		break;

	case ZYBO_ACCEL_IOCTL_SCRATCH_WRITE:
		if (copy_from_user(&scratch, argp, sizeof(scratch)))
			return -EFAULT;

		mutex_lock(&accel->lock);
		zybo_accel_writel(accel, ZYBO_ACCEL_REG_SCRATCH, scratch.value);
		mutex_unlock(&accel->lock);
		break;

	default:
		ret = -ENOTTY;
		break;
	}

	return ret;
}

#ifdef CONFIG_COMPAT
static long zybo_accel_compat_ioctl(struct file *file, unsigned int cmd,
					    unsigned long arg)
{
	return zybo_accel_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif

static const struct file_operations zybo_accel_fops = {
	.owner = THIS_MODULE,
	.open = zybo_accel_open,
	.release = zybo_accel_release,
	.unlocked_ioctl = zybo_accel_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = zybo_accel_compat_ioctl,
#endif
};

/**
 * zybo_accel_validate_mmio - validate the discovered register resource
 * @pdev:  Platform device being probed.
 * @accel: Driver-private state with mapped registers.
 *
 * Return: 0 on success, or a negative errno value if the resource is too
 * small for the registers used by this driver version.
 */
static int zybo_accel_validate_mmio(struct platform_device *pdev,
					    struct zybo_accel_dev *accel)
{
	if (accel->regs_size < ZYBO_ACCEL_MIN_MMIO_SIZE) {
		dev_err(&pdev->dev,
			"MMIO span %llu bytes is smaller than required minimum 0x%x bytes\n",
			(unsigned long long)accel->regs_size,
			ZYBO_ACCEL_MIN_MMIO_SIZE);
		return -EINVAL;
	}

	return 0;
}

/**
 * zybo_accel_probe - bind the driver to one device-tree-described device
 * @pdev: Platform device constructed from the device tree.
 *
 * Return: 0 on success, or a negative errno value on failure.
 */
static int zybo_accel_probe(struct platform_device *pdev)
{
	struct zybo_accel_dev *accel;
	struct resource *res;
	int ret;

	accel = devm_kzalloc(&pdev->dev, sizeof(*accel), GFP_KERNEL);
	if (!accel)
		return -ENOMEM;

	accel->dev = &pdev->dev;
	mutex_init(&accel->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return dev_err_probe(&pdev->dev, -ENODEV,
					     "missing AXI-Lite MMIO resource\n");

	accel->regs_size = resource_size(res);
	accel->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(accel->regs))
		return PTR_ERR(accel->regs);

	ret = zybo_accel_validate_mmio(pdev, accel);
	if (ret)
		return ret;

	accel->cached_version = zybo_accel_readl(accel, ZYBO_ACCEL_REG_VERSION);
	if (accel->cached_version != ZYBO_ACCEL_VERSION_V0_1)
		dev_warn(&pdev->dev,
			 "unexpected VERSION value 0x%08x; expected 0x%08x\n",
			 accel->cached_version, ZYBO_ACCEL_VERSION_V0_1);

	accel->miscdev.minor = MISC_DYNAMIC_MINOR;
	accel->miscdev.name = ZYBO_ACCEL_DEVICE_NAME;
	accel->miscdev.fops = &zybo_accel_fops;
	accel->miscdev.parent = &pdev->dev;
	accel->miscdev.mode = 0600;

	ret = misc_register(&accel->miscdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
					     "failed to register /dev/%s\n",
					     ZYBO_ACCEL_DEVICE_NAME);

	platform_set_drvdata(pdev, accel);

	dev_info(&pdev->dev,
		 "/dev/%s ready: VERSION=0x%08x, MMIO span=%llu bytes\n",
		 ZYBO_ACCEL_DEVICE_NAME, accel->cached_version,
		 (unsigned long long)accel->regs_size);

	return 0;
}

/**
 * zybo_accel_remove - unbind the driver from the platform device
 * @pdev: Platform device being removed.
 */
static void zybo_accel_remove(struct platform_device *pdev)
{
	struct zybo_accel_dev *accel = platform_get_drvdata(pdev);

	misc_deregister(&accel->miscdev);
	dev_info(&pdev->dev, "/dev/%s removed\n", ZYBO_ACCEL_DEVICE_NAME);
}

static const struct of_device_id zybo_accel_of_match[] = {
	/* Current compatible generated by the first Vivado/PetaLinux milestone. */
	{ .compatible = "xlnx,zybo-accel-ctrl-1.0" },

	/* Reserved for the planned stable PT2 platform compatible string. */
	{ .compatible = "digilent,zybo-accel-pt2-1.0" },
	{ }
};
MODULE_DEVICE_TABLE(of, zybo_accel_of_match);

static struct platform_driver zybo_accel_driver = {
	.probe = zybo_accel_probe,
	.remove = zybo_accel_remove,
	.driver = {
		.name = ZYBO_ACCEL_DRIVER_NAME,
		.of_match_table = zybo_accel_of_match,
	},
};
module_platform_driver(zybo_accel_driver);

MODULE_AUTHOR("Shahin");
MODULE_DESCRIPTION("Zybo Z7 AXI-Lite accelerator control driver");
MODULE_LICENSE("GPL");

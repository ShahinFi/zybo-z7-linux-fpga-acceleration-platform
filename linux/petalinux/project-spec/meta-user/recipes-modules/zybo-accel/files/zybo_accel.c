// SPDX-License-Identifier: GPL-2.0
/*
 * Zybo Z7 Linux-controlled FPGA platform driver.
 *
 * The current hardware has two verified interfaces:
 *
 *   - A small AXI-Lite control block with VERSION and SCRATCH registers.
 *   - AXI DMA MM2S and S2MM channels whose stream ports are looped back in PL.
 *
 * The loopback is only the current FPGA-side validation topology.  The driver
 * implements the permanent Linux-side transaction model: user space submits an
 * input buffer, the kernel owns DMA-safe buffers and completion handling, and
 * the completed output buffer is returned to user space through one blocking
 * ioctl.  Later stream-processing hardware can replace the loopback without
 * replacing this software transaction path.
 */

#include <linux/compat.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "zybo_accel_uapi.h"

#define ZYBO_ACCEL_DRIVER_NAME          "zybo_accel"

/* Current minimal FPGA register map. */
#define ZYBO_ACCEL_REG_VERSION          0x00U
#define ZYBO_ACCEL_REG_SCRATCH          0x04U
#define ZYBO_ACCEL_MIN_MMIO_SIZE        0x08U

/* Current verified hardware value from the first AXI-Lite milestone. */
#define ZYBO_ACCEL_VERSION_V0_1         0x00010000U

/* Current DMA transaction policy for the simple-mode loopback milestone. */
#define ZYBO_ACCEL_DMA_MAX_BYTES        (1024U * 1024U)
#define ZYBO_ACCEL_DMA_ALIGNMENT_BYTES  4U
#define ZYBO_ACCEL_DMA_TIMEOUT_DEFAULT  1000U
#define ZYBO_ACCEL_DMA_TIMEOUT_MAX      60000U

/**
 * struct zybo_accel_dev - per-device driver state
 * @dev:                Parent platform device.
 * @regs:               Kernel virtual base address of the AXI-Lite register map.
 * @regs_size:          Size, in bytes, of the MMIO resource from the device tree.
 * @lock:               Serializes all user-visible register and DMA operations.
 * @miscdev:            Character-device wrapper that creates /dev/zybo_accel0.
 * @cached_version:     VERSION register sampled during probe.
 * @tx_chan:            DMAEngine channel that feeds MM2S data into the stream path.
 * @rx_chan:            DMAEngine channel that receives S2MM data from the stream path.
 * @tx_dma_dev:         DMA mapping device associated with @tx_chan.
 * @rx_dma_dev:         DMA mapping device associated with @rx_chan.
 * @tx_cpu_buf:         CPU-visible coherent input staging buffer.
 * @rx_cpu_buf:         CPU-visible coherent output staging buffer.
 * @tx_dma_addr:        DMA-visible address of @tx_cpu_buf.
 * @rx_dma_addr:        DMA-visible address of @rx_cpu_buf.
 * @dma_buf_size:       Size of each coherent staging buffer.
 * @tx_done:            Completion signaled when MM2S finishes.
 * @rx_done:            Completion signaled when S2MM finishes.
 * @stats:              Accumulated user-visible DMA transaction counters.
 */
struct zybo_accel_dev {
	struct device *dev;
	void __iomem *regs;
	resource_size_t regs_size;
	struct mutex lock;
	struct miscdevice miscdev;
	u32 cached_version;

	struct dma_chan *tx_chan;
	struct dma_chan *rx_chan;
	struct device *tx_dma_dev;
	struct device *rx_dma_dev;
	void *tx_cpu_buf;
	void *rx_cpu_buf;
	dma_addr_t tx_dma_addr;
	dma_addr_t rx_dma_addr;
	size_t dma_buf_size;
	struct completion tx_done;
	struct completion rx_done;
	struct zybo_accel_stats stats;
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
 * zybo_accel_fill_dma_caps - populate the public DMA-capabilities structure
 * @caps: Structure returned to user space.
 */
static void zybo_accel_fill_dma_caps(struct zybo_accel_dma_caps *caps)
{
	memset(caps, 0, sizeof(*caps));
	caps->max_transfer_bytes = ZYBO_ACCEL_DMA_MAX_BYTES;
	caps->transfer_alignment_bytes = ZYBO_ACCEL_DMA_ALIGNMENT_BYTES;
	caps->default_timeout_ms = ZYBO_ACCEL_DMA_TIMEOUT_DEFAULT;
	caps->max_timeout_ms = ZYBO_ACCEL_DMA_TIMEOUT_MAX;
	caps->flags = ZYBO_ACCEL_DMA_CAP_BLOCKING_SUBMIT |
		      ZYBO_ACCEL_DMA_CAP_DRIVER_STAGING_BUFS;
}

/**
 * zybo_accel_dma_callback - wake the blocking submit path after DMA completion
 * @done: Completion object associated with the finished DMA descriptor.
 */
static void zybo_accel_dma_callback(void *done)
{
	complete(done);
}

/**
 * zybo_accel_wait_until - wait for one completion within a shared deadline
 * @done:     DMA completion object to wait for.
 * @deadline: Absolute jiffies deadline shared by the full transaction.
 *
 * Return: 0 on completion, or -ETIMEDOUT when the deadline expires first.
 */
static int zybo_accel_wait_until(struct completion *done,
					 unsigned long deadline)
{
	unsigned long remaining;

	if (time_after_eq(jiffies, deadline))
		return -ETIMEDOUT;

	remaining = deadline - jiffies;
	if (!wait_for_completion_timeout(done, remaining))
		return -ETIMEDOUT;

	return 0;
}

/**
 * zybo_accel_terminate_dma - stop both DMA directions after a failed transfer
 * @accel: Driver private data.
 *
 * The receive channel is stopped first because the stream sink is armed before
 * the stream source during normal transfer setup.
 */
static void zybo_accel_terminate_dma(struct zybo_accel_dev *accel)
{
	dmaengine_terminate_sync(accel->rx_chan);
	dmaengine_terminate_sync(accel->tx_chan);
}

/**
 * zybo_accel_validate_transfer - reject malformed user transaction requests
 * @transfer: Transaction request copied from user space.
 *
 * Return: 0 on success, or a negative errno value on failure.
 */
static int zybo_accel_validate_transfer(
	const struct zybo_accel_transfer *transfer)
{
	if (!transfer->input_ptr || !transfer->output_ptr)
		return -EINVAL;

	if (!transfer->length || transfer->length > ZYBO_ACCEL_DMA_MAX_BYTES)
		return -EINVAL;

	if (transfer->length % ZYBO_ACCEL_DMA_ALIGNMENT_BYTES)
		return -EINVAL;

	if (transfer->timeout_ms > ZYBO_ACCEL_DMA_TIMEOUT_MAX)
		return -EINVAL;

	if (transfer->flags || transfer->reserved)
		return -EINVAL;

	return 0;
}

/**
 * zybo_accel_note_transfer_result - update driver counters after one submit
 * @accel: Driver private data.
 * @length: Number of payload bytes accepted for DMA execution.
 * @ret:    Final result returned by the blocking submit path.
 */
static void zybo_accel_note_transfer_result(struct zybo_accel_dev *accel,
					    u32 length, int ret)
{
	accel->stats.last_transfer_bytes = length;
	accel->stats.last_error = ret;

	if (!ret) {
		accel->stats.complete_count++;
		return;
	}

	accel->stats.error_count++;
	if (ret == -ETIMEDOUT)
		accel->stats.timeout_count++;
}

/**
 * zybo_accel_run_transfer - execute one complete blocking DMA transaction
 * @accel:   Driver private data.
 * @transfer: Request already copied and validated from user space.
 *
 * Return: 0 on success, or a negative errno value on failure.
 */
static int zybo_accel_run_transfer(struct zybo_accel_dev *accel,
					   const struct zybo_accel_transfer *transfer)
{
	struct dma_async_tx_descriptor *rx_desc;
	struct dma_async_tx_descriptor *tx_desc;
	dma_cookie_t rx_cookie;
	dma_cookie_t tx_cookie;
	enum dma_ctrl_flags desc_flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
	enum dma_status rx_status;
	enum dma_status tx_status;
	void __user *input_user = u64_to_user_ptr(transfer->input_ptr);
	void __user *output_user = u64_to_user_ptr(transfer->output_ptr);
	u32 timeout_ms = transfer->timeout_ms ?: ZYBO_ACCEL_DMA_TIMEOUT_DEFAULT;
	unsigned long deadline;
	int ret;

	if (copy_from_user(accel->tx_cpu_buf, input_user, transfer->length))
		return -EFAULT;

	memset(accel->rx_cpu_buf, 0, transfer->length);
	reinit_completion(&accel->tx_done);
	reinit_completion(&accel->rx_done);

	/*
	 * Arm S2MM first so the loopback stream has a receive sink before MM2S
	 * starts producing data.
	 */
	rx_desc = dmaengine_prep_slave_single(accel->rx_chan,
					      accel->rx_dma_addr,
					      transfer->length,
					      DMA_DEV_TO_MEM,
					      desc_flags);
	if (!rx_desc)
		return -EIO;

	rx_desc->callback = zybo_accel_dma_callback;
	rx_desc->callback_param = &accel->rx_done;
	rx_cookie = dmaengine_submit(rx_desc);
	ret = dma_submit_error(rx_cookie);
	if (ret)
		return ret;

	tx_desc = dmaengine_prep_slave_single(accel->tx_chan,
					      accel->tx_dma_addr,
					      transfer->length,
					      DMA_MEM_TO_DEV,
					      desc_flags);
	if (!tx_desc) {
		zybo_accel_terminate_dma(accel);
		return -EIO;
	}

	tx_desc->callback = zybo_accel_dma_callback;
	tx_desc->callback_param = &accel->tx_done;
	tx_cookie = dmaengine_submit(tx_desc);
	ret = dma_submit_error(tx_cookie);
	if (ret) {
		zybo_accel_terminate_dma(accel);
		return ret;
	}

	/* Queue both directions, then make the sink live before the source. */
	dma_async_issue_pending(accel->rx_chan);
	dma_async_issue_pending(accel->tx_chan);

	deadline = jiffies + msecs_to_jiffies(timeout_ms);
	ret = zybo_accel_wait_until(&accel->rx_done, deadline);
	if (ret) {
		zybo_accel_terminate_dma(accel);
		return ret;
	}

	ret = zybo_accel_wait_until(&accel->tx_done, deadline);
	if (ret) {
		zybo_accel_terminate_dma(accel);
		return ret;
	}

	rx_status = dma_async_is_tx_complete(accel->rx_chan, rx_cookie,
					      NULL, NULL);
	tx_status = dma_async_is_tx_complete(accel->tx_chan, tx_cookie,
					      NULL, NULL);
	if (rx_status != DMA_COMPLETE || tx_status != DMA_COMPLETE) {
		zybo_accel_terminate_dma(accel);
		return -EIO;
	}

	if (copy_to_user(output_user, accel->rx_cpu_buf, transfer->length))
		return -EFAULT;

	return 0;
}

/**
 * zybo_accel_submit - handle one user-visible blocking DMA submit request
 * @accel:   Driver private data.
 * @transfer: Request already copied from user space.
 *
 * Return: 0 on success, or a negative errno value on failure.
 */
static int zybo_accel_submit(struct zybo_accel_dev *accel,
				     const struct zybo_accel_transfer *transfer)
{
	int ret;

	ret = zybo_accel_validate_transfer(transfer);
	if (ret)
		return ret;

	mutex_lock(&accel->lock);
	accel->stats.submit_count++;
	ret = zybo_accel_run_transfer(accel, transfer);
	zybo_accel_note_transfer_result(accel, transfer->length, ret);
	mutex_unlock(&accel->lock);

	return ret;
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
	struct zybo_accel_dma_caps dma_caps;
	struct zybo_accel_info info;
	struct zybo_accel_scratch scratch;
	struct zybo_accel_stats stats;
	struct zybo_accel_transfer transfer;
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

	case ZYBO_ACCEL_IOCTL_GET_DMA_CAPS:
		zybo_accel_fill_dma_caps(&dma_caps);
		if (copy_to_user(argp, &dma_caps, sizeof(dma_caps)))
			ret = -EFAULT;
		break;

	case ZYBO_ACCEL_IOCTL_SUBMIT:
		if (copy_from_user(&transfer, argp, sizeof(transfer)))
			return -EFAULT;

		ret = zybo_accel_submit(accel, &transfer);
		break;

	case ZYBO_ACCEL_IOCTL_GET_STATS:
		mutex_lock(&accel->lock);
		stats = accel->stats;
		mutex_unlock(&accel->lock);

		if (copy_to_user(argp, &stats, sizeof(stats)))
			ret = -EFAULT;
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
 * zybo_accel_release_dma - release DMA channels and coherent staging buffers
 * @accel: Driver private data.
 */
static void zybo_accel_release_dma(struct zybo_accel_dev *accel)
{
	if (accel->rx_chan)
		dmaengine_terminate_sync(accel->rx_chan);
	if (accel->tx_chan)
		dmaengine_terminate_sync(accel->tx_chan);

	if (accel->rx_cpu_buf)
		dma_free_coherent(accel->rx_dma_dev, accel->dma_buf_size,
				  accel->rx_cpu_buf, accel->rx_dma_addr);
	if (accel->tx_cpu_buf)
		dma_free_coherent(accel->tx_dma_dev, accel->dma_buf_size,
				  accel->tx_cpu_buf, accel->tx_dma_addr);

	if (accel->rx_chan)
		dma_release_channel(accel->rx_chan);
	if (accel->tx_chan)
		dma_release_channel(accel->tx_chan);

	accel->rx_cpu_buf = NULL;
	accel->tx_cpu_buf = NULL;
	accel->rx_chan = NULL;
	accel->tx_chan = NULL;
}

/**
 * zybo_accel_init_dma - acquire DMAEngine channels and allocate staging buffers
 * @pdev:  Platform device being probed.
 * @accel: Driver private data.
 *
 * Return: 0 on success, or a negative errno value on failure.
 */
static int zybo_accel_init_dma(struct platform_device *pdev,
				       struct zybo_accel_dev *accel)
{
	int ret;

	accel->tx_chan = dma_request_chan(&pdev->dev, "tx");
	if (IS_ERR(accel->tx_chan)) {
		ret = PTR_ERR(accel->tx_chan);
		accel->tx_chan = NULL;
		return dev_err_probe(&pdev->dev, ret,
					     "failed to acquire DMA channel 'tx'\n");
	}

	accel->rx_chan = dma_request_chan(&pdev->dev, "rx");
	if (IS_ERR(accel->rx_chan)) {
		ret = PTR_ERR(accel->rx_chan);
		accel->rx_chan = NULL;
		zybo_accel_release_dma(accel);
		return dev_err_probe(&pdev->dev, ret,
					     "failed to acquire DMA channel 'rx'\n");
	}

	accel->tx_dma_dev = dmaengine_get_dma_device(accel->tx_chan);
	accel->rx_dma_dev = dmaengine_get_dma_device(accel->rx_chan);
	if (!accel->tx_dma_dev || !accel->rx_dma_dev) {
		zybo_accel_release_dma(accel);
		return dev_err_probe(&pdev->dev, -ENODEV,
					     "DMA channels do not expose mapping devices\n");
	}

	accel->dma_buf_size = ZYBO_ACCEL_DMA_MAX_BYTES;
	accel->tx_cpu_buf = dma_alloc_coherent(accel->tx_dma_dev,
					       accel->dma_buf_size,
					       &accel->tx_dma_addr,
					       GFP_KERNEL);
	if (!accel->tx_cpu_buf) {
		zybo_accel_release_dma(accel);
		return dev_err_probe(&pdev->dev, -ENOMEM,
					     "failed to allocate coherent TX staging buffer\n");
	}

	accel->rx_cpu_buf = dma_alloc_coherent(accel->rx_dma_dev,
					       accel->dma_buf_size,
					       &accel->rx_dma_addr,
					       GFP_KERNEL);
	if (!accel->rx_cpu_buf) {
		zybo_accel_release_dma(accel);
		return dev_err_probe(&pdev->dev, -ENOMEM,
					     "failed to allocate coherent RX staging buffer\n");
	}

	init_completion(&accel->tx_done);
	init_completion(&accel->rx_done);
	memset(&accel->stats, 0, sizeof(accel->stats));

	dev_info(&pdev->dev,
		 "DMA channels ready: tx=MM2S, rx=S2MM, staging=%zu bytes per direction\n",
		 accel->dma_buf_size);

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

	ret = zybo_accel_init_dma(pdev, accel);
	if (ret)
		return ret;

	accel->miscdev.minor = MISC_DYNAMIC_MINOR;
	accel->miscdev.name = ZYBO_ACCEL_DEVICE_NAME;
	accel->miscdev.fops = &zybo_accel_fops;
	accel->miscdev.parent = &pdev->dev;
	accel->miscdev.mode = 0600;

	ret = misc_register(&accel->miscdev);
	if (ret) {
		zybo_accel_release_dma(accel);
		return dev_err_probe(&pdev->dev, ret,
					     "failed to register /dev/%s\n",
					     ZYBO_ACCEL_DEVICE_NAME);
	}

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
	mutex_lock(&accel->lock);
	zybo_accel_release_dma(accel);
	mutex_unlock(&accel->lock);
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
MODULE_DESCRIPTION("Zybo Z7 Linux-controlled FPGA platform driver");
MODULE_LICENSE("GPL");

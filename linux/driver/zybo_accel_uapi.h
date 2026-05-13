/* SPDX-License-Identifier: MIT */
#ifndef ZYBO_ACCEL_UAPI_H
#define ZYBO_ACCEL_UAPI_H

/*
 * User-space API for the Zybo Z7 AXI-Lite accelerator control driver.
 *
 * This header is intentionally small and stable.  It describes the ABI
 * between user-space tools and the kernel module; hardware-private details
 * such as MMIO register offsets stay in the driver source file.
 */

#include <linux/ioctl.h>
#include <linux/types.h>

#define ZYBO_ACCEL_DEVICE_NAME          "zybo_accel0"
#define ZYBO_ACCEL_ABI_VERSION          1U
#define ZYBO_ACCEL_IOCTL_MAGIC          'Z'

/**
 * struct zybo_accel_info - static information returned by the driver
 * @abi_version:      User-space ABI version implemented by the driver.
 * @hardware_version: Value read from the FPGA VERSION register.
 * @register_span:    Size, in bytes, of the mapped MMIO register resource.
 * @reserved:         Reserved for future use; always returned as zero.
 */
struct zybo_accel_info {
	__u32 abi_version;
	__u32 hardware_version;
	__u32 register_span;
	__u32 reserved;
};

/**
 * struct zybo_accel_scratch - SCRATCH register payload
 * @value: Value written to or read from the FPGA SCRATCH register.
 */
struct zybo_accel_scratch {
	__u32 value;
};

/*
 * IOCTL numbering policy:
 * 0x00: Device and ABI discovery.
 * 0x10-0x1f: AXI-Lite register-control helpers for the bring-up milestone.
 * Future DMA/control commands should use separate ranges rather than changing
 * the meaning or layout of these existing commands.
 */
#define ZYBO_ACCEL_IOCTL_GET_INFO \
	_IOR(ZYBO_ACCEL_IOCTL_MAGIC, 0x00, struct zybo_accel_info)

#define ZYBO_ACCEL_IOCTL_SCRATCH_READ \
	_IOR(ZYBO_ACCEL_IOCTL_MAGIC, 0x10, struct zybo_accel_scratch)

#define ZYBO_ACCEL_IOCTL_SCRATCH_WRITE \
	_IOW(ZYBO_ACCEL_IOCTL_MAGIC, 0x11, struct zybo_accel_scratch)

#endif /* ZYBO_ACCEL_UAPI_H */

/* SPDX-License-Identifier: MIT */
#ifndef ZYBO_ACCEL_UAPI_H
#define ZYBO_ACCEL_UAPI_H

/*
 * User-space API for the Zybo Z7 Linux-controlled FPGA platform driver.
 *
 * The ABI is intentionally small.  User space asks the kernel driver to
 * perform complete accelerator transactions; it never receives physical
 * addresses or direct ownership of the AXI DMA hardware.
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
 * DMA capability flags returned in struct zybo_accel_dma_caps::flags.
 *
 * The current DMA path uses a blocking submit model and driver-owned staging
 * buffers.  These flags let user space verify those ABI properties explicitly.
 */
#define ZYBO_ACCEL_DMA_CAP_BLOCKING_SUBMIT       (1U << 0)
#define ZYBO_ACCEL_DMA_CAP_DRIVER_STAGING_BUFS   (1U << 1)

/**
 * struct zybo_accel_dma_caps - DMA transaction limits exposed by the driver
 * @max_transfer_bytes:        Largest payload accepted by SUBMIT.
 * @transfer_alignment_bytes:  Required transfer-length alignment.
 * @default_timeout_ms:         Timeout used when SUBMIT passes zero.
 * @max_timeout_ms:             Largest caller-supplied timeout accepted.
 * @flags:                      ZYBO_ACCEL_DMA_CAP_* flags.
 * @reserved:                   Reserved for future use; always returned zero.
 */
struct zybo_accel_dma_caps {
	__u32 max_transfer_bytes;
	__u32 transfer_alignment_bytes;
	__u32 default_timeout_ms;
	__u32 max_timeout_ms;
	__u32 flags;
	__u32 reserved[3];
};

/**
 * struct zybo_accel_transfer - one blocking accelerator transaction
 * @input_ptr:   User-space pointer to the input payload.
 * @output_ptr:  User-space pointer to the output payload.
 * @length:      Number of bytes to transfer.
 * @timeout_ms:  Per-transaction timeout, or zero for the driver default.
 * @flags:       Reserved for future transaction modes; must be zero today.
 * @reserved:    Reserved for future use; must be zero today.
 */
struct zybo_accel_transfer {
	__u64 input_ptr;
	__u64 output_ptr;
	__u32 length;
	__u32 timeout_ms;
	__u32 flags;
	__u32 reserved;
};

/**
 * struct zybo_accel_stats - accumulated DMA transaction counters
 * @submit_count:         Number of DMA transactions accepted for execution.
 * @complete_count:       Number of DMA transactions completed successfully.
 * @timeout_count:        Number of transactions stopped by timeout handling.
 * @error_count:          Number of accepted transactions that failed.
 * @last_transfer_bytes:  Payload length of the last accepted transaction.
 * @last_error:           Last accepted transaction result, using errno values.
 * @reserved:             Reserved for future use; always returned zero.
 */
struct zybo_accel_stats {
	__u64 submit_count;
	__u64 complete_count;
	__u64 timeout_count;
	__u64 error_count;
	__u32 last_transfer_bytes;
	__s32 last_error;
	__u32 reserved[2];
};

/*
 * IOCTL numbering policy:
 * 0x00: Device and ABI discovery.
 * 0x10-0x1f: AXI-Lite register-control helpers from the first milestone.
 * 0x20-0x2f: DMA-backed transaction control and status.
 */
#define ZYBO_ACCEL_IOCTL_GET_INFO \
	_IOR(ZYBO_ACCEL_IOCTL_MAGIC, 0x00, struct zybo_accel_info)

#define ZYBO_ACCEL_IOCTL_SCRATCH_READ \
	_IOR(ZYBO_ACCEL_IOCTL_MAGIC, 0x10, struct zybo_accel_scratch)

#define ZYBO_ACCEL_IOCTL_SCRATCH_WRITE \
	_IOW(ZYBO_ACCEL_IOCTL_MAGIC, 0x11, struct zybo_accel_scratch)

#define ZYBO_ACCEL_IOCTL_GET_DMA_CAPS \
	_IOR(ZYBO_ACCEL_IOCTL_MAGIC, 0x20, struct zybo_accel_dma_caps)

#define ZYBO_ACCEL_IOCTL_SUBMIT \
	_IOW(ZYBO_ACCEL_IOCTL_MAGIC, 0x21, struct zybo_accel_transfer)

#define ZYBO_ACCEL_IOCTL_GET_STATS \
	_IOR(ZYBO_ACCEL_IOCTL_MAGIC, 0x22, struct zybo_accel_stats)

#endif /* ZYBO_ACCEL_UAPI_H */

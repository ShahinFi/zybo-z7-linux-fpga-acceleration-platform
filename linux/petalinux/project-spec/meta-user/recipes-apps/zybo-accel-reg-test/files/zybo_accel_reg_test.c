#define _GNU_SOURCE
/* SPDX-License-Identifier: MIT */
/*
 * Register-access validation tool for /dev/zybo_accel0.
 *
 * This program verifies the first proper driver milestone:
 *   1. Query driver/device information.
 *   2. Check the VERSION register value returned by the driver.
 *   3. Write two known patterns to SCRATCH.
 *   4. Read SCRATCH back and verify exact equality.
 *
 * The program intentionally performs no direct /dev/mem or physical-address
 * access.  It validates the public ioctl-based ABI exposed by the driver.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "zybo_accel_uapi.h"

#define EXPECTED_HW_VERSION            0x00010000U

static int die_errno(const char *what)
{
	fprintf(stderr, "error: %s: %s\n", what, strerror(errno));
	return EXIT_FAILURE;
}

static int get_info(int fd, struct zybo_accel_info *info)
{
	if (ioctl(fd, ZYBO_ACCEL_IOCTL_GET_INFO, info) < 0)
		return -1;

	return 0;
}

static int scratch_write(int fd, uint32_t value)
{
	struct zybo_accel_scratch scratch = {
		.value = value,
	};

	if (ioctl(fd, ZYBO_ACCEL_IOCTL_SCRATCH_WRITE, &scratch) < 0)
		return -1;

	return 0;
}

static int scratch_read(int fd, uint32_t *value)
{
	struct zybo_accel_scratch scratch = { 0 };

	if (ioctl(fd, ZYBO_ACCEL_IOCTL_SCRATCH_READ, &scratch) < 0)
		return -1;

	*value = scratch.value;
	return 0;
}

static bool scratch_roundtrip(int fd, uint32_t pattern)
{
	uint32_t observed = 0;

	printf("SCRATCH write: 0x%08" PRIx32 "\n", pattern);
	if (scratch_write(fd, pattern) < 0) {
		perror("ioctl(SCRATCH_WRITE)");
		return false;
	}

	if (scratch_read(fd, &observed) < 0) {
		perror("ioctl(SCRATCH_READ)");
		return false;
	}

	printf("SCRATCH read : 0x%08" PRIx32 "\n", observed);
	if (observed != pattern) {
		fprintf(stderr,
			"mismatch: expected 0x%08" PRIx32 ", got 0x%08" PRIx32 "\n",
			pattern, observed);
		return false;
	}

	puts("SCRATCH check: PASS");
	return true;
}

int main(int argc, char **argv)
{
	const char *device_path = "/dev/" ZYBO_ACCEL_DEVICE_NAME;
	const uint32_t patterns[] = { 0x12345678U, 0xA5A5A5A5U };
	struct zybo_accel_info info = { 0 };
	bool all_passed = true;
	int fd;
	size_t i;

	if (argc > 2) {
		fprintf(stderr, "usage: %s [device-path]\n", argv[0]);
		return EXIT_FAILURE;
	}

	if (argc == 2)
		device_path = argv[1];

	fd = open(device_path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return die_errno("open device");

	if (get_info(fd, &info) < 0) {
		close(fd);
		return die_errno("ioctl(GET_INFO)");
	}

	printf("Device path      : %s\n", device_path);
	printf("Driver ABI       : %" PRIu32 "\n", info.abi_version);
	printf("Hardware VERSION  : 0x%08" PRIx32 "\n", info.hardware_version);
	printf("MMIO register span: %" PRIu32 " bytes\n", info.register_span);

	if (info.abi_version != ZYBO_ACCEL_ABI_VERSION) {
		fprintf(stderr,
			"warning: expected ABI version %u, driver reported %" PRIu32 "\n",
			ZYBO_ACCEL_ABI_VERSION, info.abi_version);
		all_passed = false;
	}

	if (info.hardware_version != EXPECTED_HW_VERSION) {
		fprintf(stderr,
			"warning: expected hardware VERSION 0x%08x, got 0x%08" PRIx32 "\n",
			EXPECTED_HW_VERSION, info.hardware_version);
		all_passed = false;
	}

	for (i = 0; i < sizeof(patterns) / sizeof(patterns[0]); ++i) {
		if (!scratch_roundtrip(fd, patterns[i]))
			all_passed = false;
	}

	if (close(fd) < 0)
		return die_errno("close device");

	puts(all_passed ? "Overall result    : PASS" : "Overall result    : FAIL");
	return all_passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

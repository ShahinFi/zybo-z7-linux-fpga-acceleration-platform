#define _GNU_SOURCE
/* SPDX-License-Identifier: MIT */
/*
 * DMA XOR-accelerator validation tool for /dev/zybo_accel0.
 *
 * The current programmable-logic stream path is:
 *
 *   AXI DMA MM2S
 *   -> fixed-XOR AXI-Stream validation accelerator
 *   -> AXI DMA S2MM
 *
 * The accelerator XORs every payload byte with 0xA5 while preserving stream
 * length, TKEEP, TLAST, and transaction ordering. This program validates the
 * complete public software and hardware path:
 *
 *   user-space test
 *   -> zybo_accel ioctl ABI
 *   -> kernel driver DMA transaction
 *   -> AXI DMA MM2S
 *   -> FPGA XOR validation accelerator
 *   -> AXI DMA S2MM
 *   -> returned user-space output buffer
 *
 * The tool supports:
 * - one manually selected transfer case,
 * - a broad repeated validation suite across sizes, patterns, and timeout use,
 * - negative tests that confirm invalid requests are rejected by the driver.
 *
 * This is intentionally a correctness validator, not a benchmark program.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "zybo_accel_uapi.h"

#define DEFAULT_TRANSFER_LENGTH          4096U
#define DEFAULT_SINGLE_RUN_COUNT         1U
#define DEFAULT_SUITE_RUN_COUNT          10U
#define OUTPUT_SENTINEL                  0xcdU
#define XOR_BYTE_CONSTANT                0xa5U

/*
 * Input data patterns used by the positive validation cases.
 *
 * Multiple deterministic patterns reduce the risk of accepting a path that
 * only happens to work for one convenient buffer shape.
 */
enum dma_pattern {
	PATTERN_AFFINE = 0,
	PATTERN_ZERO,
	PATTERN_FF,
	PATTERN_ALTERNATING,
	PATTERN_INCREMENT,
};

struct pattern_desc {
	enum dma_pattern pattern;
	const char *name;
};

static const struct pattern_desc suite_patterns[] = {
	{ PATTERN_AFFINE,      "affine" },
	{ PATTERN_ZERO,        "zero" },
	{ PATTERN_FF,          "ff" },
	{ PATTERN_ALTERNATING, "alternating" },
	{ PATTERN_INCREMENT,   "increment" },
};

/*
 * Representative payload sizes for first-stage AXI-Stream accelerator
 * validation.
 *
 * These sizes span small control-like buffers, moderate payloads, and the
 * current maximum transfer policy used by the DMA driver milestone.
 */
static const uint32_t suite_sizes[] = {
	64U,
	256U,
	1024U,
	4096U,
	16384U,
	65536U,
	262144U,
	1048576U,
};

/*
 * Command-line configuration resolved before the device is opened.
 *
 * In suite mode, count means runs per positive test case. In single-case mode,
 * it means repetitions of the requested one transfer case.
 */
struct cli_options {
	const char *device_path;
	uint32_t length;
	uint32_t count;
	uint32_t timeout_ms;
	enum dma_pattern pattern;
	bool timeout_was_set;
	bool suite_mode;
};

/*
 * Aggregated result counters for the final PASS/FAIL summary.
 *
 * Positive "cases" are unique size/pattern/timeout combinations. Positive
 * "runs" are individual DMA transactions inside those cases.
 */
struct suite_summary {
	uint32_t positive_cases_attempted;
	uint32_t positive_cases_passed;
	uint64_t positive_runs_passed;
	uint32_t negative_tests_attempted;
	uint32_t negative_tests_passed;
	uint32_t skipped_tests;
	uint32_t failures;
};

static int die_errno(const char *what)
{
	fprintf(stderr, "error: %s: %s\n", what, strerror(errno));
	return EXIT_FAILURE;
}

/*
 * Parse command-line numeric values using base autodetection while rejecting
 * partial parses, overflow, and empty input.
 */
static int parse_u32(const char *text, uint32_t *value)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 0);
	if (errno || end == text || *end != '\0' || parsed > UINT32_MAX)
		return -1;

	*value = (uint32_t)parsed;
	return 0;
}

static const char *pattern_name(enum dma_pattern pattern)
{
	size_t i;

	for (i = 0; i < sizeof(suite_patterns) / sizeof(suite_patterns[0]); ++i) {
		if (suite_patterns[i].pattern == pattern)
			return suite_patterns[i].name;
	}

	return "unknown";
}

static int parse_pattern(const char *text, enum dma_pattern *pattern)
{
	size_t i;

	for (i = 0; i < sizeof(suite_patterns) / sizeof(suite_patterns[0]); ++i) {
		if (!strcmp(text, suite_patterns[i].name)) {
			*pattern = suite_patterns[i].pattern;
			return 0;
		}
	}

	return -1;
}

static void print_usage(const char *program)
{
	printf("usage:\n");
	printf("  %s [options]\n", program);
	printf("\n");
	printf("single-case mode options:\n");
	printf("  -d, --device PATH       device path, default /dev/%s\n",
	       ZYBO_ACCEL_DEVICE_NAME);
	printf("  -s, --size BYTES        transfer length, default %" PRIu32 "\n",
	       DEFAULT_TRANSFER_LENGTH);
	printf("  -n, --count COUNT       repeated runs, default %" PRIu32 "\n",
	       DEFAULT_SINGLE_RUN_COUNT);
	printf("  -t, --timeout MS        timeout in ms, default 0 for driver default\n");
	printf("  -p, --pattern NAME      affine, zero, ff, alternating, increment\n");
	printf("\n");
	printf("suite mode:\n");
	printf("  -S, --suite             run the full XOR accelerator validation suite\n");
	printf("  -n, --count COUNT       runs per positive case, default %" PRIu32 "\n",
	       DEFAULT_SUITE_RUN_COUNT);
	printf("  -t, --timeout MS        optional fixed suite timeout;\n");
	printf("                          otherwise suite tests both default and explicit\n");
	printf("                          driver-default timeout modes\n");
	printf("\n");
	printf("other:\n");
	printf("  -h, --help              show this help\n");
}

/*
 * Resolve the public command-line interface.
 *
 * The parser keeps suite-mode policy in this layer so the execution routines
 * receive a normalized configuration and do not need to interpret argv
 * directly.
 */
static int parse_options(int argc, char **argv, struct cli_options *options)
{
	static const struct option long_options[] = {
		{ "device",  required_argument, NULL, 'd' },
		{ "size",    required_argument, NULL, 's' },
		{ "count",   required_argument, NULL, 'n' },
		{ "timeout", required_argument, NULL, 't' },
		{ "pattern", required_argument, NULL, 'p' },
		{ "suite",   no_argument,       NULL, 'S' },
		{ "help",    no_argument,       NULL, 'h' },
		{ NULL,      0,                 NULL,  0  },
	};
	int opt;

	options->device_path = "/dev/" ZYBO_ACCEL_DEVICE_NAME;
	options->length = DEFAULT_TRANSFER_LENGTH;
	options->count = DEFAULT_SINGLE_RUN_COUNT;
	options->timeout_ms = 0U;
	options->pattern = PATTERN_AFFINE;
	options->timeout_was_set = false;
	options->suite_mode = false;

	while ((opt = getopt_long(argc, argv, "d:s:n:t:p:Sh",
				  long_options, NULL)) != -1) {
		switch (opt) {
		case 'd':
			options->device_path = optarg;
			break;

		case 's':
			if (parse_u32(optarg, &options->length) < 0) {
				fprintf(stderr, "error: invalid size '%s'\n", optarg);
				return -1;
			}
			break;

		case 'n':
			if (parse_u32(optarg, &options->count) < 0 ||
			    !options->count) {
				fprintf(stderr, "error: invalid count '%s'\n", optarg);
				return -1;
			}
			break;

		case 't':
			if (parse_u32(optarg, &options->timeout_ms) < 0) {
				fprintf(stderr, "error: invalid timeout '%s'\n", optarg);
				return -1;
			}
			options->timeout_was_set = true;
			break;

		case 'p':
			if (parse_pattern(optarg, &options->pattern) < 0) {
				fprintf(stderr, "error: invalid pattern '%s'\n", optarg);
				return -1;
			}
			break;

		case 'S':
			options->suite_mode = true;
			options->count = DEFAULT_SUITE_RUN_COUNT;
			break;

		case 'h':
			print_usage(argv[0]);
			exit(EXIT_SUCCESS);

		default:
			return -1;
		}
	}

	if (optind != argc) {
		fprintf(stderr, "error: unexpected positional argument '%s'\n",
			argv[optind]);
		return -1;
	}

	return 0;
}

/*
 * Populate one deterministic input payload.
 *
 * run_index perturbs selected patterns across repetitions so repeated tests are
 * not just identical buffer resubmissions.
 */
static void fill_pattern(uint8_t *buffer, uint32_t length,
			 enum dma_pattern pattern, uint32_t run_index)
{
	uint32_t i;

	switch (pattern) {
	case PATTERN_AFFINE:
		for (i = 0; i < length; ++i)
			buffer[i] = (uint8_t)((i * 37U + 11U +
					       run_index * 17U) & 0xffU);
		break;

	case PATTERN_ZERO:
		memset(buffer, 0x00, length);
		break;

	case PATTERN_FF:
		memset(buffer, 0xff, length);
		break;

	case PATTERN_ALTERNATING:
		for (i = 0; i < length; ++i)
			buffer[i] = ((i + run_index) & 1U) ? 0x55U : 0xaaU;
		break;

	case PATTERN_INCREMENT:
		for (i = 0; i < length; ++i)
			buffer[i] = (uint8_t)((i + run_index) & 0xffU);
		break;
	}
}

/*
 * Verify the fixed-XOR accelerator result.
 *
 * The hardware is expected to transform every payload byte as:
 *
 *   output = input XOR 0xA5
 *
 * First-failure reporting keeps the output compact while still providing an
 * exact failure location for accelerator, DMA, or stream-handshake debugging.
 */
static bool compare_xor_output(const uint8_t *input, const uint8_t *observed,
			       uint32_t length)
{
	uint32_t i;

	for (i = 0; i < length; ++i) {
		uint8_t expected = input[i] ^ XOR_BYTE_CONSTANT;

		if (expected != observed[i]) {
			fprintf(stderr,
				"mismatch at byte %" PRIu32
				": input 0x%02x, expected 0x%02x, got 0x%02x\n",
				i, input[i], expected, observed[i]);
			return false;
		}
	}

	return true;
}

static void print_caps(const struct zybo_accel_dma_caps *caps)
{
	printf("DMA max transfer : %" PRIu32 " bytes\n", caps->max_transfer_bytes);
	printf("DMA alignment    : %" PRIu32 " bytes\n",
	       caps->transfer_alignment_bytes);
	printf("Default timeout  : %" PRIu32 " ms\n", caps->default_timeout_ms);
	printf("Maximum timeout  : %" PRIu32 " ms\n", caps->max_timeout_ms);
	printf("DMA flags        : 0x%08" PRIx32 "\n", caps->flags);
}

static void print_stats(const struct zybo_accel_stats *stats)
{
	printf("Submit count     : %" PRIu64 "\n", (uint64_t)stats->submit_count);
	printf("Complete count   : %" PRIu64 "\n", (uint64_t)stats->complete_count);
	printf("Timeout count    : %" PRIu64 "\n", (uint64_t)stats->timeout_count);
	printf("Error count      : %" PRIu64 "\n", (uint64_t)stats->error_count);
	printf("Last bytes       : %" PRIu32 "\n", stats->last_transfer_bytes);
	printf("Last error       : %" PRId32 "\n", stats->last_error);
}

static int get_stats(int fd, struct zybo_accel_stats *stats)
{
	memset(stats, 0, sizeof(*stats));

	if (ioctl(fd, ZYBO_ACCEL_IOCTL_GET_STATS, stats) < 0)
		return -1;

	return 0;
}

/*
 * Validate requests that the test tool expects to succeed.
 *
 * Positive tests should fail early in user space if they contradict the
 * driver's published capability contract.
 */
static int validate_requested_length(uint32_t length,
				     const struct zybo_accel_dma_caps *caps)
{
	if (!length || length > caps->max_transfer_bytes)
		return -1;

	if (caps->transfer_alignment_bytes &&
	    length % caps->transfer_alignment_bytes)
		return -1;

	return 0;
}

static int validate_requested_timeout(uint32_t timeout_ms,
				      const struct zybo_accel_dma_caps *caps)
{
	if (timeout_ms > caps->max_timeout_ms)
		return -1;

	return 0;
}

/*
 * Confirm that a successful transaction also produced the expected driver
 * accounting side effects.
 *
 * This checks the software-visible transaction contract, not only transformed
 * payload bytes. Correct XOR output with broken stats would still indicate
 * driver behavior that needs correction before later benchmarking.
 */
static bool verify_positive_stats(const struct zybo_accel_stats *before,
				  const struct zybo_accel_stats *after,
				  uint32_t expected_length)
{
	bool ok = true;

	if (after->submit_count != before->submit_count + 1U) {
		fprintf(stderr,
			"stats mismatch: submit_count expected %" PRIu64 ", got %" PRIu64 "\n",
			(uint64_t)(before->submit_count + 1U),
			(uint64_t)after->submit_count);
		ok = false;
	}

	if (after->complete_count != before->complete_count + 1U) {
		fprintf(stderr,
			"stats mismatch: complete_count expected %" PRIu64 ", got %" PRIu64 "\n",
			(uint64_t)(before->complete_count + 1U),
			(uint64_t)after->complete_count);
		ok = false;
	}

	if (after->timeout_count != before->timeout_count) {
		fprintf(stderr,
			"stats mismatch: timeout_count changed from %" PRIu64 " to %" PRIu64 "\n",
			(uint64_t)before->timeout_count,
			(uint64_t)after->timeout_count);
		ok = false;
	}

	if (after->error_count != before->error_count) {
		fprintf(stderr,
			"stats mismatch: error_count changed from %" PRIu64 " to %" PRIu64 "\n",
			(uint64_t)before->error_count,
			(uint64_t)after->error_count);
		ok = false;
	}

	if (after->last_transfer_bytes != expected_length) {
		fprintf(stderr,
			"stats mismatch: last_transfer_bytes expected %" PRIu32 ", got %" PRIu32 "\n",
			expected_length, after->last_transfer_bytes);
		ok = false;
	}

	if (after->last_error != 0) {
		fprintf(stderr,
			"stats mismatch: last_error expected 0, got %" PRId32 "\n",
			after->last_error);
		ok = false;
	}

	return ok;
}

/*
 * Rejected requests must fail before becoming accepted DMA transactions.
 *
 * The driver's accumulated DMA stats therefore should not change when the ABI
 * rejects invalid user input.
 */
static bool verify_stats_unchanged(const struct zybo_accel_stats *before,
				   const struct zybo_accel_stats *after)
{
	bool ok = true;

	if (after->submit_count != before->submit_count ||
	    after->complete_count != before->complete_count ||
	    after->timeout_count != before->timeout_count ||
	    after->error_count != before->error_count ||
	    after->last_transfer_bytes != before->last_transfer_bytes ||
	    after->last_error != before->last_error) {
		fprintf(stderr, "stats mismatch: rejected request changed driver stats\n");
		ok = false;
	}

	return ok;
}

/*
 * Execute one positive validation case repeatedly.
 *
 * A case is defined by one size, one pattern, one timeout setting, and one run
 * count. Every run must satisfy both transformed-payload correctness and
 * driver-statistics correctness before the case is accepted.
 */
static int run_positive_case(int fd, const struct zybo_accel_dma_caps *caps,
			     uint32_t length, uint32_t timeout_ms,
			     enum dma_pattern pattern, uint32_t count,
			     uint64_t *runs_passed)
{
	struct zybo_accel_transfer transfer = { 0 };
	struct zybo_accel_stats before = { 0 };
	struct zybo_accel_stats after = { 0 };
	uint8_t *input = NULL;
	uint8_t *output = NULL;
	uint32_t run;
	int ret = -1;

	*runs_passed = 0;

	if (validate_requested_length(length, caps) < 0) {
		fprintf(stderr,
			"[FAIL] invalid positive test size=%" PRIu32 "\n",
			length);
		return -1;
	}

	if (validate_requested_timeout(timeout_ms, caps) < 0) {
		fprintf(stderr,
			"[FAIL] invalid positive test timeout=%" PRIu32 "\n",
			timeout_ms);
		return -1;
	}

	input = malloc(length);
	output = malloc(length);
	if (!input || !output) {
		fprintf(stderr,
			"[FAIL] allocation failed for %" PRIu32 "-byte buffers\n",
			length);
		goto out;
	}

	for (run = 0; run < count; ++run) {
		fill_pattern(input, length, pattern, run);
		memset(output, OUTPUT_SENTINEL, length);

		if (get_stats(fd, &before) < 0) {
			perror("ioctl(GET_STATS before SUBMIT)");
			goto out;
		}

		transfer.input_ptr = (uintptr_t)input;
		transfer.output_ptr = (uintptr_t)output;
		transfer.length = length;
		transfer.timeout_ms = timeout_ms;
		transfer.flags = 0U;
		transfer.reserved = 0U;

		if (ioctl(fd, ZYBO_ACCEL_IOCTL_SUBMIT, &transfer) < 0) {
			fprintf(stderr,
				"[FAIL] submit failed: size=%" PRIu32
				" pattern=%s timeout=%" PRIu32
				" run=%" PRIu32 ": %s\n",
				length, pattern_name(pattern), timeout_ms,
				run + 1U, strerror(errno));
			goto out;
		}

		if (!compare_xor_output(input, output, length)) {
			fprintf(stderr,
				"[FAIL] XOR output mismatch: size=%" PRIu32
				" pattern=%s timeout=%" PRIu32
				" run=%" PRIu32 "\n",
				length, pattern_name(pattern), timeout_ms,
				run + 1U);
			goto out;
		}

		if (get_stats(fd, &after) < 0) {
			perror("ioctl(GET_STATS after SUBMIT)");
			goto out;
		}

		if (!verify_positive_stats(&before, &after, length)) {
			fprintf(stderr,
				"[FAIL] stats verification failed: size=%" PRIu32
				" pattern=%s timeout=%" PRIu32
				" run=%" PRIu32 "\n",
				length, pattern_name(pattern), timeout_ms,
				run + 1U);
			goto out;
		}

		++(*runs_passed);
	}

	printf("[PASS] size=%" PRIu32 " pattern=%s timeout=%" PRIu32
	       " ms runs=%" PRIu32 "\n",
	       length, pattern_name(pattern), timeout_ms, count);
	ret = 0;

out:
	free(output);
	free(input);
	return ret;
}

/*
 * Submit one deliberately invalid transaction and require the driver to reject
 * it without modifying accepted-transaction statistics.
 */
static int expect_submit_rejected(int fd, const char *name,
				  const struct zybo_accel_transfer *transfer)
{
	struct zybo_accel_transfer request = *transfer;
	struct zybo_accel_stats before = { 0 };
	struct zybo_accel_stats after = { 0 };
	int saved_errno;

	if (get_stats(fd, &before) < 0) {
		perror("ioctl(GET_STATS before rejected SUBMIT)");
		return -1;
	}

	errno = 0;
	if (ioctl(fd, ZYBO_ACCEL_IOCTL_SUBMIT, &request) == 0) {
		fprintf(stderr,
			"[FAIL] reject=%s unexpectedly succeeded\n",
			name);
		return -1;
	}

	saved_errno = errno;

	if (get_stats(fd, &after) < 0) {
		perror("ioctl(GET_STATS after rejected SUBMIT)");
		return -1;
	}

	if (!verify_stats_unchanged(&before, &after)) {
		fprintf(stderr,
			"[FAIL] reject=%s changed driver stats\n",
			name);
		return -1;
	}

	printf("[PASS] reject=%s errno=%d (%s)\n",
	       name, saved_errno, strerror(saved_errno));
	return 0;
}

/*
 * Choose a small transfer length that should be valid for normal accepted
 * transactions. Negative timeout, flags, and reserved-field tests need a valid
 * payload length so only the targeted field causes rejection.
 */
static uint32_t smallest_valid_probe_length(const struct zybo_accel_dma_caps *caps)
{
	uint32_t alignment;

	alignment = caps->transfer_alignment_bytes ?
		    caps->transfer_alignment_bytes : 1U;

	if (alignment > caps->max_transfer_bytes)
		return 0U;

	return alignment;
}

/*
 * Exercise ABI rejection paths that are meaningful for the current driver
 * contract.
 *
 * These checks are intentionally sent into the driver instead of being filtered
 * in user space, because the purpose is to verify the driver's validation
 * behavior before the platform grows more complex.
 */
static int run_negative_tests(int fd, const struct zybo_accel_dma_caps *caps,
			      struct suite_summary *summary)
{
	struct zybo_accel_transfer transfer = { 0 };
	uint8_t *input = NULL;
	uint8_t *output = NULL;
	uint8_t *oversize_input = NULL;
	uint8_t *oversize_output = NULL;
	uint32_t probe_length;
	uint32_t alignment;
	uint32_t oversize_length = 0U;
	int ret = 0;

	probe_length = smallest_valid_probe_length(caps);
	alignment = caps->transfer_alignment_bytes ?
		    caps->transfer_alignment_bytes : 1U;

	if (!probe_length) {
		fprintf(stderr,
			"[FAIL] no valid probe length available for negative tests\n");
		++summary->failures;
		return -1;
	}

	input = malloc(probe_length);
	output = malloc(probe_length);
	if (!input || !output) {
		fprintf(stderr,
			"[FAIL] allocation failed for negative-test probe buffers\n");
		++summary->failures;
		ret = -1;
		goto out;
	}

	fill_pattern(input, probe_length, PATTERN_AFFINE, 0U);
	memset(output, OUTPUT_SENTINEL, probe_length);

	transfer.input_ptr = (uintptr_t)input;
	transfer.output_ptr = (uintptr_t)output;
	transfer.length = 0U;
	transfer.timeout_ms = 0U;
	transfer.flags = 0U;
	transfer.reserved = 0U;

	++summary->negative_tests_attempted;
	if (expect_submit_rejected(fd, "length-zero", &transfer) < 0) {
		++summary->failures;
		ret = -1;
	} else {
		++summary->negative_tests_passed;
	}

	if (alignment > 1U) {
		transfer.length = alignment - 1U;

		++summary->negative_tests_attempted;
		if (expect_submit_rejected(fd, "length-unaligned", &transfer) < 0) {
			++summary->failures;
			ret = -1;
		} else {
			++summary->negative_tests_passed;
		}
	} else {
		printf("[SKIP] reject=length-unaligned alignment requirement is 1 byte\n");
		++summary->skipped_tests;
	}

	if (caps->max_transfer_bytes <= UINT32_MAX - alignment) {
		oversize_length = caps->max_transfer_bytes + alignment;
		oversize_input = malloc(oversize_length);
		oversize_output = malloc(oversize_length);

		if (!oversize_input || !oversize_output) {
			fprintf(stderr,
				"[FAIL] allocation failed for oversize negative-test buffers\n");
			++summary->failures;
			ret = -1;
		} else {
			memset(oversize_input, 0xa5, oversize_length);
			memset(oversize_output, OUTPUT_SENTINEL, oversize_length);

			transfer.input_ptr = (uintptr_t)oversize_input;
			transfer.output_ptr = (uintptr_t)oversize_output;
			transfer.length = oversize_length;
			transfer.timeout_ms = 0U;
			transfer.flags = 0U;
			transfer.reserved = 0U;

			++summary->negative_tests_attempted;
			if (expect_submit_rejected(fd, "length-over-limit",
						   &transfer) < 0) {
				++summary->failures;
				ret = -1;
			} else {
				++summary->negative_tests_passed;
			}
		}
	} else {
		printf("[SKIP] reject=length-over-limit would overflow uint32_t\n");
		++summary->skipped_tests;
	}

	transfer.input_ptr = (uintptr_t)input;
	transfer.output_ptr = (uintptr_t)output;
	transfer.length = probe_length;
	transfer.flags = 0U;
	transfer.reserved = 0U;

	if (caps->max_timeout_ms < UINT32_MAX) {
		transfer.timeout_ms = caps->max_timeout_ms + 1U;

		++summary->negative_tests_attempted;
		if (expect_submit_rejected(fd, "timeout-over-limit", &transfer) < 0) {
			++summary->failures;
			ret = -1;
		} else {
			++summary->negative_tests_passed;
		}
	} else {
		printf("[SKIP] reject=timeout-over-limit would overflow uint32_t\n");
		++summary->skipped_tests;
	}

	transfer.timeout_ms = 0U;
	transfer.flags = 1U;
	transfer.reserved = 0U;

	++summary->negative_tests_attempted;
	if (expect_submit_rejected(fd, "flags-nonzero", &transfer) < 0) {
		++summary->failures;
		ret = -1;
	} else {
		++summary->negative_tests_passed;
	}

	transfer.flags = 0U;
	transfer.reserved = 1U;

	++summary->negative_tests_attempted;
	if (expect_submit_rejected(fd, "reserved-nonzero", &transfer) < 0) {
		++summary->failures;
		ret = -1;
	} else {
		++summary->negative_tests_passed;
	}

out:
	free(oversize_output);
	free(oversize_input);
	free(output);
	free(input);
	return ret;
}

static int run_single_case(int fd, const struct zybo_accel_dma_caps *caps,
			   const struct cli_options *options,
			   struct suite_summary *summary)
{
	uint64_t runs_passed = 0;

	printf("Requested length : %" PRIu32 " bytes\n", options->length);
	printf("Requested count  : %" PRIu32 "\n", options->count);
	printf("Requested pattern: %s\n", pattern_name(options->pattern));
	printf("Requested timeout: %" PRIu32 " ms%s\n",
	       options->timeout_ms,
	       options->timeout_ms ? "" : " (driver default)");
	printf("Reference model  : output byte = input byte XOR 0x%02x\n",
	       XOR_BYTE_CONSTANT);

	++summary->positive_cases_attempted;

	if (run_positive_case(fd, caps, options->length, options->timeout_ms,
			      options->pattern, options->count,
			      &runs_passed) < 0) {
		++summary->failures;
		return -1;
	}

	++summary->positive_cases_passed;
	summary->positive_runs_passed += runs_passed;
	return 0;
}

/*
 * Run the broad first-stage XOR accelerator validation campaign.
 *
 * Without an explicit timeout argument, the suite covers both ABI timeout
 * forms that are currently useful:
 * - zero, meaning "use the driver default",
 * - the explicit numeric value equal to the reported driver default.
 *
 * These are distinct submit inputs even though they should produce the same
 * practical timeout behavior in the current driver.
 */
static int run_suite(int fd, const struct zybo_accel_dma_caps *caps,
		     const struct cli_options *options,
		     struct suite_summary *summary)
{
	uint32_t timeout_modes[2] = { 0U, 0U };
	size_t timeout_mode_count = 0U;
	size_t timeout_index;
	size_t size_index;
	size_t pattern_index;
	int ret = 0;

	if (options->timeout_was_set) {
		if (validate_requested_timeout(options->timeout_ms, caps) < 0) {
			fprintf(stderr,
				"error: requested suite timeout %" PRIu32
				" exceeds driver maximum %" PRIu32 "\n",
				options->timeout_ms, caps->max_timeout_ms);
			return -1;
		}

		timeout_modes[0] = options->timeout_ms;
		timeout_mode_count = 1U;
	} else {
		timeout_modes[0] = 0U;
		timeout_mode_count = 1U;

		if (caps->default_timeout_ms &&
		    caps->default_timeout_ms <= caps->max_timeout_ms) {
			timeout_modes[1] = caps->default_timeout_ms;
			timeout_mode_count = 2U;
		}
	}

	printf("XOR constant      : 0x%02x\n", XOR_BYTE_CONSTANT);
	printf("Suite repetitions : %" PRIu32 " runs per positive case\n",
	       options->count);
	printf("Suite timeout mode: %s\n",
	       options->timeout_was_set ?
	       "caller-specified timeout only" :
	       "driver default plus explicit driver-default timeout");

	for (timeout_index = 0; timeout_index < timeout_mode_count;
	     ++timeout_index) {
		for (size_index = 0;
		     size_index < sizeof(suite_sizes) / sizeof(suite_sizes[0]);
		     ++size_index) {
			uint32_t length = suite_sizes[size_index];

			if (validate_requested_length(length, caps) < 0) {
				printf("[SKIP] size=%" PRIu32
				       " outside current driver DMA limits\n",
				       length);
				++summary->skipped_tests;
				continue;
			}

			for (pattern_index = 0;
			     pattern_index < sizeof(suite_patterns) /
					     sizeof(suite_patterns[0]);
			     ++pattern_index) {
				uint64_t runs_passed = 0;
				enum dma_pattern pattern =
					suite_patterns[pattern_index].pattern;

				++summary->positive_cases_attempted;

				if (run_positive_case(fd, caps, length,
						      timeout_modes[timeout_index],
						      pattern, options->count,
						      &runs_passed) < 0) {
					++summary->failures;
					ret = -1;
					continue;
				}

				++summary->positive_cases_passed;
				summary->positive_runs_passed += runs_passed;
			}
		}
	}

	if (run_negative_tests(fd, caps, summary) < 0)
		ret = -1;

	return ret;
}

static void print_summary(const struct suite_summary *summary)
{
	printf("\nDMA XOR validation summary\n");
	printf("Positive cases attempted : %" PRIu32 "\n",
	       summary->positive_cases_attempted);
	printf("Positive cases passed    : %" PRIu32 "\n",
	       summary->positive_cases_passed);
	printf("Positive runs passed     : %" PRIu64 "\n",
	       summary->positive_runs_passed);
	printf("Negative tests attempted : %" PRIu32 "\n",
	       summary->negative_tests_attempted);
	printf("Negative tests passed    : %" PRIu32 "\n",
	       summary->negative_tests_passed);
	printf("Skipped tests            : %" PRIu32 "\n",
	       summary->skipped_tests);
	printf("Failures                 : %" PRIu32 "\n",
	       summary->failures);
}

int main(int argc, char **argv)
{
	struct cli_options options;
	struct zybo_accel_dma_caps caps = { 0 };
	struct zybo_accel_stats final_stats = { 0 };
	struct suite_summary summary = { 0 };
	int fd = -1;
	int ret = EXIT_FAILURE;

	if (parse_options(argc, argv, &options) < 0) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	fd = open(options.device_path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return die_errno("open device");

	if (ioctl(fd, ZYBO_ACCEL_IOCTL_GET_DMA_CAPS, &caps) < 0) {
		ret = die_errno("ioctl(GET_DMA_CAPS)");
		goto out;
	}

	printf("Device path      : %s\n", options.device_path);
	print_caps(&caps);
	printf("\n");

	if (options.suite_mode) {
		if (run_suite(fd, &caps, &options, &summary) < 0)
			ret = EXIT_FAILURE;
		else
			ret = EXIT_SUCCESS;
	} else {
		if (validate_requested_length(options.length, &caps) < 0) {
			fprintf(stderr,
				"error: length %" PRIu32
				" violates driver DMA limits\n",
				options.length);
			goto out;
		}

		if (validate_requested_timeout(options.timeout_ms, &caps) < 0) {
			fprintf(stderr,
				"error: timeout %" PRIu32
				" exceeds driver maximum %" PRIu32 "\n",
				options.timeout_ms, caps.max_timeout_ms);
			goto out;
		}

		if (run_single_case(fd, &caps, &options, &summary) < 0)
			ret = EXIT_FAILURE;
		else
			ret = EXIT_SUCCESS;
	}

	print_summary(&summary);

	if (get_stats(fd, &final_stats) < 0) {
		if (ret == EXIT_SUCCESS)
			ret = die_errno("ioctl(GET_STATS final)");
		else
			perror("ioctl(GET_STATS final)");
	} else {
		printf("\nFinal driver stats\n");
		print_stats(&final_stats);
	}

out:
	if (fd >= 0 && close(fd) < 0 && ret == EXIT_SUCCESS)
		ret = die_errno("close device");

	puts(ret == EXIT_SUCCESS ? "Overall result    : PASS" :
					 "Overall result    : FAIL");
	return ret;
}
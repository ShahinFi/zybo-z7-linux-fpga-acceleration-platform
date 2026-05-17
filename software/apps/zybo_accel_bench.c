#define _GNU_SOURCE
/* SPDX-License-Identifier: MIT */
/*
 * End-to-end Linux-controlled FPGA transaction benchmark for
 * /dev/zybo_accel0.
 *
 * The measured transaction path is:
 *
 *   user-space benchmark
 *   -> blocking SUBMIT ioctl
 *   -> zybo_accel kernel driver
 *   -> AXI DMA MM2S
 *   -> fixed-XOR AXI-Stream validation accelerator
 *   -> AXI DMA S2MM
 *   -> driver return to user space
 *
 * The XOR operation is not the benchmark target. It is only the deterministic
 * processing stage that proves the timed path still includes custom PL logic.
 *
 * For each transfer size, the program:
 * - verifies correctness before timing,
 * - measures repeated blocking SUBMIT ioctl calls,
 * - verifies correctness again after timing,
 * - checks driver statistics for hidden timeout or DMA-error regressions,
 * - writes one result row to terminal output and CSV.
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
#include <time.h>
#include <unistd.h>

#include "zybo_accel_uapi.h"

#define XOR_BYTE_CONSTANT        0xa5U
#define OUTPUT_SENTINEL          0xcdU
#define DEFAULT_CSV_PATH         "zybo_accel_bench_results.csv"

/*
 * Standard platform benchmark sweep.
 *
 * Small transfers need more samples because fixed transaction overhead
 * dominates their timing. Large transfers use fewer iterations so the complete
 * sweep stays practical on the board.
 */
struct benchmark_case {
	uint32_t length;
	uint32_t iterations;
};

static const struct benchmark_case standard_cases[] = {
	{        64U, 10000U },
	{       256U, 10000U },
	{      1024U,  5000U },
	{      4096U,  5000U },
	{     16384U,  2000U },
	{     65536U,  1000U },
	{    262144U,   300U },
	{   1048576U,  100U },
};

enum benchmark_mode {
	BENCHMARK_SWEEP = 0,
	BENCHMARK_SINGLE,
};

/*
 * Fully resolved command-line state.
 *
 * Sweep mode uses the fixed benchmark table above. Single mode exists for
 * targeted reruns of one transfer size without changing the standard sweep.
 */
struct cli_options {
	const char *device_path;
	const char *csv_path;
	enum benchmark_mode mode;
	uint32_t single_length;
	uint32_t single_iterations;
	uint32_t timeout_ms;
	bool sweep_requested;
	bool size_was_set;
	bool count_was_set;
};

/*
 * Result record for one transfer size.
 *
 * Latency fields cover only successful timed SUBMIT calls. Overall PASS still
 * requires every requested timed transaction to succeed and driver statistics
 * to match the requested iteration count exactly.
 */
struct benchmark_result {
	uint32_t length;
	uint32_t requested_iterations;
	uint32_t successful_iterations;
	uint32_t failed_iterations;

	uint64_t total_payload_bytes;
	uint64_t wall_time_ns;
	uint64_t cpu_time_ns;

	uint64_t latency_sum_ns;
	uint64_t latency_min_ns;
	uint64_t latency_max_ns;

	uint64_t submit_delta;
	uint64_t complete_delta;
	uint64_t timeout_delta;
	uint64_t error_delta;

	bool precheck_passed;
	bool postcheck_passed;
	bool passed;
};

static int die_errno(const char *what)
{
	fprintf(stderr, "error: %s: %s\n", what, strerror(errno));
	return EXIT_FAILURE;
}

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

static uint64_t timespec_to_ns(const struct timespec *value)
{
	return (uint64_t)value->tv_sec * 1000000000ULL +
	       (uint64_t)value->tv_nsec;
}

static uint64_t elapsed_ns(const struct timespec *start,
			   const struct timespec *end)
{
	return timespec_to_ns(end) - timespec_to_ns(start);
}

static double ns_to_us(uint64_t value)
{
	return (double)value / 1000.0;
}

static double ns_to_ms(uint64_t value)
{
	return (double)value / 1000000.0;
}

static double throughput_mib_per_s(uint64_t bytes, uint64_t ns)
{
	double seconds;

	if (!ns)
		return 0.0;

	seconds = (double)ns / 1000000000.0;
	return ((double)bytes / (1024.0 * 1024.0)) / seconds;
}

static double cpu_usage_percent(uint64_t cpu_ns, uint64_t wall_ns)
{
	if (!wall_ns)
		return 0.0;

	return ((double)cpu_ns / (double)wall_ns) * 100.0;
}

static void print_usage(const char *program)
{
	printf("usage:\n");
	printf("  %s [--sweep] [options]\n", program);
	printf("  %s --size BYTES --count N [options]\n", program);
	printf("\n");
	printf("modes:\n");
	printf("  --sweep              run the standard platform benchmark sweep\n");
	printf("                       this is the default mode when no size is given\n");
	printf("  --size BYTES         run one selected transfer size\n");
	printf("  --count N            transaction count for single-size mode\n");
	printf("\n");
	printf("options:\n");
	printf("  -d, --device PATH    device path, default /dev/%s\n",
	       ZYBO_ACCEL_DEVICE_NAME);
	printf("  -t, --timeout MS     DMA timeout, default 0 for driver default\n");
	printf("  -c, --csv PATH       CSV output file, default %s\n",
	       DEFAULT_CSV_PATH);
	printf("  -h, --help           show this help\n");
}

static int parse_options(int argc, char **argv, struct cli_options *options)
{
	static const struct option long_options[] = {
		{ "sweep",   no_argument,       NULL, 'S' },
		{ "size",    required_argument, NULL, 's' },
		{ "count",   required_argument, NULL, 'n' },
		{ "device",  required_argument, NULL, 'd' },
		{ "timeout", required_argument, NULL, 't' },
		{ "csv",     required_argument, NULL, 'c' },
		{ "help",    no_argument,       NULL, 'h' },
		{ NULL,      0,                 NULL,  0  },
	};
	int opt;

	options->device_path = "/dev/" ZYBO_ACCEL_DEVICE_NAME;
	options->csv_path = DEFAULT_CSV_PATH;
	options->mode = BENCHMARK_SWEEP;
	options->single_length = 0U;
	options->single_iterations = 0U;
	options->timeout_ms = 0U;
	options->sweep_requested = false;
	options->size_was_set = false;
	options->count_was_set = false;

	while ((opt = getopt_long(argc, argv, "Ss:n:d:t:c:h",
				  long_options, NULL)) != -1) {
		switch (opt) {
		case 'S':
			options->sweep_requested = true;
			break;

		case 's':
			if (parse_u32(optarg, &options->single_length) < 0) {
				fprintf(stderr, "error: invalid size '%s'\n", optarg);
				return -1;
			}
			options->size_was_set = true;
			break;

		case 'n':
			if (parse_u32(optarg, &options->single_iterations) < 0 ||
			    !options->single_iterations) {
				fprintf(stderr, "error: invalid count '%s'\n", optarg);
				return -1;
			}
			options->count_was_set = true;
			break;

		case 'd':
			options->device_path = optarg;
			break;

		case 't':
			if (parse_u32(optarg, &options->timeout_ms) < 0) {
				fprintf(stderr, "error: invalid timeout '%s'\n", optarg);
				return -1;
			}
			break;

		case 'c':
			options->csv_path = optarg;
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

	if (options->sweep_requested &&
	    (options->size_was_set || options->count_was_set)) {
		fprintf(stderr,
			"error: --sweep cannot be combined with --size or --count\n");
		return -1;
	}

	if (options->size_was_set != options->count_was_set) {
		fprintf(stderr,
			"error: --size and --count must be provided together\n");
		return -1;
	}

	if (options->size_was_set)
		options->mode = BENCHMARK_SINGLE;
	else
		options->mode = BENCHMARK_SWEEP;

	return 0;
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

/*
 * Benchmark input must stay inside the driver's published DMA capability
 * contract. Invalid custom single-size runs are rejected before any timing.
 */
static int validate_length(uint32_t length,
			   const struct zybo_accel_dma_caps *caps)
{
	if (!length || length > caps->max_transfer_bytes)
		return -1;

	if (caps->transfer_alignment_bytes &&
	    length % caps->transfer_alignment_bytes)
		return -1;

	return 0;
}

static int validate_timeout(uint32_t timeout_ms,
			    const struct zybo_accel_dma_caps *caps)
{
	if (timeout_ms > caps->max_timeout_ms)
		return -1;

	return 0;
}

static int get_stats(int fd, struct zybo_accel_stats *stats)
{
	memset(stats, 0, sizeof(*stats));

	if (ioctl(fd, ZYBO_ACCEL_IOCTL_GET_STATS, stats) < 0)
		return -1;

	return 0;
}

/*
 * Use a deterministic nontrivial payload for benchmark verification.
 *
 * Timed iterations reuse one filled buffer so the reported timing measures the
 * accelerator transaction path, not user-space pattern generation.
 */
static void fill_input_pattern(uint8_t *buffer, uint32_t length,
			       uint32_t seed)
{
	uint32_t i;

	for (i = 0; i < length; ++i)
		buffer[i] = (uint8_t)((i * 37U + 11U + seed * 17U) & 0xffU);
}

/*
 * The current validation accelerator XORs every payload byte with 0xA5.
 * Correctness checks use this fixed reference before and after each timed
 * benchmark section.
 */
static bool verify_xor_output(const uint8_t *input,
			      const uint8_t *output,
			      uint32_t length)
{
	uint32_t i;

	for (i = 0; i < length; ++i) {
		uint8_t expected = input[i] ^ XOR_BYTE_CONSTANT;

		if (output[i] != expected) {
			fprintf(stderr,
				"mismatch at byte %" PRIu32
				": input 0x%02x, expected 0x%02x, got 0x%02x\n",
				i, input[i], expected, output[i]);
			return false;
		}
	}

	return true;
}

/*
 * Submit one complete blocking accelerator transaction through the public ABI.
 * The caller owns user-space buffers; the driver owns all DMA-safe staging.
 */
static int submit_once(int fd, uint8_t *input, uint8_t *output,
		       uint32_t length, uint32_t timeout_ms)
{
	struct zybo_accel_transfer transfer = { 0 };

	transfer.input_ptr = (uintptr_t)input;
	transfer.output_ptr = (uintptr_t)output;
	transfer.length = length;
	transfer.timeout_ms = timeout_ms;
	transfer.flags = 0U;
	transfer.reserved = 0U;

	return ioctl(fd, ZYBO_ACCEL_IOCTL_SUBMIT, &transfer);
}

/*
 * Prove that one transfer size still returns the expected transformed payload.
 * These checks are intentionally outside the timed interval so full-byte output
 * comparison does not distort benchmark latency and throughput results.
 */
static bool run_correctness_check(int fd, uint8_t *input, uint8_t *output,
				  uint32_t length, uint32_t timeout_ms,
				  uint32_t seed, const char *stage)
{
	fill_input_pattern(input, length, seed);
	memset(output, OUTPUT_SENTINEL, length);

	if (submit_once(fd, input, output, length, timeout_ms) < 0) {
		fprintf(stderr,
			"[FAIL] %s correctness submit failed for size=%" PRIu32
			": %s\n",
			stage, length, strerror(errno));
		return false;
	}

	if (!verify_xor_output(input, output, length)) {
		fprintf(stderr,
			"[FAIL] %s correctness output mismatch for size=%" PRIu32 "\n",
			stage, length);
		return false;
	}

	return true;
}

static void initialize_result(struct benchmark_result *result,
			      uint32_t length, uint32_t iterations)
{
	memset(result, 0, sizeof(*result));

	result->length = length;
	result->requested_iterations = iterations;
	result->latency_min_ns = UINT64_MAX;
}

/*
 * Benchmark one transfer size.
 *
 * The wall-time interval covers the complete timed repetition block. Per-run
 * latency covers each successful blocking SUBMIT ioctl individually. Process
 * CPU time is measured over the same repetition block as wall time.
 *
 * Driver statistics are read before and after the timed block. A benchmark row
 * passes only when accepted/completed transaction counts match the requested
 * iteration count and no timeout or DMA-error counters increase.
 */
static int run_benchmark_case(int fd,
			      const struct zybo_accel_dma_caps *caps,
			      uint32_t length,
			      uint32_t iterations,
			      uint32_t timeout_ms,
			      struct benchmark_result *result)
{
	struct zybo_accel_stats before = { 0 };
	struct zybo_accel_stats after = { 0 };
	struct timespec wall_start = { 0 };
	struct timespec wall_end = { 0 };
	struct timespec cpu_start = { 0 };
	struct timespec cpu_end = { 0 };
	uint8_t *input = NULL;
	uint8_t *output = NULL;
	uint32_t run;
	int ret = -1;

	initialize_result(result, length, iterations);

	if (validate_length(length, caps) < 0) {
		fprintf(stderr,
			"error: benchmark size %" PRIu32
			" violates current DMA limits\n",
			length);
		return -1;
	}

	input = malloc(length);
	output = malloc(length);
	if (!input || !output) {
		fprintf(stderr,
			"error: failed to allocate %" PRIu32 "-byte benchmark buffers\n",
			length);
		goto out;
	}

	result->precheck_passed =
		run_correctness_check(fd, input, output, length,
				      timeout_ms, 1U, "pre");

	if (!result->precheck_passed)
		goto out;

	/*
	 * Timed iterations reuse a stable payload so user-space buffer generation
	 * is not included in the transaction-path measurement.
	 */
	fill_input_pattern(input, length, 100U);
	memset(output, OUTPUT_SENTINEL, length);

	if (get_stats(fd, &before) < 0) {
		perror("ioctl(GET_STATS before benchmark)");
		goto out;
	}

	if (clock_gettime(CLOCK_MONOTONIC, &wall_start) < 0) {
		perror("clock_gettime(CLOCK_MONOTONIC)");
		goto out;
	}

	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_start) < 0) {
		perror("clock_gettime(CLOCK_PROCESS_CPUTIME_ID)");
		goto out;
	}

	for (run = 0; run < iterations; ++run) {
		struct timespec start = { 0 };
		struct timespec end = { 0 };
		uint64_t latency_ns;

		if (clock_gettime(CLOCK_MONOTONIC, &start) < 0) {
			perror("clock_gettime(CLOCK_MONOTONIC)");
			goto out;
		}

		if (submit_once(fd, input, output, length, timeout_ms) < 0) {
			++result->failed_iterations;
			continue;
		}

		if (clock_gettime(CLOCK_MONOTONIC, &end) < 0) {
			perror("clock_gettime(CLOCK_MONOTONIC)");
			goto out;
		}

		latency_ns = elapsed_ns(&start, &end);

		++result->successful_iterations;
		result->latency_sum_ns += latency_ns;

		if (latency_ns < result->latency_min_ns)
			result->latency_min_ns = latency_ns;

		if (latency_ns > result->latency_max_ns)
			result->latency_max_ns = latency_ns;
	}

	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_end) < 0) {
		perror("clock_gettime(CLOCK_PROCESS_CPUTIME_ID)");
		goto out;
	}

	if (clock_gettime(CLOCK_MONOTONIC, &wall_end) < 0) {
		perror("clock_gettime(CLOCK_MONOTONIC)");
		goto out;
	}

	result->wall_time_ns = elapsed_ns(&wall_start, &wall_end);
	result->cpu_time_ns = elapsed_ns(&cpu_start, &cpu_end);
	result->total_payload_bytes =
		(uint64_t)length * (uint64_t)result->successful_iterations;

	if (!result->successful_iterations)
		result->latency_min_ns = 0U;

	if (get_stats(fd, &after) < 0) {
		perror("ioctl(GET_STATS after benchmark)");
		goto out;
	}

	result->submit_delta = after.submit_count - before.submit_count;
	result->complete_delta = after.complete_count - before.complete_count;
	result->timeout_delta = after.timeout_count - before.timeout_count;
	result->error_delta = after.error_count - before.error_count;

	result->postcheck_passed =
		run_correctness_check(fd, input, output, length,
				      timeout_ms, 2U, "post");

	result->passed =
		result->precheck_passed &&
		result->postcheck_passed &&
		result->successful_iterations == iterations &&
		result->failed_iterations == 0U &&
		result->submit_delta == iterations &&
		result->complete_delta == iterations &&
		result->timeout_delta == 0U &&
		result->error_delta == 0U;

	ret = result->passed ? 0 : -1;

out:
	free(output);
	free(input);
	return ret;
}

static double average_latency_us(const struct benchmark_result *result)
{
	if (!result->successful_iterations)
		return 0.0;

	return ns_to_us(result->latency_sum_ns /
			(uint64_t)result->successful_iterations);
}

static void print_result_header(void)
{
	printf("\n");
	printf("%-10s %-8s %-12s %-12s %-12s %-12s %-10s %-8s %-8s %-8s %-8s\n",
	       "Size", "Runs", "Avg us", "Min us", "Max us",
	       "MiB/s", "CPU %", "Fail", "Tmo", "Err", "Result");
}

static void print_result_row(const struct benchmark_result *result)
{
	printf("%-10" PRIu32 " %-8" PRIu32 " %-12.3f %-12.3f %-12.3f "
	       "%-12.3f %-10.2f %-8" PRIu32 " %-8" PRIu64 " %-8" PRIu64 " %-8s\n",
	       result->length,
	       result->requested_iterations,
	       average_latency_us(result),
	       ns_to_us(result->latency_min_ns),
	       ns_to_us(result->latency_max_ns),
	       throughput_mib_per_s(result->total_payload_bytes,
				    result->wall_time_ns),
	       cpu_usage_percent(result->cpu_time_ns,
				 result->wall_time_ns),
	       result->failed_iterations,
	       result->timeout_delta,
	       result->error_delta,
	       result->passed ? "PASS" : "FAIL");
}

/*
 * CSV columns keep raw counters and derived values together so benchmark
 * results can be reviewed without parsing terminal tables.
 */
static int write_csv_header(FILE *csv)
{
	if (fprintf(csv,
		    "size_bytes,requested_iterations,successful_iterations,"
		    "failed_iterations,total_payload_bytes,wall_time_ms,"
		    "cpu_time_ms,avg_latency_us,min_latency_us,max_latency_us,"
		    "throughput_mib_s,process_cpu_usage_percent,"
		    "submit_delta,complete_delta,timeout_delta,error_delta,"
		    "precheck_passed,postcheck_passed,result\n") < 0)
		return -1;

	return 0;
}

static int write_csv_row(FILE *csv, const struct benchmark_result *result)
{
	if (fprintf(csv,
		    "%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ","
		    "%" PRIu64 ",%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
		    "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
		    "%s,%s,%s\n",
		    result->length,
		    result->requested_iterations,
		    result->successful_iterations,
		    result->failed_iterations,
		    result->total_payload_bytes,
		    ns_to_ms(result->wall_time_ns),
		    ns_to_ms(result->cpu_time_ns),
		    average_latency_us(result),
		    ns_to_us(result->latency_min_ns),
		    ns_to_us(result->latency_max_ns),
		    throughput_mib_per_s(result->total_payload_bytes,
					 result->wall_time_ns),
		    cpu_usage_percent(result->cpu_time_ns,
				      result->wall_time_ns),
		    result->submit_delta,
		    result->complete_delta,
		    result->timeout_delta,
		    result->error_delta,
		    result->precheck_passed ? "yes" : "no",
		    result->postcheck_passed ? "yes" : "no",
		    result->passed ? "PASS" : "FAIL") < 0)
		return -1;

	return 0;
}

static int run_sweep(int fd,
		     const struct zybo_accel_dma_caps *caps,
		     uint32_t timeout_ms,
		     FILE *csv)
{
	size_t i;
	int overall_ret = 0;

	print_result_header();

	for (i = 0; i < sizeof(standard_cases) / sizeof(standard_cases[0]); ++i) {
		struct benchmark_result result;
		const struct benchmark_case *test = &standard_cases[i];

		if (run_benchmark_case(fd, caps, test->length,
				       test->iterations, timeout_ms,
				       &result) < 0)
			overall_ret = -1;

		print_result_row(&result);

		if (write_csv_row(csv, &result) < 0) {
			perror("write CSV row");
			return -1;
		}
	}

	return overall_ret;
}

static int run_single(int fd,
		      const struct zybo_accel_dma_caps *caps,
		      uint32_t length,
		      uint32_t iterations,
		      uint32_t timeout_ms,
		      FILE *csv)
{
	struct benchmark_result result;
	int ret;

	print_result_header();

	ret = run_benchmark_case(fd, caps, length, iterations,
				 timeout_ms, &result);

	print_result_row(&result);

	if (write_csv_row(csv, &result) < 0) {
		perror("write CSV row");
		return -1;
	}

	return ret;
}

int main(int argc, char **argv)
{
	struct cli_options options;
	struct zybo_accel_dma_caps caps = { 0 };
	FILE *csv = NULL;
	int fd = -1;
	int benchmark_ret;
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

	if (validate_timeout(options.timeout_ms, &caps) < 0) {
		fprintf(stderr,
			"error: timeout %" PRIu32
			" exceeds driver maximum %" PRIu32 "\n",
			options.timeout_ms, caps.max_timeout_ms);
		goto out;
	}

	csv = fopen(options.csv_path, "w");
	if (!csv) {
		ret = die_errno("open CSV output");
		goto out;
	}

	if (write_csv_header(csv) < 0) {
		ret = die_errno("write CSV header");
		goto out;
	}

	printf("Device path      : %s\n", options.device_path);
	print_caps(&caps);
	printf("Benchmark mode   : %s\n",
	       options.mode == BENCHMARK_SWEEP ? "standard sweep" : "single size");
	printf("Requested timeout: %" PRIu32 " ms%s\n",
	       options.timeout_ms,
	       options.timeout_ms ? "" : " (driver default)");
	printf("CSV output       : %s\n", options.csv_path);
	printf("Measured interval: blocking SUBMIT ioctl only\n");
	printf("Correctness      : XOR pre-check and post-check per size\n");

	if (options.mode == BENCHMARK_SWEEP) {
		benchmark_ret = run_sweep(fd, &caps,
					  options.timeout_ms, csv);
	} else {
		benchmark_ret = run_single(fd, &caps,
					   options.single_length,
					   options.single_iterations,
					   options.timeout_ms,
					   csv);
	}

	if (fflush(csv) < 0) {
		ret = die_errno("flush CSV output");
		goto out;
	}

	ret = benchmark_ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

out:
	if (csv && fclose(csv) < 0 && ret == EXIT_SUCCESS)
		ret = die_errno("close CSV output");

	if (fd >= 0 && close(fd) < 0 && ret == EXIT_SUCCESS)
		ret = die_errno("close device");

	puts(ret == EXIT_SUCCESS ? "\nOverall result    : PASS" :
					 "\nOverall result    : FAIL");
	return ret;
}

# SPDX-License-Identifier: MIT
# User-space build file for Zybo accelerator validation utilities.

CC ?= gcc
CPPFLAGS ?=
CPPFLAGS += -I../../linux/driver
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
LDFLAGS ?=
TARGETS := zybo_accel_reg_test zybo_accel_dma_test zybo_accel_bench

.PHONY: all clean

all: $(TARGETS)

zybo_accel_reg_test: zybo_accel_reg_test.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS)

zybo_accel_dma_test: zybo_accel_dma_test.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS)

zybo_accel_bench: zybo_accel_bench.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)

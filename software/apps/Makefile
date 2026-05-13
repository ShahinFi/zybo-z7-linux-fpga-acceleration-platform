# SPDX-License-Identifier: MIT
# Simple user-space build file for the register-test utility.

CC ?= gcc
CPPFLAGS ?=
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11
LDFLAGS ?=
TARGET := zybo_accel_reg_test
SRC := zybo_accel_reg_test.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

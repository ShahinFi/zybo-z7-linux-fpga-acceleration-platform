# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree kernel-module build file for the first Zybo accelerator driver.
#
# Typical usage in a prepared PetaLinux SDK or kernel build environment:
#   make KDIR=/path/to/kernel/build
#
# The module output will be zybo_accel.ko.

obj-m += zybo_accel.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

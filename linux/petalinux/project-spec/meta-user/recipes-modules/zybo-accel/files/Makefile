obj-m += zybo-accel.o
zybo-accel-y := zybo_accel.o

MODULE_KERNEL_DIR := $(KERNEL_SRC)

ifeq ($(strip $(MODULE_KERNEL_DIR)),)
MODULE_KERNEL_DIR := /lib/modules/$(shell uname -r)/build
endif

MODULE_SRC_DIR := $(shell pwd)

all:
	$(MAKE) -C $(MODULE_KERNEL_DIR) M=$(MODULE_SRC_DIR) modules

modules_install:
	$(MAKE) -C $(MODULE_KERNEL_DIR) M=$(MODULE_SRC_DIR) modules_install

clean:
	$(MAKE) -C $(MODULE_KERNEL_DIR) M=$(MODULE_SRC_DIR) clean

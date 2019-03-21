# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Linux Bluetooth HCI device drivers.
#

SHELL := /bin/sh
CC = gcc
KVER  ?= $(shell uname -r)
KSRC := /lib/modules/$(KVER)/build
FIRMWAREDIR := /lib/firmware/
PWD := $(shell pwd)
CLR_MODULE_FILES := *.mod.c *.mod *.o .*.cmd *.ko *~ .tmp_versions* modules.order Module.symvers
SYMBOL_FILE := Module.symvers
MODDESTDIR := /lib/modules/$(KVER)/kernel/drivers/bluetooth
#Handle the compression option for modules in 3.18+
ifneq ("","$(wildcard $(MODDESTDIR)/*.ko.gz)")
COMPRESS_GZIP := y
endif
ifneq ("","$(wildcard $(MODDESTDIR)/*.ko.xz)")
COMPRESS_XZ := y
endif

EXTRA_CFLAGS += -O2

obj-m	+= usbbt.o

usbbt-objs += btusb.o btrtl.o

ccflags-y += -D__CHECK_ENDIAN__

all: 
	$(MAKE) -C $(KSRC) M=$(PWD) modules
install: all
	@mkdir -p $(MODDESTDIR)
	@install -p -D -m 644 usbbt.ko $(MODDESTDIR)	
ifeq ($(COMPRESS_GZIP), y)
	@gzip -f $(MODDESTDIR)/*.ko
endif
ifeq ($(COMPRESS_XZ), y)
	@xz -f $(MODDESTDIR)/*.ko
endif

	@depmod -a $(KVER)

	@#copy firmware images to target folder
	@mkdir -p $(FIRMWAREDIR)/rtl_bt/
	@cp -f firmware/*.bin $(FIRMWAREDIR)/rtl_bt/
	@echo "Install usbbt SUCCESS"

uninstall:
clean:
	@rm -fr *.mod.c *.mod *.o .*.cmd *.ko *~ .*.o.d .cache.mk
	@rm -fr .tmp_versions
	@rm -fr Modules.symvers
	@rm -fr Module.symvers
	@rm -fr Module.markers
	@rm -fr modules.order

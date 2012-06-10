TARGET = blec_usb.ko
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

obj-m += blec_usb.o

default:
		$(MAKE) -C $(KERNELDIR) SUBDIRS=$(PWD) modules

clean:
		$(MAKE) -C $(KERNELDIR) SUBDIRS=$(PWD) clean


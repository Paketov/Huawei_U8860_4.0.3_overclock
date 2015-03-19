KERNEL_DIR=/home/andrew/kernel/
obj-m := setfreg.o
PWD := $(shell pwd)
default:
	$(MAKE) ARCH=arm CROSS_COMPILE=arm-linux-gnueabi- -C $(KERNEL_DIR) SUBDIRS=$(PWD) modules
clean:
	$(MAKE) -C $(KERNEL_DIR) SUBDIRS=$(PWD) clean


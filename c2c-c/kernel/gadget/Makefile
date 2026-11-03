# out-of-tree module, build it on the Jetson itself
obj-m := usb_f_c2c.o
ccflags-y := -I$(src)/../../common

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

obj-m += trusty_probe.o
KDIR ?= /home/simon/pmbootstrap/linux-7.2-gs101
PWD  := $(shell pwd)
all:
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=1 modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=1 clean

CFLAGS_cryptrd.o = -DDEBUG

obj-m += cryptrd.o

KDIR ?= /lib/modules/$(shell uname -r)/build
# KDIR ?=../linux
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

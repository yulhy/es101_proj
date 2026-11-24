CC = gcc
obj-m := guard.o
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
# 기존 ES-101 예제는 SUBDIRS 사용. 최신 커널에서도 동작하도록 M을 함께 지정.
.PHONY: default clean
default: guard_ctl
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) M=$(PWD) modules

guard_ctl: guard_ctl.c
	$(CC) -Wall -Wextra -O2 -pthread -o $@ $<

clean:
	$(MAKE) -C $(KDIR) SUBDIRS=$(PWD) M=$(PWD) clean
	rm -f guard_ctl

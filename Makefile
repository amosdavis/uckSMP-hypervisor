# Makefile for UCK (Unified Compute Kernel) module + userspace tools
#
# Build against the running kernel:
#   make
#
# Build against a specific kernel:
#   make KDIR=/path/to/kernel/source
#
# Build userspace tools:
#   make tools
#
# Install into a node image:
#   make install DESTDIR=/mnt/node1

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
CC ?= gcc

.PHONY: all clean install tools test

all: tools
	$(MAKE) -C $(KDIR) M=$(PWD) modules

tools: uckd uck_test uckctl uck_restore uck_run uck_smp

uckd: uckd.c uck.h
	$(CC) -Wall -O2 -o uckd uckd.c

uck_test: uck_test.c uck.h
	$(CC) -Wall -O2 -o uck_test uck_test.c

uckctl: uckctl.c uck.h
	$(CC) -Wall -O2 -o uckctl uckctl.c

uck_restore: uck_restore.c uck.h
	$(CC) -Wall -O2 -o uck_restore uck_restore.c

uck_run: uck_run.c uck.h
	$(CC) -Wall -O2 -o uck_run uck_run.c

uck_smp: uck_smp.c uck.h
	$(CC) -Wall -O2 -o uck_smp uck_smp.c

test:
	$(MAKE) -C tests test

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f uckd uck_test uckctl uck_restore uck_run uck_smp

install:
ifdef DESTDIR
	install -D -m 644 uck.ko $(DESTDIR)/lib/modules/6.1.0-42-amd64/extra/uck.ko
	install -D -m 755 uckd $(DESTDIR)/usr/sbin/uckd
	install -D -m 755 uck_test $(DESTDIR)/usr/sbin/uck_test
	install -D -m 755 uckctl $(DESTDIR)/usr/sbin/uckctl
	install -D -m 755 uck_restore $(DESTDIR)/usr/sbin/uck_restore
	install -D -m 755 uck_run $(DESTDIR)/usr/sbin/uck_run
	install -D -m 755 uck_smp $(DESTDIR)/usr/sbin/uck_smp
else
	$(error DESTDIR is not set. Usage: make install DESTDIR=/mnt/node1)
endif

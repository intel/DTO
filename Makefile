# Copyright (C) 2023 Intel Corporation
#
# SPDX-License-Identifier: MIT

all: libdto dtoctl dto-test-wodto man

.PHONY: man install-man

DML_LIB_CXX=-D_GNU_SOURCE

# Path to libdto.so that dtoctl prepends to LD_PRELOAD (matches "make install").
DTOCTL_LIBPATH ?= /usr/lib64/libdto.so
PANDOC ?= pandoc
MANDIR ?= /usr/share/man

libdto: dto.c
	gcc -shared -fPIC -Wl,-soname,libdto.so dto.c $(DML_LIB_CXX) -DDTO_STATS_SUPPORT -DDTO_ACCEL_CONFIG_SUPPORT -DDTO_NUMA_SUPPORT -o libdto.so.1.0 -laccel-config -ldl -lnuma -mwaitpkg

libdto_nostats: dto.c
	gcc -shared -fPIC -Wl,-soname,libdto.so dto.c $(DML_LIB_CXX) -DDTO_ACCEL_CONFIG_SUPPORT -DDTO_NUMA_SUPPORT -o libdto.so.1.0 -laccel-config -ldl -lnuma -mwaitpkg

dtoctl: dtoctl.c
	gcc -O2 -Wall dtoctl.c -DLIBDTO_PATH=\"$(DTOCTL_LIBPATH)\" -o dtoctl

man: doc/dtoctl.1

doc/dtoctl.1: doc/dtoctl.md
	@if command -v "$(PANDOC)" >/dev/null 2>&1; then \
		"$(PANDOC)" --standalone --to=man $< -o $@; \
	else \
		echo "Skipping dtoctl manual generation: install pandoc to build the man page."; \
	fi

install: install-man
	cp libdto.so.1.0 /usr/lib64/
	ln -sf /usr/lib64/libdto.so.1.0 /usr/lib64/libdto.so.1
	ln -sf /usr/lib64/libdto.so.1.0 /usr/lib64/libdto.so
	cp dtoctl /usr/bin/

install-man: man
	@if [ -f doc/dtoctl.1 ]; then \
		install -D -m 644 doc/dtoctl.1 "$(MANDIR)/man1/dtoctl.1"; \
	else \
		echo "Skipping dtoctl manual installation: no generated man page is available."; \
	fi

install-local:
	ln -sf ./libdto.so.1.0 ./libdto.so.1
	ln -sf ./libdto.so.1.0 ./libdto.so

dto-test: dto-test.c
	gcc -g dto-test.c $(DML_LIB_CXX) -o dto-test -ldto -lpthread

dto-test-wodto: dto-test.c
	gcc -g dto-test.c $(DML_LIB_CXX) -o dto-test-wodto -lpthread

clean:
	rm -rf *.o *.so dto-test dtoctl
	rm -f doc/dtoctl.1

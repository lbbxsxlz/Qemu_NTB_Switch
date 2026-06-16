CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra
LDFLAGS ?=

TARGETS := ntb_profiling

.PHONY: all clean

all: $(TARGETS)

ntb_profiling: ntb_profiling.c ntb_generic.c ntb_utils.c ntb_profiling.h ntb_utils.h ntb_sisci.c ntb_sisci.h
	$(CC) $(CFLAGS) -std=c11 -o $@ ntb_profiling.c ntb_generic.c ntb_utils.c ntb_sisci.c $(LDFLAGS)

clean:
	rm -f $(TARGETS)
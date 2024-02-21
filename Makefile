CC=gcc
CCFLAGS=-pthread -Wall -O0 -luring -g

.PHONY: all clean
all: bin/multiclient bin/simple bin/client bin/uring bin/uring2 bin/epoll_single bin/epoll_sharded bin/io_uring_sharded bin/io_uring_multishot bin/x710 bin/i10e
clean:
	rm -f bin/multiclient bin/simple bin/client bin/uring bin/uring2 bin/epoll_single bin/epoll_sharded bin/io_uring_sharded bin/io_uring_multishot bin/x710 bin/i10e

bin/multiclient: multiclient.c dsync.c
	$(CC) $(CCFLAGS) $^ -Lhiredis -l:libhiredis.a -o $@

bin/%: %.c
	$(CC) $^ -o $@ $(CCFLAGS)

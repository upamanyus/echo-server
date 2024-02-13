CC=gcc
CCFLAGS=-pthread -Wall -O2 -luring

.PHONY: all clean
all: bin/multiclient bin/simple bin/client bin/uring bin/uring2 bin/epoll_single bin/epoll_sharded bin/io_uring_sharded bin/io_uring_multishot
clean:
	rm bin/multiclient bin/simple bin/client bin/uring bin/uring2 bin/epoll_single bin/epoll_sharded bin/io_uring_sharded bin/io_uring_multishot

bin/multiclient: multiclient.c dsync.c
	$(CC) $(CCFLAGS) $^ -Lhiredis -l:libhiredis.a -o $@

bin/%: %.c
	$(CC) $^ -o $@ $(CCFLAGS)

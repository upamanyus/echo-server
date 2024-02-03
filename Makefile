CC=gcc
CCFLAGS=-pthread -Wall -O2

.PHONY: all clean
all: bin/multiclient bin/simple bin/client bin/uring bin/uring2 bin/sem_trywait_scalability
clean:
	rm bin/multiclient bin/simple bin/client bin/uring bin/uring2 bin/sem_trywait_scalability

bin/multiclient: multiclient.c dsem.c
	$(CC) $(CCFLAGS) $^ -Lhiredis -l:libhiredis.a -o $@

bin/simple: simple.c
	$(CC) $(CCFLAGS) $^ -o $@

bin/client: client.c
	$(CC) $(CCFLAGS) $^ -o $@

bin/uring: uring.c
	$(CC) $(CCFLAGS) $^ -o $@ -luring

bin/uring2: uring2.c
	$(CC) $(CCFLAGS) $^ -o $@ -luring

bin/sem_trywait_scalability: sem_trywait_scalability.c
	$(CC) $(CCFLAGS) $^ -o $@

CC=gcc

.PHONY: all
all: multiclient simple client

multiclient: multiclient.c dsem.c
	$(CC) -g -Wall $< -Lhiredis -l:libhiredis.a -o $@

simple: simple.c
	$(CC) -Wall $< -o $@

client: client.c
	$(CC) -Wall $< -o $@

uring: uring.c
	$(CC) -Wall $< -o $@ -luring

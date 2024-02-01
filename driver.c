#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/ip.h>
#include <pthread.h>
#include <semaphore.h>
#include "hiredis/hiredis.h"

#define BUF_SIZE 1024
// #define DEBUG_ONE_OP

typedef struct {
    int N;
    const char* address;
    const char* port;
    int warmup_sec;
    int measure_sec;
} bench_params_t;
static bench_params_t params;

struct client_thread_args {
    uint64_t num_ops;
    uint64_t runtime;
};

static redisContext *redis_ctx;

void load_bench_params() {
    redisReply *reply = redisCommand(redis_ctx, "MGET numthreads address port warmup_sec measure_sec");
    if (reply == NULL) {
        printf("Error: %s\n", redis_ctx->errstr);
        exit(-1);
    }
    if (reply->type != REDIS_REPLY_ARRAY) {
        printf("Got unexpected reply type: %d\n", reply->type);
        exit(-1);
    }

    // make sure the element[j]'s are strings
    for (int i = 0; i < 5; i++) {
        if (reply->element[i]->type != REDIS_REPLY_STRING) {
            printf("Got unexpected reply type: %d\n"
                   "Make sure the redis instance is configured for this benchmark\n", reply->element[i]->type);
            exit(-1);
        }
    }
    params.N = atoi(reply->element[0]->str);
    params.address = strdup(reply->element[1]->str);
    params.port = strdup(reply->element[2]->str);
    params.warmup_sec = atoi(reply->element[3]->str);
    params.measure_sec = atoi(reply->element[4]->str);

    freeReplyObject(reply);
}

// n is the amount by which we increase
void dsem_post(const char *a, int n) {
    void *reply = redisCommand(redis_ctx, "INCRBY \"%s\" %d", a, n);
    if (reply == NULL) {
        printf("Error: %s\n", redis_ctx->errstr);
        exit(-1);
    }
    freeReplyObject(reply);
}

// n is the amount by which we decrease, but only at once.
void dsem_wait(const char *a, int n) {
    for (;;) {
        redisReply *reply = redisCommand(redis_ctx, "GET %s", a);
        if (reply == NULL) {
            printf("Error: %s\n", redis_ctx->errstr);
            exit(-1);
        }
        if (reply->type != REDIS_REPLY_STRING) {
            printf("Got unexpected reply type: %d\n", reply->type);
            exit(-1);
        }
        uint64_t x = strtol(reply->str, NULL, 10);
        if (x >= n) {
            printf("Wait done");
            exit(-1);
        }
    }
    exit(-1);
}

void bench(const char* redis_addr) {
    redis_ctx = redisConnect(redis_addr, 6379);
    if (redis_ctx == NULL || redis_ctx->err) {
        if (redis_ctx) {
            printf("Error: %s\n", redis_ctx->errstr);
            // handle error
        } else {
            printf("Can't allocate redis context\n");
        }
        puts("here");
        exit(-1);
    }
    load_bench_params();

    sem_init(&started, 0, 0);
    sem_init(&should_measure, 0, 0);
    sem_init(&should_report, 0, 0);
    sem_init(&reported, 0, 0);

    pthread_t tid;

    struct client_thread_args args[params.N];
    memset(args, 0, sizeof(args));

    for (int i = 0; i < params.N; i++) {
        pthread_create(&tid, NULL, &start_client, &args[i]);
    }

    // Wait for N threads to start running.
    for (int i = 0; i < params.N; i++) {
        sem_wait(&started);
    }

    puts("all threads started");
    dsem_post("started", 1);

    // After hearing "should_measure", tell the N threads to start measuring.
    dsem_wait("should_measure", 1);
    for (int i = 0; i < params.N; i++) {
        sem_post(&should_measure);
    }

    puts("measuring");

    // After hearing "should_report", tell the N threads to report in.
    dsem_wait("should_report", 1);
    for (int i = 0; i < params.N; i++) {
        sem_post(&should_report);
    }

    // Wait for N threads to report.
    for (int i = 0; i < params.N; i++) {
        sem_wait(&reported);
    }

    uint64_t num_ops = 0, runtime = 0;
    // now aggregate the measurements
    for (int i = 0; i < params.N; i++) {
        num_ops += args[i].num_ops;
        runtime += args[i].runtime;
    }

    // Send report upstairs.
    put_report(num_ops, runtime);
    dsem_post("reported", 1);

    printf("%ld ops/sec across %d threads", (num_ops * 1000000000 / runtime), params.N);

    // The spawned threads have a pointer to sem_t's on this thread's stack, so
    // should never return from this function so long as those threads are running.
    exit(0);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("must provide redis host as first argument");
        exit(-1);
    }
    bench(argv[1]);
}

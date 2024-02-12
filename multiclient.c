#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/ip.h>
#include <pthread.h>
#include <semaphore.h>
#include <err.h>
#include "hiredis/hiredis.h"
#include "dsync.h"

#define BUF_SIZE 1024

void one_operation(int conn_fd, int msg_size) {
    static __thread uint64_t buf[BUF_SIZE/8] = {0};
    static __thread uint64_t rng_state = 1;

    for (int i = 0; i < msg_size; i+=8) {
        // NOTE: no fixpoints.
        // 37x + 1 = x mod 2^n iff
        // -36x = 1 mod 2^n, which is unsatisfiable via Bézout's lemma because
        // (-36) and 2^n not coprime.
        rng_state = (rng_state*37 + 1);
        *(uint64_t*)(&buf[i]) = rng_state;
    }

    ssize_t n = send(conn_fd, buf, msg_size, 0);
    // n = send(conn_fd, buf, msg_size/2, 0);
    // n = send(conn_fd, buf + msg_size/2, (msg_size/2), 0);
    if (n < msg_size) {
        err(-1, "send");
    }

    n = recv(conn_fd, &buf, msg_size, MSG_WAITALL);

    if (n < msg_size) {
        err(-1, "recv");
    }
}

typedef struct {
    int N;
    const char* address;
    const char* port;
} bench_params_t;
static bench_params_t params;

static sem_t started;
static sem_t should_measure;
static sem_t should_report;
static sem_t reported;

struct client_thread_args {
    uint64_t num_ops;
    uint64_t runtime;
};

void* start_client(void* args_in) {
    struct client_thread_args *args = args_in;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *ai;
    int e = getaddrinfo(params.address, params.port, &hints, &ai);
    if (0 != e) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(e));
        exit(-1);
    }

    int conn_fd = -1;

    for (struct addrinfo *cur_ai = ai; cur_ai != NULL; cur_ai = cur_ai->ai_next) {
        // XXX: this only tries the first addrinfo in the linked list.
        conn_fd = socket(cur_ai->ai_family, cur_ai->ai_socktype, cur_ai->ai_protocol);
        if (0 > conn_fd) {
            err(-1, "socket");
        }

        if (0 != connect(conn_fd, cur_ai->ai_addr, cur_ai->ai_addrlen)) {
            fprintf(stderr, "address: %s, port: %s\n", params.address, params.port);
            err(-1, "connect");
        }
    }
    freeaddrinfo(ai);
    // XXX: At this point, conn_fd is ready to send; should only recv after the first
    // send to avoid a half-connected socket caused by SYN cookies.

    if (conn_fd < 0) {
        perror("conn_fd negative");
    }

    // Get clock resolution.
    struct timespec res;
    e = clock_getres(CLOCK_MONOTONIC, &res);
    if (e != 0) {
        err(-1, "clock_getres");
    }

    // send "Hi", a 2 byte value.
    send(conn_fd, "Hi", 2, 0);

    // get the message size
    int32_t msg_size;
    if (sizeof(msg_size) != recv(conn_fd, &msg_size, sizeof(msg_size), MSG_WAITALL)) {
        err(-1, "recv msg_size");
    }
    msg_size = ntohl(msg_size);

    // Do one operation, then signal the thread that another thread has started.
    one_operation(conn_fd, msg_size);
    sem_post(&started);

    // Warm up by sending messages until notified that we should begin measuring.
    for (;;) {
        if (sem_trywait(&should_measure) >= 0) {
            break;
        }
        one_operation(conn_fd, msg_size);
    }

    struct timespec start, end;
    e = clock_gettime(CLOCK_MONOTONIC, &start);
    if (e != 0) {
        err(-1, "clock_gettime");
    }

    args->num_ops = 0;
    for (;;) {
        if (sem_trywait(&should_report) >= 0) {
            break;
        }
        one_operation(conn_fd, msg_size);
        args->num_ops += 1;
    }
    e = clock_gettime(CLOCK_MONOTONIC, &end);
    if (e != 0) {
        err(-1, "clock_gettime");
    }
    args->runtime = (end.tv_sec - start.tv_sec)*1000000000 + end.tv_nsec - start.tv_nsec;
    sem_post(&reported);

    // Run forever in case other threads are still measuring.
    for (;;) {
        one_operation(conn_fd, msg_size);
    }
    return NULL;
}

static redisContext *redis_ctx;

void load_bench_params() {
    redisReply *reply = redisCommand(redis_ctx, "MGET numthreads address port");
    if (reply == NULL) {
        fprintf(stderr, "Error: %s\n", redis_ctx->errstr);
        exit(-1);
    }
    if (reply->type != REDIS_REPLY_ARRAY) {
        fprintf(stderr, "Got unexpected reply type: %d\n", reply->type);
        exit(-1);
    }

    // make sure the element[j]'s are strings
    for (int i = 0; i < 3; i++) {
        if (reply->element[i]->type != REDIS_REPLY_STRING) {
            fprintf(stderr, "ERROR: Got unexpected reply type: %d\n"
                   "Make sure the redis instance is configured for this benchmark\n", reply->element[i]->type);
            exit(-1);
        }
    }
    params.N = atoi(reply->element[0]->str);
    params.address = strdup(reply->element[1]->str);
    params.port = strdup(reply->element[2]->str);

    freeReplyObject(reply);
}

void try_claim_spot() {
    redisReply *reply = redisCommand(redis_ctx, "DECR numclients");
    if (reply == NULL) {
        fprintf(stderr, "Error: %s\n", redis_ctx->errstr);
        exit(-1);
    }
    if (reply->type != REDIS_REPLY_INTEGER) {
        fprintf(stderr, "Got unexpected reply type (instead of int): %d\n", reply->type);
        exit(-1);
    }
    if (reply->integer < 0) {
        fprintf(stderr, "Enough client processes already running, exiting.\n");
        exit(-1);
    }
}

void put_report(uint64_t throughput) {
    char *data;
    int ret = asprintf(&data, " %ld", throughput);
    if (ret < 0) {
        err(-1, "asprintf");
    }
    redisReply *reply = redisCommand(redis_ctx, "APPEND results %s", data);
    if (reply == NULL) {
        fprintf(stderr, "Error: %s\n", redis_ctx->errstr);
        exit(-1);
    }
    if (reply->type != REDIS_REPLY_INTEGER) {
        fprintf(stderr, "Got unexpected reply type (instead of int): %d\n", reply->type);
        fprintf(stderr, "Reply error: %s\n", reply->str);
        exit(-1);
    }
    free(data);
    freeReplyObject(reply);
}

void bench(const char* redis_addr) {
    redis_ctx = redisConnect(redis_addr, 6379);
    if (redis_ctx == NULL || redis_ctx->err) {
        if (redis_ctx) {
            fprintf(stderr, "Error: %s\n", redis_ctx->errstr);
            // handle error
        } else {
            fprintf(stderr, "Can't allocate redis context\n");
        }
        puts("here");
        exit(-1);
    }
    try_claim_spot();
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
    dcounter_incr(redis_ctx, "started", 1);

    // After hearing "should_measure", tell the N threads to start measuring.
    dflag_wait(redis_ctx, "should_measure");
    for (int i = 0; i < params.N; i++) {
        sem_post(&should_measure);
    }

    puts("measuring");

    // After hearing "should_report", tell the N threads to report in.
    dflag_wait(redis_ctx, "should_report");
    for (int i = 0; i < params.N; i++) {
        sem_post(&should_report);
    }

    puts("collecting reports from threads");
    // Wait for N threads to report.
    for (int i = 0; i < params.N; i++) {
        sem_wait(&reported);
    }

    // now aggregate the measurements
    uint64_t num_ops = 0;
    uint64_t throughput = 0;
    for (int i = 0; i < params.N; i++) {
        num_ops += args[i].num_ops;
        throughput += ((args[i].num_ops*1000000000) + (args[i].runtime/2)) / args[i].runtime;
    }

    // Send report upstairs.
    put_report(throughput);
    dcounter_incr(redis_ctx, "reported", 1);
    puts("sent report");

    printf("%ld total ops\n", num_ops);
    printf("%ld ops/sec across %d threads\n", throughput, params.N);

    // The spawned threads have a pointer to sem_t's on this thread's stack, so
    // should never return from this function so long as those threads are running.
    exit(0);
}

int main(int argc, char **argv) {
    stderr = stdout;
    if (argc < 2) {
        puts("must provide redis host as first argument");
        exit(-1);
    }
    bench(argv[1]);
}

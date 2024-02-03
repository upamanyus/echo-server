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

#define BUF_SIZE 1024
// #define DEBUG_ONE_OP

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

    #ifdef DEBUG_ONE_OP
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    #endif

    ssize_t n = send(conn_fd, buf, msg_size, 0);
    // n = send(conn_fd, buf, msg_size/2, 0);
    // n = send(conn_fd, buf + msg_size/2, (msg_size/2), 0);
    if (n < msg_size/2) {
        perror("send");
        exit(-1);
    }

    #ifdef DEBUG_ONE_OP
    clock_gettime(CLOCK_MONOTONIC, &end);
    if (rand() % 4096 == 0) {
        printf("send %ld\n", (end.tv_sec - start.tv_sec)*1000000000 + end.tv_nsec - start.tv_nsec);
    }
    clock_gettime(CLOCK_MONOTONIC, &start);
    #endif

    n = recv(conn_fd, &buf, msg_size, MSG_WAITALL);

    #ifdef DEBUG_ONE_OP
    clock_gettime(CLOCK_MONOTONIC, &end);
    if (rand() % 4096 == 0) {
        printf("recv %ld\n", (end.tv_sec - start.tv_sec)*1000000000 + end.tv_nsec - start.tv_nsec);
    }
    #endif

    if (n < msg_size) {
        perror("recv");
        exit(-1);
    }
}

static const char* address;
static const char* port;

static sem_t started;
static sem_t should_measure;
static sem_t should_report;
static sem_t reported;

struct client_thread_args {
    uint64_t num_ops;
    uint64_t runtime;
};

void print_clock_res() {
    // Get clock resolution.
    struct timespec res;
    int e = clock_getres(CLOCK_MONOTONIC, &res);
    if (e != 0) {
        err(-1, "clock_getres");
    }
    printf("Clock resolution: %ld ns\n", res.tv_sec * 1000000000 + res.tv_nsec);
}

void* start_client(void* args_in) {
    struct client_thread_args *args = args_in;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *ai;
    int err = getaddrinfo(address, port, &hints, &ai);
    if (0 != err) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        exit(-1);
    }

    int conn_fd = -1;

    for (struct addrinfo *cur_ai = ai; cur_ai != NULL; cur_ai = cur_ai->ai_next) {
        // XXX: this only tries the first addrinfo in the linked list.
        conn_fd = socket(cur_ai->ai_family, cur_ai->ai_socktype, cur_ai->ai_protocol);
        if (0 > conn_fd) {
            perror("socket");
            exit(-1);
        }

        if (0 != connect(conn_fd, cur_ai->ai_addr, cur_ai->ai_addrlen)) {
            perror("connect");
            exit(-1);
        }
    }
    freeaddrinfo(ai);
    // XXX: At this point, conn_fd is ready to send; should only recv after the first
    // send to avoid a half-connected socket caused by SYN cookies.

    // print_clock_res();

    // send "Hi", a 2 byte value.
    send(conn_fd, "Hi", 4, 0);

    // get and print the message size
    int32_t msg_size;
    if (sizeof(msg_size) != recv(conn_fd, &msg_size, sizeof(msg_size), MSG_WAITALL)) {
        perror("recv msg_size");
        exit(-1);
    }
    msg_size = ntohl(msg_size);
    // printf("Message size: %d\n", msg_size);

    // Do one operation, then signal the thread that another thread has started.
    // one_operation(conn_fd, msg_size);
    sem_post(&started);

    // Warm up by sending messages until notified that we should begin measuring.
    for (;;) {
        if (sem_trywait(&should_measure) >= 0) {
            break;
        }
        one_operation(conn_fd, msg_size);
    }

    struct timespec start, end;
    err = clock_gettime(CLOCK_MONOTONIC, &start);
    if (err != 0) {
        perror("clock_gettime");
        exit(-1);
    }

    args->num_ops = 0;
    for (;;) {
        if (sem_trywait(&should_report) >= 0) {
            break;
        }
        one_operation(conn_fd, msg_size);
        args->num_ops += 1;
    }
    err = clock_gettime(CLOCK_MONOTONIC, &end);
    if (err != 0) {
        perror("clock_gettime");
        exit(-1);
    }
    args->runtime = (end.tv_sec - start.tv_sec)*1000000000 + end.tv_nsec - start.tv_nsec;
    sem_post(&reported);

    // Run forever in case other threads are still measuring.
    for (;;) {
        one_operation(conn_fd, msg_size);
    }
    return NULL;
}

void bench(int N, int warmup_sec, int measure_sec) {
    port = "12345";

    sem_init(&started, 0, 0);
    sem_init(&should_measure, 0, 0);
    sem_init(&should_report, 0, 0);
    sem_init(&reported, 0, 0);

    pthread_t tid;

    struct client_thread_args args[N];
    memset(args, 0, sizeof(args));

    for (int i = 0; i < N; i++) {
        pthread_create(&tid, NULL, &start_client, &args[i]);
    }

    // Wait for N threads to start running.
    for (int i = 0; i < N; i++) {
        sem_wait(&started);
    }

    puts("warming up");
    // warmup
    nanosleep(&(struct timespec){.tv_sec = warmup_sec, .tv_nsec = 0}, NULL);

    // Signal N threads to start measuring.
    for (int i = 0; i < N; i++) {
        sem_post(&should_measure);
    }

    puts("measuring");
    nanosleep(&(struct timespec){.tv_sec = measure_sec, .tv_nsec = 0}, NULL);
    puts("done measuring, combining per-thread statistics");

    // Signal N threads to stop measuring.
    for (int i = 0; i < N; i++) {
        sem_post(&should_report);
    }

    // Wait for N threads to report.
    for (int i = 0; i < N; i++) {
        sem_wait(&reported);
    }

    // now aggregate the measurements
    uint64_t num_ops = 0;
    uint64_t throughput = 0;
    for (int i = 0; i < N; i++) {
        num_ops += args[i].num_ops;
        throughput += ((args[i].num_ops*1000000000) + (args[i].runtime/2)) / args[i].runtime;
    }

    printf("%ld total ops\n", num_ops);
    printf("%ld ops/sec across %d threads\n", throughput, N);

    // The spawned threads have a pointer to sem_t's on this thread's stack, so
    // should never return from this function so long as those threads are running.
    exit(0);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        puts("must provide num_threads as first argument, and server address as second argument");
        exit(-1);
    }
    address = argv[2];
    bench(strtol(argv[1], NULL, 10), 5, 15);
}

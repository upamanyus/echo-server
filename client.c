#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/ip.h>
#include <pthread.h>

struct client_thread_args {
    const char* address;
    const char* port;

    _Atomic bool should_measure;
    _Atomic uint64_t num_ops;
    _Atomic uint64_t runtime;
};

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

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    ssize_t n = send(conn_fd, buf, msg_size, 0);
    // n = send(conn_fd, buf, msg_size/2, 0);
    // n = send(conn_fd, buf + msg_size/2, (msg_size/2), 0);
    if (n < msg_size/2) {
        perror("send");
        exit(-1);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    if (rand() % 4096 == 0) {
        printf("send %ld\n", (end.tv_sec - start.tv_sec)*1000000000 + end.tv_nsec - start.tv_nsec);
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    n = recv(conn_fd, &buf, msg_size, MSG_WAITALL);
    clock_gettime(CLOCK_MONOTONIC, &end);
    if (rand() % 4096 == 0) {
        printf("recv %ld\n", (end.tv_sec - start.tv_sec)*1000000000 + end.tv_nsec - start.tv_nsec);
    }
    if (n < msg_size) {
        perror("recv");
        exit(-1);
    }
}

void* start_client(void* args_in) {
    struct client_thread_args *args = args_in;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *ai;
    int err = getaddrinfo(args->address, args->port, &hints, &ai);
    if (0 != err) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(err));
        exit(-1);
    }

    int conn_fd;

    for (struct addrinfo *cur_ai = ai; cur_ai != NULL; cur_ai = cur_ai->ai_next) {
        // XXX: this only tries the first addrinfo in the linked list
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

    // get clock resolution
    struct timespec res;
    err = clock_getres(CLOCK_MONOTONIC, &res);
    if (err != 0) {
        perror("clock_getres");
        exit(-1);
    }
    printf("Clock resolution %ld\n", res.tv_sec * 1000000000 + res.tv_nsec);

    // at this point, conn_fd is ready to send/recv

    // get and print the message size
    int32_t msg_size;
    if (sizeof(msg_size) != recv(conn_fd, &msg_size, sizeof(msg_size), MSG_WAITALL)) {
        perror("recv msg_size");
        exit(-1);
    }
    msg_size = ntohl(msg_size);
    printf("Message size: %d\n", msg_size);

    // warmup by sending messages until notified that we should begin measuring
    for (;;) {
        if (args->should_measure) {
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
        if (!args->should_measure) {
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

    // run forever now
    for (;;) {
        one_operation(conn_fd, msg_size);
    }
    return NULL;
}

int main() {
    struct client_thread_args args;
    args.address = "127.0.0.1";
    args.port = "12345";
    args.should_measure = false;
    pthread_t tid;
    pthread_create(&tid, NULL, &start_client, &args);

    args.should_measure = true;
    nanosleep(&(struct timespec){.tv_sec = 5, .tv_nsec = 0}, NULL);
    args.should_measure = false;

    // FIXME: need to wait for data to actually be reported, but should do so in a more principled way.
    nanosleep(&(struct timespec){.tv_sec = 1, .tv_nsec = 0}, NULL);

    printf("ops: %ld in %ld ns\n", args.num_ops, args.runtime);
    printf("ops/sec: %ld\n", args.num_ops*1000000000/args.runtime);
}

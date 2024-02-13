#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <pthread.h>

#include <err.h>
#include <liburing.h>

#define BUF_SIZE 1024
#define MAX_EVENTS 4096

static int msg_size;

typedef struct {
    int listen_fd;
} thread_args_t;

void handle_new_conn(int conn_fd) {
    char buf[BUF_SIZE];
    // TODO: do this in the "accept" case
    // announce the message size
    int32_t msg_size_conv = htonl(msg_size);
    puts("sending message size");
    ssize_t n = send(conn_fd, &msg_size_conv, sizeof(msg_size_conv), 0);
    puts("sent message size");
    if (n <= 0 || (size_t)n < sizeof(msg_size_conv)) {
        perror("send");
        return;
    }

    // receive the 2 byte initial value from the client.
    n = recv(conn_fd, buf, 2, MSG_WAITALL);
    if (n < 2) {
        perror("recv");
        return;
    }
}

#define NUM_BUFS 16384

enum {
    ACCEPTED,
    RECEIVED_HEADER,
    RECEIVED_MSG,
};

enum {
    WANT_HEADER,
    RUNNING,
};

typedef struct {
    uint8_t pc;

    // this is the union of all the possible state any threadlet might have
    int32_t fd;
    char *buf;
} threadlet_state_t;

void add_accept(struct io_uring *ring, int listen_fd) {
    // multishot?
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
        errx(-1, "sq is full\n");
    }
    io_uring_prep_accept(sqe, listen_fd, NULL, NULL, 0);
    threadlet_state_t *d = malloc(sizeof(threadlet_state_t));
    io_uring_sqe_set_data(sqe, d);
    d->fd = listen_fd;
    d->pc= ACCEPTED;
}

void* main_loop(void *args) {
    // XXX: args is malloc'd by parent thread
    thread_args_t *targs = (thread_args_t*)args;
    int listen_fd = targs->listen_fd;
    free(args);

    struct io_uring ring;
    io_uring_queue_init(4096, &ring, 0);

    // set up accept
    add_accept(&ring, listen_fd);
    int32_t msg_size_conv = htonl(msg_size);
    char header_buf[2];

    for (;;) {
        io_uring_submit(&ring);

        struct io_uring_cqe *cqe;
        int e = io_uring_wait_cqe(&ring, &cqe);
        if (e < 0) {
            fprintf(stderr, "io_uring_wait_cqe: %s\n", strerror(-e));
            exit(-1);
        }

        unsigned head;
        int i = 0;
        io_uring_for_each_cqe(&ring, head, cqe) {
            i++;
            int res = cqe->res;
            threadlet_state_t *r = (threadlet_state_t*)cqe->user_data;
            if (r == NULL) { // threadlet exited
                continue;
            }
            if (r->pc == ACCEPTED) {
                int conn_fd = res;
                if (conn_fd < 0) {
                    fprintf(stderr, "io_uring_wait_cqe/accept: %s\n", strerror(-conn_fd));
                    exit(-1);
                }

                // add `send(msg_size)` to ring
                struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                if (sqe == NULL) {
                    errx(-1, "sq is full\n");
                }
                io_uring_prep_send(sqe, conn_fd, &msg_size_conv, sizeof(msg_size_conv), 0);
                io_uring_sqe_set_data(sqe, NULL);

                // do another accept
                add_accept(&ring, listen_fd);

                // now receive header
                sqe = io_uring_get_sqe(&ring);
                if (sqe == NULL) {
                    errx(-1, "sq is full\n");
                }
                io_uring_prep_recv(sqe, conn_fd, header_buf, sizeof(header_buf), MSG_WAITALL);
                r->pc = RECEIVED_HEADER;
                r->fd = conn_fd;
                io_uring_sqe_set_data(sqe, r);
            } else if (r->pc == RECEIVED_HEADER) {
                if (res < 2) {
                    continue;
                }

                // add first msg `recv`
                struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                if (sqe == NULL) {
                    errx(-1, "sq is full\n");
                }
                r->buf = malloc(BUF_SIZE);
                r->pc = RECEIVED_MSG;
                io_uring_prep_recv(sqe, r->fd, r->buf, msg_size, MSG_WAITALL);
                io_uring_sqe_set_data(sqe, r);
            } else if (r->pc == RECEIVED_MSG) {
                if (res < msg_size) {
                    continue;
                }

                // add `send` for reply, and delay the recv until the send finishes;
                struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
                if (sqe == NULL) {
                    errx(-1, "sq is full\n");
                }
                io_uring_prep_send(sqe, r->fd, r->buf, msg_size, MSG_WAITALL);
                io_uring_sqe_set_data(sqe, NULL);
                io_uring_sqe_set_flags(sqe, IOSQE_IO_LINK);
                // XXX: technically, LINK isn't needed if the client only sends more
                // data after they receive a response. A good client should do that.

                // add next `recv` for msg
                sqe = io_uring_get_sqe(&ring);
                if (sqe == NULL) {
                    errx(-1, "sq is full\n");
                }
                io_uring_prep_recv(sqe, r->fd, r->buf, msg_size, MSG_WAITALL);
                io_uring_sqe_set_data(sqe, r);
            }
        }
        io_uring_cq_advance(&ring, i);
    }
}

void *start_server(void* a) {
    uint16_t port = 12345;
    int msg_sz = 64;

    if (msg_sz > BUF_SIZE) {
        puts("Message size too large");
        exit(-1);
    }
    msg_size = msg_sz;

    // could use getaddrinfo here if desired
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > listen_fd) {
        perror("socket");
        exit(-1);
    }

    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0) {
        perror("setsockopt");
    }

    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEPORT, &(int){1}, sizeof(int)) < 0) {
        perror("setsockopt");
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr = { INADDR_ANY },
        .sin_zero = {0}, // see C99 6.7.8.{10 + 21} to confirm this zero-initializes the whole sin_zero.
    };

    if (0 != bind(listen_fd, (struct sockaddr*) &addr, sizeof(addr))) {
        perror("bind");
        exit(-1);
    }

    if (0 != listen(listen_fd, 1024)) {
        perror("listen");
        exit(-1);
    }
    puts("listening");

    // accept connections and spawn threads to handle them
    thread_args_t *arg = malloc(sizeof(thread_args_t));
    arg->listen_fd = listen_fd;
    main_loop(arg);
    // pthread_t tid;
    // pthread_create(&tid, NULL, &main_loop, arg);
    return NULL;
}

int main() {
    pthread_t tid;
    for (int i = 0; i < 8; i++) {
        pthread_create(&tid, NULL, start_server, NULL);
    }
    pthread_join(tid, NULL);
}

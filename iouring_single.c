#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/ip.h>

#include <pthread.h>
#include <err.h>
// #include <sys/epoll.h>
// #include <fcntl.h>
#include <liburing.h>

#define BUF_SIZE 1024
#define MAX_EVENTS 4096

static int msg_size;

typedef struct {
    int listen_fd;
} thread_args_t;

void set_nonblocking(int fd) {
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == -1) {
        errx(-1, "Unable to set fd to nonblocking");
    }
    if ((fcntl(fd, F_GETFL) & O_NONBLOCK) == 0) {
        errx(-1, "fd not set to nonblocking");
    }
}

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
    RECV,
    SEND,
    ACCEPTER,
};

enum {
    WANT_HEADER,
    RUNNING,
};

typedef struct {
    uint8_t fn_id;
    uint8_t pc;

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
    d->fn_id = ACCEPTER;
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

    // When accepting a new connection, do some synchronous + blocking send+recv
    // operations. Allocate a new buf for that connection.
    // When a recv completes, use that buf for a send.
    for (;;) {
        io_uring_submit(&ring);
        struct io_uring_cqe *cqe;
        int e = io_uring_wait_cqe(&ring, &cqe);
        if (e < 0) {
            fprintf(stderr, "io_uring_wait_cqe: %s\n", strerror(-e));
            exit(-1);
        }
        threadlet_state_t *r = (threadlet_state_t*)cqe->user_data;
        if (r == NULL) { // threadlet_exit
            continue;
        }
        if (r->fn_id == ACCEPTER) {
            int conn_fd = cqe->res;
            if (conn_fd < 0) {
                fprintf(stderr, "io_uring_wait_cqe/accept: %s\n", strerror(-cqe->res));
                exit(-1);
            }
            char *buf = malloc(BUF_SIZE);

            // add send msg_size to ring
            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            if (sqe == NULL) {
                errx(-1, "sq is full\n");
            }
            io_uring_prep_send(sqe, conn_fd, &msg_size_conv, sizeof(msg_size_conv), 0);

            // add recv header to ring
            sqe = io_uring_get_sqe(&ring);
            if (sqe == NULL) {
                errx(-1, "sq is full\n");
            }
            io_uring_prep_recv(sqe, conn_fd, header_buf, sizeof(header_buf), MSG_WAITALL);
        } else if (r->opcode == RECV) {
            char *buf = NULL;
            if (cqe->res < 2) {
                fprintf(stderr, "io_uring_wait_cqe/recv: %s\n", strerror(-cqe->res));
                exit(-1);
            }
            if (r->state == WANT_HEADER) {
                // allocate new buffer
                buf = malloc(BUF_SIZE);
            } else {
                // use existing buffer
                buf = // FIXME: what is the existing buffer...?
            }
        } else if (r->opcode == SEND) {
            // ignore, though report errors for convenience
            if (cqe->res < 0) {
                fprintf(stderr, "io_uring_wait_cqe/accept: %s\n", strerror(-cqe->res));
            }
        }
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
    // set_nonblocking(listen_fd);

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

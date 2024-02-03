#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <liburing.h>
#include <err.h>

#include <pthread.h>

#define BUF_SIZE 1024

static int msg_size;

void* handle_conn(void *args) {
    // XXX: args is malloc'd by parent thread
    int conn_fd = *(int*)args;
    free(args);

    char buf[BUF_SIZE];

    // announce the message size
    int32_t msg_size_conv = htonl(msg_size);
    puts("sending message size");
    ssize_t n = send(conn_fd, &msg_size_conv, sizeof(msg_size_conv), 0);
    puts("sent message size");
    if (n <= 0 || (size_t)n < sizeof(msg_size_conv)) {
        perror("send");
        return NULL;
    }

    // receive the 2 byte initial value from the client.
    n = recv(conn_fd, buf, 2, MSG_WAITALL);
    if (n < 2) {
        perror("recv");
        return NULL;
    }

    // make uring
    struct io_uring ring;
    io_uring_queue_init(2, &ring, 0);
    // XXX: 2 entries should be enough since we'll only queue at most a send+recv every
    // time.


    // echo loop
    for (;;) {
        int e;
        struct io_uring_sqe *sqe;
        struct io_uring_cqe *cqe;

        // do `n = recv(conn_fd, buf, msg_size, MSG_WAITALL);`
        sqe = io_uring_get_sqe(&ring);
        if (sqe == NULL) {
            err(-1, "io_uring_get_sqe: full sq\n");
        }
        io_uring_prep_recv(sqe, conn_fd, buf, msg_size, MSG_WAITALL);

        e = io_uring_submit(&ring);
        if (err < 0) {
            err(-1, "io_uring_submit: %d\n", -e);
        }

        e = io_uring_wait_cqe(&ring, &cqe);
        if (e < 0) {
            err(-1, "io_uring_wait_cqe: %d\n", -e);
        }
        // `recv` done at this point
        n = cqe->res;
        io_uring_cqe_seen(&ring, cqe);

        if (n < msg_size) {
            perror("recv");
            return NULL;
        }

        // do `n = send(conn_fd, buf, msg_size, 0);`
        sqe = io_uring_get_sqe(&ring);
        if (sqe == NULL) {
            err(-1, "io_uring_get_sqe: full sq\n");
        }
        io_uring_prep_send(sqe, conn_fd, buf, msg_size, 0);

        e = io_uring_submit(&ring);
        if (err < 0) {
            err(-1, "io_uring_submit: %d\n", -e);
        }

        e = io_uring_wait_cqe(&ring, &cqe);
        if (e < 0) {
            err(-1, "io_uring_wait_cqe: %d\n", -e);
        }
        // `send` done at this point
        n = cqe->res;
        io_uring_cqe_seen(&ring, cqe);

        // send back the same bytes
        if (n < msg_size) {
            perror("send");
            return NULL;
        }
    }
}

int start_server(uint16_t port, int msg_sz) {
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
    for (;;) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (0 > conn_fd) {
            perror("accept");
            exit(-1);
        }
        puts("accepted");

        pthread_t tid;
        int *conn_fd_arg = malloc(sizeof(int));
        *conn_fd_arg = conn_fd;
        pthread_create(&tid, NULL, &handle_conn, conn_fd_arg);
    }
}

int main() {
    puts("thread per client connection, io_uring per thread/conn");
    start_server(12345, 64);
}

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/ip.h>

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
    ssize_t n = send(conn_fd, &msg_size_conv, sizeof(msg_size_conv), 0);
    if (n <= 0 || (size_t)n < sizeof(msg_size_conv)) {
        perror("send");
        return NULL;
    }

    // echo loop
    for (;;) {
        n = recv(conn_fd, buf, msg_size, MSG_WAITALL);
        if (n <= 0 || (size_t)n < msg_size) {
            perror("recv");
            return NULL;
        }

        // send back the same bytes
        n = send(conn_fd, buf, msg_size, 0);
        if (n <= 0 || (size_t)n < msg_size) {
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

    if (0 != listen(listen_fd, 0)) {
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
        // puts("accepted");

        pthread_t tid;
        int *conn_fd_arg = malloc(sizeof(int));
        *conn_fd_arg = conn_fd;
        pthread_create(&tid, NULL, &handle_conn, conn_fd_arg);
    }
}

int main() {
    start_server(12345, 64);
}

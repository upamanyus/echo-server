#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/ip.h>

#include <pthread.h>
#include <err.h>
#include <sys/epoll.h>
#include <fcntl.h>

#define BUF_SIZE 1024
#define MAX_EVENTS 4096

static int msg_size;

typedef struct {
    int epfd;
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

void* main_loop(void *args) {
    // XXX: args is malloc'd by parent thread
    thread_args_t *targs = (thread_args_t*)args;
    int epfd = targs->epfd;
    int listen_fd = targs->listen_fd;
    free(args);

    struct epoll_event events[MAX_EVENTS];
    char buf[BUF_SIZE];

    struct epoll_event ev;

    // echo loop
    for (;;) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            err(-1, "epoll_wait");
        }
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == listen_fd) {
                // FIXME: drain the listen_fd here?
                int conn_fd = accept(listen_fd, NULL, NULL);
                if (0 > conn_fd) {
                    perror("accept");
                    exit(-1);
                }
                puts("accepted");
                handle_new_conn(conn_fd);
                set_nonblocking(conn_fd);
                ev.events = EPOLLIN; // FIXME: should also poll for sends, and handle that case
                ev.data.fd = conn_fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &ev);
            } else { // a connection socket
                int conn_fd = events[i].data.fd;
                if ((ev.events & EPOLLIN) == 0) {
                    errx(-1, "epoll event without EPOLLIN");
                }
                int n = recv(conn_fd, buf, msg_size, MSG_WAITALL);
                if (n < msg_size) {
                    perror("recv");
                    epoll_ctl(epfd, EPOLL_CTL_DEL, conn_fd, NULL);
                    continue;
                }

                // FIXME: this could return EAGAIN/EWOULDBLOCK, in which case we
                // should put conn_fd on the epoll instance.
                n = send(conn_fd, buf, msg_size, 0);
                if (n < msg_size) {
                    perror("send");
                    epoll_ctl(epfd, EPOLL_CTL_DEL, conn_fd, NULL);
                    continue;
                }
            }
        }
    }
}

void start_server(uint16_t port, int msg_sz) {
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
    set_nonblocking(listen_fd);
    int epfd = epoll_create(1);
    if (epfd < 0) {
        err(-1, "epoll_create");
    }
    struct epoll_event ev;
    ev.events = EPOLLIN; // FIXME: should also poll for sends, and handle that case
    ev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);


    // accept connections and spawn threads to handle them
    thread_args_t *arg = malloc(sizeof(thread_args_t));
    arg->epfd = epfd;
    arg->listen_fd = listen_fd;
    main_loop(arg);
    // pthread_t tid;
    // pthread_create(&tid, NULL, &main_loop, arg);
}

int main() {
    start_server(12345, 64);
}

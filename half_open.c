#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netdb.h>

void client_connect_one() {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *ai;
    int err = getaddrinfo("127.0.0.1", "12345", &hints, &ai);
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
    // At this point, conn_fd is ready to send/recv.
}

int start_server() {
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
        .sin_port = htons(12345),
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

    // fill up the "established" queue
    client_connect_one();
    for (int i = 0; i < 10; i++) {
        client_connect_one();
        printf("%d\n", i);
    }

    // accept connections and spawn threads to handle them
    for (;;) {
        int conn_fd = accept(listen_fd, NULL, NULL);
        if (0 > conn_fd) {
            perror("accept");
            exit(-1);
        }
        puts("accepted");
    }
}

int main() {
    start_server();
}

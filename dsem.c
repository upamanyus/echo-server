#include "dsem.h"
#include <stdlib.h>
#include <time.h>

// n is the amount by which we increase
void dsem_post(redisContext *redis_ctx, const char *a, int n) {
    void *reply = redisCommand(redis_ctx, "INCRBY \"%s\" %d", a, n);
    if (reply == NULL) {
        fprintf(stderr, "Error: %s\n", redis_ctx->errstr);
        exit(-1);
    }
    freeReplyObject(reply);
}

// n is the amount by which we decrease, but only at once.
void dsem_wait(redisContext *redis_ctx, const char *a, int n) {
    for (;;) {
        redisReply *reply = redisCommand(redis_ctx, "GET %s", a);
        if (reply == NULL) {
            fprintf(stderr, "Error: %s\n", redis_ctx->errstr);
            exit(-1);
        }
        if (reply->type != REDIS_REPLY_STRING) {
            fprintf(stderr, "Got unexpected reply type: %d\n", reply->type);
            exit(-1);
        }
        int64_t x = strtol(reply->str, NULL, 10);
        if (x >= n) {
            printf("Wait done");
            exit(-1);
        }
        nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 100000000}, NULL);
    }
    exit(-1);
}

#include "dsync.h"
#include <stdlib.h>
#include <time.h>

void dcounter_incr(redisContext *redis_ctx, const char *a, int n) {
    void *reply = redisCommand(redis_ctx, "INCRBY %s %d", a, n);
    if (reply == NULL) {
        fprintf(stderr, "Error: %s\n", redis_ctx->errstr);
        exit(-1);
    }
    freeReplyObject(reply);
}

void dflag_wait(redisContext *redis_ctx, const char *a) {
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
        if (x > 0) {
            return;
        }
        nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 100000000}, NULL);
    }
    exit(-1);
}

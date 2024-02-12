#ifndef DSYNC_H_
#define DSYNC_H_

#include "hiredis/hiredis.h"

// n is the amount by which the counter is increased
void dcounter_incr(redisContext *redis_ctx, const char *a, int n);

void dflag_wait(redisContext *redis_ctx, const char *a);

#endif // DSYNC_H_

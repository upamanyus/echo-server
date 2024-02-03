#ifndef DSEM_H_
#define DSEM_H_

#include "hiredis/hiredis.h"

// n is the amount by which we increase
void dsem_post(redisContext *redis_ctx, const char *a, int n);

// n is the amount by which we decrease, but only at once.
void dsem_wait(redisContext *redis_ctx, const char *a, int n);

#endif // DSEM_H_

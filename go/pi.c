#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>

typedef long double float64;

float64 custom_pow(float64 x, uint64_t n) {
    if (n == 0) {
        return 1.0;
    }
    float64 y = 1.0;
    while (n > 0) {
        if (n % 2 == 1) {
            y *= x;
        }
        x = x*x;
        n = n/2;
    }
    return y;
}

uint64_t numIters;
volatile bool shouldStart = false;
sem_t threads;

void* oneThread(void* _unused) {
    float64 x = (float64)rand()/(float64)RAND_MAX;
    printf("%Lf\n", x);

    float64 a = 0;
    while (shouldStart == false) {
    }

    for (uint64_t n = 0; n < numIters; n++) {
        uint64_t k = 2*n + 1;
        a += custom_pow(-1, n) * custom_pow(x,k)/(float64)k;
    }
    sem_post(&threads);
    printf("%Lf\n", a);
    return NULL;
}

void bench(uint64_t numThreads, uint64_t numOps) {
    pthread_t tid;
    numIters = numOps;
    for (uint64_t i = 0; i < numThreads; i++) {
        pthread_create(&tid, NULL, oneThread, NULL);
    }

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    shouldStart = true; // FIXME: use an atomic operation here

    for (uint64_t i = 0; i < numThreads; i++) {
        sem_wait(&threads);
    }
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("Took %ld ms\n", ((end.tv_sec - start.tv_sec)*1000000000 + end.tv_nsec - start.tv_nsec)/1000000);
}

int main() {
    const uint64_t oneK = 1000;
    bench(6, 10 * oneK * oneK);
}

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#include "alloc.h"

#define THREAD_COUNT 16
#define ALLOCATIONS_PER_THREAD 1000000
#define BLOCK_SIZE 64

typedef enum {
    USE_MALLOC,
    USE_SLAB,
} Mode;

typedef struct {
    Mode mode;
    int thread_id;
} ThreadArg;

void *worker(void *arg_ptr) {
    ThreadArg *arg = (ThreadArg *)arg_ptr;
    void **ptrs = malloc(sizeof(void *) * ALLOCATIONS_PER_THREAD);

    if(arg->mode == USE_MALLOC) {
        for(int i = 0; i < ALLOCATIONS_PER_THREAD; i++)
            ptrs[i] = malloc(BLOCK_SIZE);
        for(int i = 0; i < ALLOCATIONS_PER_THREAD; i++)
            free(ptrs[i]);
    } else {
        for(int i = 0; i < ALLOCATIONS_PER_THREAD; i++)
            ptrs[i] = slab_alloc();
        for(int i = 0; i < ALLOCATIONS_PER_THREAD; i++)
            slab_free(ptrs[i]);
    }

    free(ptrs);
    return NULL;
}

double benchmark_multithreaded(Mode mode) {
    pthread_t threads[THREAD_COUNT];
    ThreadArg args[THREAD_COUNT];

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for(int i = 0; i < THREAD_COUNT; i++) {
        args[i].mode = mode;
        args[i].thread_id = i;
        pthread_create(&threads[i], NULL, worker, &args[i]);
    }

    for(int i = 0; i < THREAD_COUNT; i++)
        pthread_join(threads[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &end);

    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

int main(int argc, char **argv) {
    printf("Threads: %d\nAllocations per thread: %d\n\n", THREAD_COUNT, ALLOCATIONS_PER_THREAD);

    double malloc_time = benchmark_multithreaded(USE_MALLOC);
    double slab_time = benchmark_multithreaded(USE_SLAB);

    slab_deinit();

    printf("Multithreaded Benchmark Results:\n");
    printf("malloc:\t\t%.6f ns\n", malloc_time);
    printf("slab_alloc:\t%.6f ns\n", slab_time);
    printf("Speedup:\t\t%.2fx\n", malloc_time / slab_time);

    return 0;
}
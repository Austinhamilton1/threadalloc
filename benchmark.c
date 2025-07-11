#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

#include "alloc.h"

#define THREAD_COUNT 4
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
    void *block;

    if(arg->mode == USE_MALLOC) {
        for(int i = 0; i < ALLOCATIONS_PER_THREAD; i++)
            ptrs[i] = malloc(BLOCK_SIZE);
        for(int i = 0; i < ALLOCATIONS_PER_THREAD; i++)
            free(ptrs[i]);
        for(int i = 0; i < ALLOCATIONS_PER_THREAD; i++) {
            block = malloc(BLOCK_SIZE);
            free(block);
        }
    } else {
        for(int i = 0; i < ALLOCATIONS_PER_THREAD; i++)
            ptrs[i] = slab_alloc();
        for(int i = 0; i < ALLOCATIONS_PER_THREAD; i++)
            slab_free(ptrs[i]);
        for(int i = 0; i < ALLOCATIONS_PER_THREAD; i++) {
            block = slab_alloc();
            slab_free(block);
        }
    }

    free(ptrs);
    return NULL;
}

double benchmark_multithreaded(int thread_count, Mode mode) {
    pthread_t threads[thread_count];
    ThreadArg args[thread_count];

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for(int i = 0; i < thread_count; i++) {
        args[i].mode = mode;
        args[i].thread_id = i;
        pthread_create(&threads[i], NULL, worker, &args[i]);
    }

    for(int i = 0; i < thread_count; i++)
        pthread_join(threads[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &end);

    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

double benchmark_singlethreaded(Mode mode) {
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    void **ptrs = malloc(sizeof(void *) * ALLOCATIONS_PER_THREAD);
    void *block;

    if(mode == USE_MALLOC) {
        for(int i = 0; i < ALLOCATIONS_PER_THREAD; i++)
            ptrs[i] = malloc(BLOCK_SIZE);
        for(int i = 0; i < ALLOCATIONS_PER_THREAD; i++)
            free(ptrs[i]);
        for(int i = 0; i < ALLOCATIONS_PER_THREAD; i++) {
            block = malloc(BLOCK_SIZE);
            free(block);
        }
    } else {
        for(int i = 0; i < ALLOCATIONS_PER_THREAD; i++)
            ptrs[i] = slab_alloc();
        for(int i = 0; i < ALLOCATIONS_PER_THREAD; i++)
            slab_free(ptrs[i]);
        for(int i = 0; i < ALLOCATIONS_PER_THREAD; i++) {
            block = slab_alloc();
            slab_free(block);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    free(ptrs);

    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

int main(int argc, char **argv) {
    if(argc > 2) {
        printf("Usage: benchmark [opt:num_threads]\n");
        return -1;
    }

    int thread_count = THREAD_COUNT;
    if(argc == 2) {
        thread_count = atoi(argv[1]);
    }

    printf("Threads: %d\nAllocations per thread: %d\n\n", thread_count, ALLOCATIONS_PER_THREAD);

    double malloc_time = benchmark_singlethreaded(USE_MALLOC);
    double slab_time = benchmark_singlethreaded(USE_SLAB);

    printf("Singlethreaded Benchmark Results:\n");
    printf("malloc:\t\t%.6f sec\n", malloc_time);
    printf("slab_alloc:\t%.6f sec\n", slab_time);
    printf("Speedup:\t\t%.2fx\n", malloc_time / slab_time);
    
    malloc_time = benchmark_multithreaded(thread_count, USE_MALLOC);
    slab_time = benchmark_multithreaded(thread_count, USE_SLAB);

    printf("Multithreaded Benchmark Results:\n");
    printf("malloc:\t\t%.6f sec\n", malloc_time);
    printf("slab_alloc:\t%.6f sec\n", slab_time);
    printf("Speedup:\t\t%.2fx\n", malloc_time / slab_time);

    return 0;
}
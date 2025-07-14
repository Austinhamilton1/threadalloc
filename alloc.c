#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>

#include "alloc.h"

#define BLOCK_SIZE 64                                                   // Make sure the blocks can hold *most* generic datatypes.
#define BLOCK_COUNT 1024                                                // 1024 blocks per slab.
#define ALIGN_UP(x, align) (((x) + ((align) - 1)) & ~((align) - 1))     // Align x to the next multiple of align
#define SLAB_OVERHEAD ALIGN_UP(sizeof(Slab), BLOCK_SIZE)                // This is how many blocks are used by the slab pointer.
#define EFFECTIVE_BLOCKS (BLOCK_COUNT - SLAB_OVERHEAD)                  // This is how many blocks we can use.
#define BLOCK_CACHE_LIMIT 64
#define BLOCK_CACHE_REFILL_LIMIT 32

// Key for cleaning up thread cache after use.
static pthread_key_t thread_cache_key;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static __thread ThreadCache *thread_cache = NULL;

/*
 * Free up a thread's cache.
 * Arguments:
 *     void *arg - Pointer to the thread cache.
 */
static void slab_thread_destructor(void *arg) {
    ThreadCache *cache = (ThreadCache *)arg;
    if(!cache) return;

    // Free up the current slab list
    Slab *slab = cache->current_slab;
    while(slab) {
        Slab *next = slab->next;

        // Deallocate blocks
        if(slab->raw_allocation)
            free(slab->raw_allocation);

        // Deallocate slab
        slab = next;
    }

    // Free up the partial slab list
    slab = cache->partial_slabs;
    while(slab) {
        Slab *next = slab->next;

        // Deallocate blocks
        if(slab->raw_allocation)
            free(slab->raw_allocation);

        // Deallocate slab
        slab = next;
    }

    // Deallocate cache
    free(cache);
}

/*
 * Initialize a pthread with the defined destructor.
 */
static void slab_global_init() {
    pthread_key_create(&thread_cache_key, slab_thread_destructor);
}

/*
 * Get the local thread cache or allocate a new one if it doesn't yet exist.
 * Returns:
 *     ThreadCache * - The local thread cache.
 */
static ThreadCache *get_thread_cache() {
    // Set the initialization function if this hasn't been called before
    pthread_once(&init_once, slab_global_init);

    // Attempt to get the thread cache, if it doesn't exist create a new one
    ThreadCache *cache = (ThreadCache *)pthread_getspecific(thread_cache_key);
    if(!cache) {
        cache = calloc(1, sizeof(ThreadCache)); // Zero-init
        pthread_setspecific(thread_cache_key, cache);
        thread_cache = cache;
    }
    return cache;
}

/*
 * Helper function that quickly grabs the cache if it is already defined. Keeps the
 * program from calling pthread_once multiple times.
 */
static inline ThreadCache *fast_thread_cache() {
    if(__builtin_expect(thread_cache != 0, 1)) {
        return thread_cache;
    }
    return get_thread_cache();
}

/*
 * Allocate a new slab. This will be called if the current slab is full.
 * Arguments:
 *     ThreadCache *current_context - The thread we are allocating from or NULL if there is no context.
 * Returns:
 *     Slab * - An initialized slab ready for use or NULL on error.
 */
static Slab *allocate_new_slab() {
    size_t alignment = BLOCK_SIZE * BLOCK_COUNT;
    size_t total_size = alignment + alignment; // Extra space for alignment

    // Allocate the slab memory and check for errors
    void *raw_mem = malloc(total_size);
    if(!raw_mem) return NULL;

    // Align the memory
    uintptr_t aligned_addr = ALIGN_UP((uintptr_t)raw_mem, alignment);

    // Store the slab at the beginning of the memory region
    Slab *slab = (Slab *)aligned_addr;

    // Set slab metadata
    slab->free_count = EFFECTIVE_BLOCKS;

    // Store the slab's memory as the allocated memory
    slab->mem = (void *)aligned_addr;
    slab->raw_allocation = raw_mem;
    *((Slab **)slab->mem) = slab;

    // Calculate where the actual blocks start (after the slab)
    void *block_start = (char *)slab->mem + (SLAB_OVERHEAD * BLOCK_SIZE);

    // Zero out blocks to load them into RAM
    memset(block_start, 0, EFFECTIVE_BLOCKS * BLOCK_SIZE);
    
    // Set the free list
    Block *current = (Block *)block_start;
    slab->free_list = current;

    // Link the blocks
    for(int i = 0; i < EFFECTIVE_BLOCKS - 1; i++) {
        void *next_block_mem = (char *)current + BLOCK_SIZE;
        Block *next_block = (Block *)next_block_mem;
        current->next = next_block;
        current = next_block;
    }
    current->next = NULL;

    // Add to the slab list if a current context was provided
    ThreadCache *cache = fast_thread_cache();
    slab->next = cache->current_slab;
    cache->current_slab = slab;

    return slab;
}

/*
 * Allocate a block from the slab allocator. Try to allocate from the thread's slabs first.
 * If there are no slabs in the thread, create a new one.
 * Returns:
 *      void * - A block of memory for the thread to use or NULL on empty.
 */
void *slab_alloc() {
    ThreadCache *cache = fast_thread_cache();
    Block *block = NULL;

    // Try to allocate from the block fastbin (fastest)
    if(cache->fastbin) {
        block = cache->fastbin;
        cache->fastbin = block->next;
        cache->fastbin_count--;
        return (void *)block;
    }

    // Try to allocate from thread local cache (fast)
    if(cache->current_slab && cache->current_slab->free_count) {
        // Allocate from the cached slab
        Slab *slab = cache->current_slab;

        // Check if we have enough to partially refil fastbin
        if(cache->current_slab->free_count > BLOCK_CACHE_REFILL_LIMIT) {
            // Partially refill fastbin
            Block *batch[BLOCK_CACHE_REFILL_LIMIT];

            for(int i = 0; i < BLOCK_CACHE_REFILL_LIMIT; i++) {
                batch[i] = slab->free_list;
                slab->free_list = batch[i]->next;
                slab->free_count--;
            }

            for(int i = 0; i < BLOCK_CACHE_REFILL_LIMIT; i++) {
                batch[i]->next = cache->fastbin;
                cache->fastbin = batch[i];
                cache->fastbin_count++;
            }

            // Pop off top of fastbin
            block = cache->fastbin;
            cache->fastbin = block->next;
            cache->fastbin_count--;
            return (void *)block;
        }

        block = slab->free_list;
        slab->free_list = block->next;
        slab->free_count--;

        // If the slab is empty, we will drop to partials in next allocation
        if(slab->free_count == 0)
            cache->current_slab = NULL;

        return (void *)block;
    }

    // If the current slab head is empty, look and see if there are other slabs in the list that are not
    Slab *slab = cache->partial_slabs;
    if(slab) {
        // Pop head of partial list, set is as current, and go back to fast path
        cache->partial_slabs = slab->next;
        cache->current_slab = slab;

        return slab_alloc();
    }

    // If the allocation fails, allocate a new slab (slow)
    slab = allocate_new_slab();
    if(!slab) return NULL;

    // Initialize thread-local slab and free list
    cache->current_slab = slab;
    return slab_alloc();
}

/*
 * Free a block back to the allocator. This needs to determine which slab owns the
 * block using pointer arithmetic and give it back to that slab.
 * Arguments:
 *     void *block - The block that was allocated.
 */
void slab_free(void *block) {
    ThreadCache *cache = fast_thread_cache();
    Block *b = (Block *)block;

    // Fast path: just push to the thread-local block cache
    if(cache->fastbin_count < BLOCK_CACHE_LIMIT) {
        b->next = cache->fastbin;
        cache->fastbin = b;
        cache->fastbin_count++;
        return;
    }

    // Get the parent of the block
    uintptr_t mem_start = (uintptr_t)block & ~(BLOCK_SIZE * BLOCK_COUNT - 1);
    Slab *parent = *((Slab **)mem_start);

    // Add the block to the head of the free_list
    b->next = parent->free_list;
    parent->free_list = b;
    parent->free_count++;

    // If we went from full to partial, put it in the partial list
    if(parent->free_count == 1 && parent != cache->current_slab) {
        parent->next = cache->partial_slabs;
        cache->partial_slabs = parent;
    }
}
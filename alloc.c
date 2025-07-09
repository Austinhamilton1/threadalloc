#include <stddef.h>
#include <stdlib.h>
#include <stdalign.h>

#include "alloc.h"

#define BLOCK_SIZE 64
#define BLOCK_COUNT 1024

/*
 * Allocate a new slab. This will be called if the current slab is full.
 * Arguments:
 *     ThreadCache *current_context - The thread we are allocating from or NULL if there is no context.
 *     size_t block_size - The size of the blocks to allocate (A slab will start with 1024 blocks).
 * Returns:
 *     Slab * - An initialized slab ready for use or NULL on error.
 */
Slab *allocate_new_slab(ThreadCache *current_context, size_t block_size) {
    // Allocate the slab and check for error
    Slab *slab = (Slab *)malloc(sizeof(Slab));
    if(!slab)
        return NULL;

    // Set slab metadata
    slab->block_size = BLOCK_SIZE;
    slab->total_blocks = BLOCK_COUNT;
    slab->free_count = BLOCK_COUNT;

    // Allocate the slab memory and check for errors
    slab->mem = malloc(BLOCK_SIZE * BLOCK_COUNT);
    if(!slab->mem) {
        free(slab);
        return NULL;
    }

    // Set the free list
    Block *current = (Block *)slab->mem;
    slab->free_list = current;

    // Link the blocks
    for(int i = 0; i < BLOCK_COUNT - 1; i++) {
        void *next_block_mem = (char *)current + BLOCK_SIZE;
        Block *next_block = (Block *)next_block_mem;
        current->next = next_block;
        current = next_block;
    }
    current->next = NULL;

    // Add to the slab list if a current context was provided
    if(current_context) {
        slab->next = current_context->current_slab;
        current_context->current_slab = slab;
    }

    return slab;
}

/*
 * Deallocates a slab list. Called for cleanup when all slabs are done being used.
 * Arguments:
 *     Slab *head - The head of the slab list. Start stepping from here.
 */
void free_slab_list(Slab *head) {
    // Step through the linked list of slabs
    Slab *current = head;
    while(current) {
        // Free the slab memory
        if(current->mem)
            free(current->mem);

        // Free the slab
        Slab *prev = current;
        current = current->next;
        free(prev);
    }
}

/*
 * Initialize the global free list. This should be run one time before allocations are made
 * from individual threads.
 * Returns:
 *     int - A result code (0 on success, -1 on failure).
 */
int init_global_free_list() {
    // Allocate the block memory and check for error
    void *mem = malloc(64 * BLOCK_COUNT);
    if(!mem)
        return -1;

    // Set the free list
    Block *head = (Block *)mem;
    Block *current = head;

    // Link the blocks
    for(int i = 0; i < BLOCK_COUNT - 1; i++) {
        void *next_block_mem = (char *)current + BLOCK_SIZE;
        Block *next_block = (Block *)next_block_mem;
        current->next = next_block;
        current = next_block;
    }
    current->next = NULL;

    // Set the global memory 
    atomic_store_explicit(&global_free_list_head, head, memory_order_release);
    return 0;
        
}

/*
 * Allocate a block from the slab allocator. Try to allocate from the thread's slabs first.
 * If there are no slabs in the thread, allocate from the global memory pool. If this is also 
 * full, create a new slab to allocate from.
 * Arguments:
 *     ThreadCache *cache - The thread's local cache.
 * Returns:
 *      void * - A block of memory for the thread to use or NULL on empty.
 */
void *slab_alloc(ThreadCache *cache) {
    Block *block = NULL;

    // Try to allocate from thread local cache (fastest)
    if(cache->current_slab && cache->current_slab->free_list != NULL) {
        block = cache->current_slab->free_list;
        cache->current_slab->free_list = block->next;
        cache->current_slab->free_count--;
        return (void *)block;
    }

    // If the current slab head is empty, look and see if there are other slabs in the list that are not
    if(cache->current_slab) {
        // Loop until a non-empty slab is found
        Slab *current = cache->current_slab;
        while(current && current->free_count == 0) {
            current = current->next;
        }

        // If there is a non-empty slab, allocate from it.
        if(current) {
            block = current->free_list;
            current->free_list = block->next;
            current->free_count--;
            return (void *)block;
        }
    }

    // If that fails, try to allocate from lock-free global memory pool (not as fast but still fast)
    Block *old_head = atomic_load_explicit(&global_free_list_head, memory_order_acquire);
    while(old_head != NULL) {
        Block *next = old_head->next;
        if(atomic_compare_exchange_weak_explicit(&global_free_list_head, &old_head, next, memory_order_acq_rel, memory_order_acquire))
            return (void *)old_head;
        old_head = atomic_load_explicit(&global_free_list_head, memory_order_acquire);
    }

    // If all else fails, allocate a new slab (slow)
    Slab *slab = allocate_new_slab(cache);
    if(!slab)
        return NULL;

    // Initialize thread-local slab and free list
    cache->current_slab = slab;

    // Allocate from the new slab's free list
    block = slab->free_list;
    slab->free_list = block->next;
    slab->free_count--;
    return (void *)block;
}
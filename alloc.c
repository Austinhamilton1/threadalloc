#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include "alloc.h"

#define BLOCK_SIZE 64                                                   // Make sure the blocks can hold *most* generic datatypes.
#define BLOCK_COUNT 1024                                                // 1024 blocks per slab.
#define ALIGN_UP(x, align) (((x) + ((align) - 1)) & ~((align) - 1))     // Align x to the next multiple of align
#define SLAB_POINTER_OVERHEAD ALIGN_UP(sizeof(Slab *), BLOCK_SIZE)      // This is how many blocks are used by the slab pointer.
#define EFFECTIVE_BLOCKS (BLOCK_COUNT - SLAB_POINTER_OVERHEAD)          // This is how many blocks we can use.

static __thread ThreadCache thread_cache = {
    .current_slab = NULL,
};

/*
 * Allocate a new slab. This will be called if the current slab is full.
 * Arguments:
 *     ThreadCache *current_context - The thread we are allocating from or NULL if there is no context.
 * Returns:
 *     Slab * - An initialized slab ready for use or NULL on error.
 */
Slab *allocate_new_slab(ThreadCache *current_context) {
    // Allocate the slab and check for error
    Slab *slab = (Slab *)malloc(sizeof(Slab));
    if(!slab)
        return NULL;

    // Set slab metadata
    slab->block_size = BLOCK_SIZE;
    slab->total_blocks = EFFECTIVE_BLOCKS;
    slab->free_count = EFFECTIVE_BLOCKS;

    size_t alignment = BLOCK_SIZE * BLOCK_COUNT;
    size_t total_size = alignment + alignment; // Extra space for alignment

    // Allocate the slab memory and check for errors
    void *raw_mem = malloc(total_size);
    if(!raw_mem) {
        free(slab);
        return NULL;
    }

    // Align the memory
    uintptr_t aligned_addr = ALIGN_UP((uintptr_t)raw_mem, alignment);
    slab->mem = (void *)aligned_addr;

    // Store the slab pointer at the beginning of the memory region
    *((Slab **)slab->mem) = slab;

    // Store the raw memory to free later
    slab->raw_allocation = raw_mem;

    // Calculate where the actual blocks start (after the slab pointer)
    void *block_start = (char *)slab->mem + (SLAB_POINTER_OVERHEAD * BLOCK_SIZE);
    
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
    if(current_context) {
        slab->next = current_context->current_slab;
        current_context->current_slab = slab;
    }

    return slab;
}

/*
 * Allocate a block from the slab allocator. Try to allocate from the thread's slabs first.
 * If there are no slabs in the thread, create a new one.
 * Returns:
 *      void * - A block of memory for the thread to use or NULL on empty.
 */
void *slab_alloc() {
    Block *block = NULL;

    // Try to allocate from thread local cache (fast)
    if(thread_cache.current_slab && thread_cache.current_slab->free_list != NULL) {
        block = thread_cache.current_slab->free_list;
        thread_cache.current_slab->free_list = block->next;
        thread_cache.current_slab->free_count--;
        return (void *)block;
    }

    // If the current slab head is empty, look and see if there are other slabs in the list that are not
    if(thread_cache.current_slab) {
        // Loop until a non-empty slab is found
        Slab *current = thread_cache.current_slab;
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

    // If the allocation fails, allocate a new slab (slow)
    Slab *slab = allocate_new_slab(&thread_cache);
    if(!slab)
        return NULL;

    // Initialize thread-local slab and free list
    thread_cache.current_slab = slab;

    // Allocate from the new slab's free list
    block = slab->free_list;
    slab->free_list = block->next;
    slab->free_count--;
    return (void *)block;
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
        if(current->raw_allocation)
            free(current->raw_allocation);

        // Free the slab
        Slab *prev = current;
        current = current->next;
        free(prev);
    }
}

/*
 * Deinitialize the slab allocator.
 */
void slab_deinit() {
    free_slab_list(thread_cache.current_slab);
    thread_cache.current_slab = NULL;
}

/*
 * Free a block back to the allocator. This needs to determine which slab owns the
 * block using pointer arithmetic and give it back to that slab.
 * Arguments:
 *     void *block - The block that was allocated.
 */
void slab_free(void *block) {
    // Get the parent of the block
    uintptr_t mem_start = (uintptr_t)block & ~(BLOCK_SIZE * BLOCK_COUNT - 1);
    Slab *parent = *((Slab **)mem_start);

    // Add the block to the head of the free_list
    Block *new_head = (Block *)block;
    new_head->next = parent->free_list;
    parent->free_list = new_head;
    parent->free_count++;
}
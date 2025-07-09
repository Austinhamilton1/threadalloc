#ifndef ALLOC_H
#define ALLOC_H

#include <stddef.h>
#include <stdatomic.h>

typedef struct block {
    struct block *next;     // Free list used for cached allocations (intrusive linked list).
} Block;

typedef struct slab {
    void *mem;              // The slab's memory allocation.
    size_t block_size;      // Size of each block in the slab.
    size_t total_blocks;    // Total number of blocks available in slab.
    size_t free_count;      // Total number of free blocks available in slab.
    Block *free_list;       // Linked list of blocks to allocate from.
    Slab *next;             // If we use up one of the slabs, we need to allocate a new one, but keep tracking the old one.
} Slab;

typedef struct threadcache {
    Slab *current_slab;     // Used by each thread to cache a slab for quick allocation.
} ThreadCache;

// Global lock-free free_list head (atomic)
_Atomic(Block *) global_free_list_head;

#endif
/* memory.c - Memory management implementation */

#include "../include/kernel.h"
#include "../include/memory.h"
#include "../include/process.h"
#include "../include/interrupts.h"

#include <string.h>
#include <stdbool.h>

extern char _heap_start;
extern char _heap_end;
extern char _stack_start;
extern char _stack_end;

#ifndef __LINKER_DEFINED_HEAP__
#define HEAP_SIZE       (64 * 1024)
#define STACK_SIZE      (32 * 1024)
static char heap_memory[HEAP_SIZE] __attribute__((aligned(8)));
static char stack_memory[STACK_SIZE] __attribute__((aligned(8)));
#endif

typedef struct memblk {
    struct memblk   *mnext;
    uint32_t        mlength;
} memblk_t;

static struct {
    memblk_t    *mhead;
    uint32_t    mfree;
    uint32_t    mtotal;
    uint32_t    mallocs;
    uint32_t    frees;
} memlist;

static struct {
    memblk_t    *mhead;
    uint32_t    mfree;
    uint32_t    mtotal;
} stkpool;

#define MIN_BLOCK_SIZE  (sizeof(memblk_t) + 8)
#define ROUNDUP(x, align)   (((x) + (align) - 1) & ~((align) - 1))
#define ROUNDDOWN(x, align) ((x) & ~((align) - 1))
#define MEM_ALIGNMENT       8

/* Initialize heap */
/* Initialize heap */
syscall meminit(void *heapstart, void *heapend) {
    memblk_t *block;
    uint32_t heapsize;
    
    if (heapstart == NULL || heapend == NULL || heapstart >= heapend) {
        return SYSERR;
    }
    
    heapstart = (void *)ROUNDUP((uintptr_t)heapstart, MEM_ALIGNMENT);
    heapend = (void *)ROUNDDOWN((uintptr_t)heapend, MEM_ALIGNMENT);
    
    heapsize = (char *)heapend - (char *)heapstart;
    
    if (heapsize < MIN_BLOCK_SIZE) {
        return SYSERR;
    }
    
    block = (memblk_t *)heapstart;
    block->mnext = NULL;
    block->mlength = heapsize;
    
    memlist.mhead = block;
    memlist.mfree = heapsize;
    memlist.mtotal = heapsize;
    memlist.mallocs = 0;
    memlist.frees = 0;
    
    return OK;
}

/* Initialize stack pool */
syscall stkinit(void *stkstart, void *stkend) {
    memblk_t *block;
    uint32_t stksize;
    
    if (stkstart == NULL || stkend == NULL || stkstart >= stkend) {
        return SYSERR;
    }
    
    stkstart = (void *)ROUNDUP((uintptr_t)stkstart, MEM_ALIGNMENT);
    stkend = (void *)ROUNDDOWN((uintptr_t)stkend, MEM_ALIGNMENT);
    
    stksize = (char *)stkend - (char *)stkstart;
    
    if (stksize < MIN_BLOCK_SIZE) {
        return SYSERR;
    }
    
    block = (memblk_t *)stkstart;
    block->mnext = NULL;
    block->mlength = stksize;
    
    stkpool.mhead = block;
    stkpool.mfree = stksize;
    stkpool.mtotal = stksize;
    
    return OK;
}

/* Initialize with default memory regions */
void mem_init_default(void) {
#ifndef __LINKER_DEFINED_HEAP__
    meminit(heap_memory, heap_memory + HEAP_SIZE);
    stkinit(stack_memory, stack_memory + STACK_SIZE);
#else
    meminit(&_heap_start, &_heap_end);
    stkinit(&_stack_start, &_stack_end);
#endif
}

/* Allocate heap memory (first-fit) */
void *getmem(uint32_t nbytes) {
    intmask mask;
    memblk_t *prev, *curr, *leftover;
    uint32_t length;
    
    if (nbytes == 0) {
        return (void *)SYSERR;
    }
    
    length = ROUNDUP(nbytes + sizeof(memblk_t), MEM_ALIGNMENT);
    
    mask = disable();
    
    prev = NULL;
    curr = memlist.mhead;
    
    while (curr != NULL) {
        if (curr->mlength >= length) {
            if (curr->mlength >= length + MIN_BLOCK_SIZE) {
                leftover = (memblk_t *)((char *)curr + length);
                leftover->mnext = curr->mnext;
                leftover->mlength = curr->mlength - length;
                
                curr->mlength = length;
                
                if (prev != NULL) {
                    prev->mnext = leftover;
                } else {
                    memlist.mhead = leftover;
                }
            } else {
                /* Use entire block */
                if (prev != NULL) {
                    prev->mnext = curr->mnext;
                } else {
                    memlist.mhead = curr->mnext;
                }
            }
            
            memlist.mfree -= curr->mlength;
            memlist.mallocs++;
            
            restore(mask);
            
            /* Return pointer past header */
            return (void *)((char *)curr + sizeof(memblk_t));
        }
        
        prev = curr;
        curr = curr->mnext;
    }
    
    restore(mask);
    return (void *)SYSERR;  /* No suitable block found */
}

/**
 * freemem - Free previously allocated heap memory
 * 
 * @param block: Pointer to memory block (from getmem)
 * @param nbytes: Number of bytes to free (must match allocation)
 * 
 * Returns: OK on success, SYSERR on error
 * 
 * Coalesces adjacent free blocks.
 */
syscall freemem(void *block, uint32_t nbytes) {
    intmask mask;
    memblk_t *blk, *prev, *curr, *next;
    uint32_t length;
    
    if (block == NULL || nbytes == 0) {
        return SYSERR;
    }
    
    /* Get block header */
    blk = (memblk_t *)((char *)block - sizeof(memblk_t));
    length = ROUNDUP(nbytes + sizeof(memblk_t), MEM_ALIGNMENT);
    
    /* Validate block length matches */
    if (blk->mlength != length) {
        /* Size mismatch - use stored size */
        length = blk->mlength;
    }
    
    mask = disable();
    
    /* Find insertion point (maintain sorted order by address) */
    prev = NULL;
    curr = memlist.mhead;
    
    while (curr != NULL && curr < blk) {
        prev = curr;
        curr = curr->mnext;
    }
    
    /* Insert into free list */
    if (prev == NULL) {
        /* Insert at head */
        blk->mnext = memlist.mhead;
        memlist.mhead = blk;
    } else {
        blk->mnext = curr;
        prev->mnext = blk;
    }
    
    /* Try to coalesce with next block */
    next = blk->mnext;
    if (next != NULL && (char *)blk + blk->mlength == (char *)next) {
        blk->mlength += next->mlength;
        blk->mnext = next->mnext;
    }
    
    /* Try to coalesce with previous block */
    if (prev != NULL && (char *)prev + prev->mlength == (char *)blk) {
        prev->mlength += blk->mlength;
        prev->mnext = blk->mnext;
    }
    
    memlist.mfree += length;
    memlist.frees++;
    
    restore(mask);
    return OK;
}

/**
 * getbuf - Allocate aligned buffer memory
 * 
 * @param nbytes: Number of bytes to allocate
 * @param align: Required alignment (must be power of 2)
 * 
 * Returns: Aligned pointer, or NULL on error
 */
void *getbuf(uint32_t nbytes, uint32_t align) {
    void *ptr, *aligned;
    uint32_t extra;
    
    if (nbytes == 0 || align == 0 || (align & (align - 1)) != 0) {
        return NULL;
    }
    
    /* Allocate extra space for alignment */
    extra = align + sizeof(void *);
    ptr = getmem(nbytes + extra);
    
    if (ptr == (void *)SYSERR) {
        return NULL;
    }
    
    /* Align the pointer */
    aligned = (void *)ROUNDUP((uintptr_t)ptr + sizeof(void *), align);
    
    /* Store original pointer before aligned address */
    *((void **)aligned - 1) = ptr;
    
    return aligned;
}

/**
 * freebuf - Free aligned buffer memory
 * 
 * @param buf: Pointer to aligned buffer (from getbuf)
 * @param nbytes: Size of buffer
 * @param align: Alignment used in allocation
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall freebuf(void *buf, uint32_t nbytes, uint32_t align) {
    void *ptr;
    uint32_t extra;
    
    if (buf == NULL) {
        return SYSERR;
    }
    
    /* Retrieve original pointer */
    ptr = *((void **)buf - 1);
    extra = align + sizeof(void *);
    
    return freemem(ptr, nbytes + extra);
}

/*------------------------------------------------------------------------
 * Stack Memory Allocation
 *------------------------------------------------------------------------*/

/**
 * getstk - Allocate stack memory from stack pool
 * 
 * @param nbytes: Stack size in bytes
 * 
 * Returns: Pointer to TOP of stack (highest address), or SYSERR
 * 
 * Note: Stacks grow downward, so we return the high address.
 */
void *getstk(uint32_t nbytes) {
    intmask mask;
    memblk_t *prev, *curr, *leftover;
    uint32_t length;
    void *stktop;
    
    if (nbytes == 0) {
        return (void *)SYSERR;
    }
    
    length = ROUNDUP(nbytes + sizeof(memblk_t), MEM_ALIGNMENT);
    
    mask = disable();
    
    /* First-fit search */
    prev = NULL;
    curr = stkpool.mhead;
    
    while (curr != NULL) {
        if (curr->mlength >= length) {
            if (curr->mlength >= length + MIN_BLOCK_SIZE) {
                /* Split block - take from high end */
                leftover = curr;
                leftover->mlength -= length;
                
                curr = (memblk_t *)((char *)leftover + leftover->mlength);
                curr->mlength = length;
            } else {
                /* Use entire block */
                if (prev != NULL) {
                    prev->mnext = curr->mnext;
                } else {
                    stkpool.mhead = curr->mnext;
                }
            }
            
            stkpool.mfree -= curr->mlength;
            
            restore(mask);
            
            /* Return top of stack */
            stktop = (char *)curr + curr->mlength - sizeof(memblk_t);
            return stktop;
        }
        
        prev = curr;
        curr = curr->mnext;
    }
    
    restore(mask);
    return (void *)SYSERR;
}

/**
 * freestk - Free stack memory back to stack pool
 * 
 * @param stktop: Top of stack (from getstk)
 * @param nbytes: Stack size
 * 
 * Returns: OK on success, SYSERR on error
 */
syscall freestk(void *stktop, uint32_t nbytes) {
    intmask mask;
    memblk_t *blk, *prev, *curr, *next;
    uint32_t length;
    
    if (stktop == NULL || nbytes == 0) {
        return SYSERR;
    }
    
    length = ROUNDUP(nbytes + sizeof(memblk_t), MEM_ALIGNMENT);
    
    /* Calculate block start from stack top */
    blk = (memblk_t *)((char *)stktop - length + sizeof(memblk_t));
    blk->mlength = length;
    
    mask = disable();
    
    /* Insert into free list (sorted by address) */
    prev = NULL;
    curr = stkpool.mhead;
    
    while (curr != NULL && curr < blk) {
        prev = curr;
        curr = curr->mnext;
    }
    
    if (prev == NULL) {
        blk->mnext = stkpool.mhead;
        stkpool.mhead = blk;
    } else {
        blk->mnext = curr;
        prev->mnext = blk;
    }
    
    /* Coalesce with next */
    next = blk->mnext;
    if (next != NULL && (char *)blk + blk->mlength == (char *)next) {
        blk->mlength += next->mlength;
        blk->mnext = next->mnext;
    }
    
    /* Coalesce with previous */
    if (prev != NULL && (char *)prev + prev->mlength == (char *)blk) {
        prev->mlength += blk->mlength;
        prev->mnext = blk->mnext;
    }
    
    stkpool.mfree += length;
    
    restore(mask);
    return OK;
}

/*------------------------------------------------------------------------
 * Memory Information and Utilities
 *------------------------------------------------------------------------*/

/**
 * memfree - Get amount of free heap memory
 * 
 * Returns: Free memory in bytes
 */
uint32_t memfree(void) {
    return memlist.mfree;
}

/**
 * memtotal - Get total heap size
 * 
 * Returns: Total heap memory in bytes
 */
uint32_t memtotal(void) {
    return memlist.mtotal;
}

/**
 * memused - Get used heap memory
 * 
 * Returns: Used memory in bytes
 */
uint32_t memused(void) {
    return memlist.mtotal - memlist.mfree;
}

/**
 * stkfree - Get free stack pool memory
 * 
 * Returns: Free stack memory in bytes
 */
uint32_t stkfree(void) {
    return stkpool.mfree;
}

/**
 * stktotal - Get total stack pool size
 * 
 * Returns: Total stack pool in bytes
 */
uint32_t stktotal(void) {
    return stkpool.mtotal;
}

/**
 * memcount_blocks - Count free blocks in heap
 * 
 * Returns: Number of free blocks
 */
int32_t memcount_blocks(void) {
    memblk_t *curr;
    int32_t count = 0;
    intmask mask;
    
    mask = disable();
    
    curr = memlist.mhead;
    while (curr != NULL) {
        count++;
        curr = curr->mnext;
    }
    
    restore(mask);
    return count;
}

/**
 * memlargest - Find largest free block
 * 
 * Returns: Size of largest free block (usable, not including header)
 */
uint32_t memlargest(void) {
    memblk_t *curr;
    uint32_t largest = 0;
    intmask mask;
    
    mask = disable();
    
    curr = memlist.mhead;
    while (curr != NULL) {
        if (curr->mlength > largest) {
            largest = curr->mlength;
        }
        curr = curr->mnext;
    }
    
    restore(mask);
    
    /* Subtract header size */
    if (largest >= sizeof(memblk_t)) {
        return largest - sizeof(memblk_t);
    }
    return 0;
}

/**
 * memcopy - Copy memory regions
 * 
 * @param dest: Destination address
 * @param src: Source address
 * @param nbytes: Number of bytes to copy
 */
void memcopy(void *dest, const void *src, uint32_t nbytes) {
    char *d = (char *)dest;
    const char *s = (const char *)src;
    
    /* Handle overlapping regions */
    if (d < s) {
        while (nbytes-- > 0) {
            *d++ = *s++;
        }
    } else if (d > s) {
        d += nbytes;
        s += nbytes;
        while (nbytes-- > 0) {
            *--d = *--s;
        }
    }
}

/**
 * memset_block - Fill memory with a value
 * 
 * @param dest: Destination address
 * @param value: Byte value to fill
 * @param nbytes: Number of bytes to fill
 */
void memset_block(void *dest, uint8_t value, uint32_t nbytes) {
    uint8_t *d = (uint8_t *)dest;
    
    while (nbytes-- > 0) {
        *d++ = value;
    }
}

/**
 * memzero - Zero out memory region
 * 
 * @param dest: Destination address
 * @param nbytes: Number of bytes to zero
 */
void memzero(void *dest, uint32_t nbytes) {
    memset_block(dest, 0, nbytes);
}

/**
 * meminfo - Print memory subsystem information
 */
void meminfo(void) {
    kprintf("\n===== Memory Information =====\n");
    kprintf("Heap Memory:\n");
    kprintf("  Total:      %lu bytes\n", memlist.mtotal);
    kprintf("  Free:       %lu bytes\n", memlist.mfree);
    kprintf("  Used:       %lu bytes\n", memlist.mtotal - memlist.mfree);
    kprintf("  Free blocks: %ld\n", memcount_blocks());
    kprintf("  Largest block: %lu bytes\n", memlargest());
    kprintf("  Allocations: %lu\n", memlist.mallocs);
    kprintf("  Frees:       %lu\n", memlist.frees);
    kprintf("\nStack Pool:\n");
    kprintf("  Total:      %lu bytes\n", stkpool.mtotal);
    kprintf("  Free:       %lu bytes\n", stkpool.mfree);
    kprintf("  Used:       %lu bytes\n", stkpool.mtotal - stkpool.mfree);
    kprintf("==============================\n\n");
}

/*------------------------------------------------------------------------
 * Memory Debugging
 *------------------------------------------------------------------------*/

#ifdef DEBUG_MEMORY

/**
 * memdump_list - Dump the free list for debugging
 */
void memdump_list(void) {
    memblk_t *curr;
    int i = 0;
    intmask mask;
    
    mask = disable();
    
    kprintf("\nHeap Free List:\n");
    curr = memlist.mhead;
    while (curr != NULL) {
        kprintf("  [%d] addr=%p size=%lu next=%p\n",
                i++, curr, curr->mlength, curr->mnext);
        curr = curr->mnext;
    }
    
    kprintf("\nStack Free List:\n");
    i = 0;
    curr = stkpool.mhead;
    while (curr != NULL) {
        kprintf("  [%d] addr=%p size=%lu next=%p\n",
                i++, curr, curr->mlength, curr->mnext);
        curr = curr->mnext;
    }
    
    restore(mask);
}

/**
 * memvalidate - Validate free list integrity
 * 
 * Returns: OK if valid, SYSERR if corruption detected
 */
syscall memvalidate(void) {
    memblk_t *prev, *curr;
    intmask mask;
    
    mask = disable();
    
    prev = NULL;
    curr = memlist.mhead;
    
    while (curr != NULL) {
        /* Check for out-of-order addresses */
        if (prev != NULL && curr <= prev) {
            kprintf("ERROR: Free list not sorted at %p\n", curr);
            restore(mask);
            return SYSERR;
        }
        
        /* Check for overlapping blocks */
        if (prev != NULL) {
            if ((char *)prev + prev->mlength > (char *)curr) {
                kprintf("ERROR: Overlapping blocks at %p\n", prev);
                restore(mask);
                return SYSERR;
            }
        }
        
        /* Check minimum size */
        if (curr->mlength < sizeof(memblk_t)) {
            kprintf("ERROR: Block too small at %p\n", curr);
            restore(mask);
            return SYSERR;
        }
        
        prev = curr;
        curr = curr->mnext;
    }
    
    restore(mask);
    return OK;
}

#endif /* DEBUG_MEMORY */

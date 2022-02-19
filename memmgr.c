//----------------------------------------------------------------
//
// Few features added by Bhathiya Perera
//
// Added memmgr_realloc, memmgr_calloc
// Should not crash if NULL is passed to memmgr_free
//----------------------------------------------------------------
// Statically-allocated memory manager
//
// by Eli Bendersky (eliben@gmail.com)
//
// This code is in the public domain.
//----------------------------------------------------------------
#include "memmgr.h"
#include <memory.h>
#include <stdio.h>

union mem_header_union {
    struct
    {
        // Pointer to the next block in the free list
        //
        union mem_header_union *next;

        // Size of the block (in quantas of sizeof(mem_header_t))
        //
        memmgr_int_t size;
    } s;

    // Used to align headers in memory to a boundary
    //
    memmgr_int_t align_dummy_;
};

typedef union mem_header_union mem_header_t;

// Initial empty list
//
static mem_header_t base;

// Start of free list
//
static mem_header_t *freep = 0;

// Static pool for new allocations
//
static uint8_t real_pool[POOL_SIZE] = {0};
static uint8_t *pool = real_pool;
static memmgr_int_t pool_free_pos = 0;


void memmgr_init() {
    base.s.next = 0;
    base.s.size = 0;
    freep = 0;
    pool_free_pos = 0;
}


void memmgr_print_stats() {
#ifdef DEBUG_MEMMGR_SUPPORT_STATS
    mem_header_t *p;

    printf("------ Memory manager stats ------\n\n");
    printf("Pool: free_pos = %llu (%llu bytes left)\n\n",
           pool_free_pos, POOL_SIZE - pool_free_pos);

    p = (mem_header_t *) pool;

    while (p < (mem_header_t *) (pool + pool_free_pos)) {
        printf("  * Addr: %p; Size: %8llu\n",
               p, p->s.size);

        p += p->s.size;
    }

    printf("\nFree list:\n\n");

    if (freep) {
        p = freep;

        while (1) {
            printf("  * Addr: %p; Size: %8llu; Next: %p\n",
                   p, p->s.size, p->s.next);

            p = p->s.next;

            if (p == freep)
                break;
        }
    } else {
        printf("Empty\n");
    }

    printf("\n");
#endif// DEBUG_MEMMGR_SUPPORT_STATS
}


static mem_header_t *get_mem_from_pool(memmgr_int_t nquantas) {
    memmgr_int_t total_req_size;
    if (nquantas < MIN_POOL_ALLOC_QUANTAS)
        nquantas = MIN_POOL_ALLOC_QUANTAS;

    total_req_size = nquantas * sizeof(mem_header_t);

    if (pool_free_pos + total_req_size <= POOL_SIZE) {
        mem_header_t *h;
        h = (mem_header_t *) (pool + pool_free_pos);
        h->s.size = nquantas;
        memmgr_free((void *) (h + 1));
        pool_free_pos += total_req_size;
    } else {
        return 0;
    }

    return freep;
}


// Allocations are done in 'quantas' of header size.
// The search for a free block of adequate size begins at the point 'freep'
// where the last block was found.
// If a too-big block is found, it is split and the tail is returned (this
// way the header of the original needs only to have its size adjusted).
// The pointer returned to the user points to the free space within the block,
// which begins one quanta after the header.
//
void *memmgr_alloc(memmgr_int_t nbytes) {
    mem_header_t *p;
    mem_header_t *prevp;

    // Calculate how many quantas are required: we need enough to house all
    // the requested bytes, plus the header. The -1 and +1 are there to make sure
    // that if nbytes is a multiple of nquantas, we don't allocate too much
    //
    memmgr_int_t nquantas = (nbytes + sizeof(mem_header_t) - 1) / sizeof(mem_header_t) + 1;

    // First alloc call, and no free list yet ? Use 'base' for an initial
    // denegerate block of size 0, which points to itself
    //
    if ((prevp = freep) == 0) {
        base.s.next = freep = prevp = &base;
        base.s.size = 0;
    }

    for (p = prevp->s.next;; prevp = p, p = p->s.next) {
        // big enough ?
        if (p->s.size >= nquantas) {
            // exactly ?
            if (p->s.size == nquantas) {
                // just eliminate this block from the free list by pointing
                // its prev's next to its next
                //
                prevp->s.next = p->s.next;
            } else// too big
            {
                p->s.size -= nquantas;
                p += p->s.size;
                p->s.size = nquantas;
            }

            freep = prevp;
            return (void *) (p + 1);
        }
        // Reached end of free list ?
        // Try to allocate the block from the pool. If that succeeds,
        // get_mem_from_pool adds the new block to the free list and
        // it will be found in the following iterations. If the call
        // to get_mem_from_pool doesn't succeed, we've run out of
        // memory
        //
        else if (p == freep) {
            if ((p = get_mem_from_pool(nquantas)) == 0) {
#ifdef DEBUG_MEMMGR_FATAL
                printf("!! Memory allocation failed !!\n");
#endif
                return 0;
            }
        }
    }
}


// Scans the free list, starting at freep, looking the the place to insert the
// free block. This is either between two existing blocks or at the end of the
// list. In any case, if the block being freed is adjacent to either neighbor,
// the adjacent blocks are combined.
//
void memmgr_free(void *ap) {
    if (NULL == ap) {
        return;
    }
    mem_header_t *block;
    mem_header_t *p;

    // acquire pointer to block header
    block = ((mem_header_t *) ap) - 1;

    // Find the correct place to place the block in (the free list is sorted by
    // address, increasing order)
    //
    for (p = freep; !(block > p && block < p->s.next); p = p->s.next) {
        // Since the free list is circular, there is one link where a
        // higher-addressed block points to a lower-addressed block.
        // This condition checks if the block should be actually
        // inserted between them
        //
        if (p >= p->s.next && (block > p || block < p->s.next))
            break;
    }

    // Try to combine with the higher neighbor
    //
    if (block + block->s.size == p->s.next) {
        block->s.size += p->s.next->s.size;
        block->s.next = p->s.next->s.next;
    } else {
        block->s.next = p->s.next;
    }

    // Try to combine with the lower neighbor
    //
    if (p + p->s.size == block) {
        p->s.size += block->s.size;
        p->s.next = block->s.next;
    } else {
        p->s.next = block;
    }

    freep = p;
}

void *memmgr_realloc(void *ap, memmgr_int_t nbytes) {
    if (NULL == ap) {
        return memmgr_alloc(nbytes);
    } else if (0 == nbytes) {
        memmgr_free(ap);
        return NULL;
    }

    mem_header_t *block;

    block = ((mem_header_t *) ap) - 1;
    memmgr_int_t expected = (nbytes + sizeof(mem_header_t) - 1) / sizeof(mem_header_t) + 1;

    if (expected <= block->s.size) {
        return ap;
    } else {
        void *ptrNew = memmgr_alloc(nbytes);
        memmgr_int_t originalLength = (block->s.size - 1) * sizeof(mem_header_t);
        if (ptrNew) {
            memcpy(ptrNew, ap, originalLength);
            memmgr_free(ap);
        }
        return ptrNew;
    }
}

void *memmgr_calloc(memmgr_int_t num, memmgr_int_t nbytes) {
    memmgr_int_t n = num * nbytes;
    void *p = memmgr_alloc(n);
    if (NULL != p) {
        memset(p, 0, n);
    }
    return p;
}

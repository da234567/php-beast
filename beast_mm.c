
/* +--------------------------------+
 * | share memory manager algorithm |
 * +--------------------------------+
 */

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/types.h>
#ifdef PHP_WIN32
#include <Windows.h>
#else
#include <sys/mman.h>
#endif

#include "spinlock.h"
#include "beast_log.h"
#include "shm.h"

#define BEAST_SEGMENT_DEFAULT_SIZE (256 * 1024)

#define _BLOCKAT(offset)  ((beast_block_t *)((char *)shmaddr + (offset)))
#define _OFFSET(block)    ((int)(((char *)(block)) - (char *)shmaddr))

#ifdef max
#undef max
#endif
#define max(a, b) ((a) > (b) ? (a) : (b))


typedef struct beast_header_s {
    int segsize;    /* size of entire segment */
    int avail;      /* bytes available memorys */
} beast_header_t;


typedef struct beast_block_s {
    int size;       /* size of this block */
    int next;       /* offset in segment of next free block */
} beast_block_t;


static int beast_mm_initialized = 0;
static void *beast_mm_block = NULL;
static int beast_mm_block_size = 0;
static beast_atomic_t *mm_lock;
static int mm_pid = -1;


void beast_mm_lock()
{
    if (mm_pid == -1) {
        mm_pid = (int)getpid();
    }
    beast_spinlock(mm_lock, mm_pid);
}


void beast_mm_unlock()
{
    if (mm_pid == -1) {
        mm_pid = (int)getpid();
    }
    beast_spinunlock(mm_lock, mm_pid);
}


/*
 * memory align function
 * @param bits, align bits
 */
static inline int beast_mm_alignmem(int bits)
{
    typedef union {
        void* p;
        int i;
        long l;
        double d;
        void (*f)();
    } beast_word_t; /* may be 8 bits */

    return sizeof(beast_word_t) * (1 + ((bits - 1) / sizeof(beast_word_t)));
}

static int beast_mm_allocate(void *shmaddr, int size)
{
    beast_header_t *header;       /* header of shared memory segment */
    beast_block_t *prv;           /* block prior to working block */
    beast_block_t *cur;           /* working block in list */
    beast_block_t *prvbestfit;    /* block before best fit */
    int realsize;                 /* actual size of block needed, including header */
    int minsize;                  /* for finding best fit */

    /* Realsize must be aligned to a word boundary on some architectures. */
    realsize = beast_mm_alignmem(
             max(size + beast_mm_alignmem(sizeof(int)), sizeof(beast_block_t)));

    /*
     * First, insure that the segment contains at least realsize free bytes,
     * even if they are not contiguous.
     */
    header = (beast_header_t *)shmaddr;
    if (header->avail < realsize) {
        beast_write_log(beast_log_error,
                        "Not enough memory for beast_mm_alloc()");
        return -1;
    }

    prvbestfit = 0;    /* best block prev's node */
    minsize = INT_MAX;

    prv = _BLOCKAT(sizeof(beast_header_t)); /* free list header */

    while (prv->next != 0) {
        cur = _BLOCKAT(prv->next); /* current active block */
        if (cur->size == realsize) {
            prvbestfit = prv;
            break;
        }
        else if (cur->size > (sizeof(beast_block_t) + realsize) &&
                 cur->size < minsize)
        {
            prvbestfit = prv;
            minsize = cur->size;
        }
        prv = cur;
    }

    if (prvbestfit == 0) { /* not found best block */
        return -1;
    }

    prv = prvbestfit;
    cur = _BLOCKAT(prv->next);

    /* update the block header */
    header->avail -= realsize;

    if (cur->size == realsize) {
        prv->next = cur->next;
    } else {
        beast_block_t *nxt;   /* the new block (chopped part of cur) */
        int nxtoffset;        /* offset of the block currently after cur */
        int oldsize;          /* size of cur before split */

        /* bestfit is too big; split it into two smaller blocks */
        nxtoffset = cur->next;
        oldsize = cur->size;
        prv->next += realsize;
        cur->size = realsize;
        nxt = _BLOCKAT(prv->next);
        nxt->next = nxtoffset;
        nxt->size = oldsize - realsize;
    }

    return _OFFSET(cur) + beast_mm_alignmem(sizeof(int)); /* skip size field */
}


static int beast_mm_deallocate(void *shmaddr, int offset)
{
    beast_header_t *header;   /* header of shared memory segment */
    beast_block_t *cur;       /* the new block to insert */
    beast_block_t *prv;       /* the block before cur */
    beast_block_t *nxt;       /* the block after cur */
    int size;                 /* size of deallocated block */

    offset -= beast_mm_alignmem(sizeof(int)); /* really offset */

    /* find position of new block in free list */
    prv = _BLOCKAT(sizeof(beast_header_t));
    while (prv->next != 0 && prv->next < offset) {
        prv = _BLOCKAT(prv->next);
    }

    /* insert new block after prv */
    cur = _BLOCKAT(offset);
    cur->next = prv->next;
    prv->next = offset;

    /* update the block header */
    header = (beast_header_t *)shmaddr;
    header->avail += cur->size;
    size = cur->size;

    if (((char *)prv) + prv->size == (char *) cur) {
        /* cur and prv share an edge, combine them */
        prv->size += cur->size;
        prv->next = cur->next;
        cur = prv;
    }

    nxt = _BLOCKAT(cur->next);
    if (((char *)cur) + cur->size == (char *) nxt) {
        /* cur and nxt shared an edge, combine them */
        cur->size += nxt->size;
        cur->next = nxt->next;
    }

    return size;
}


/*
 * init memory manager
 */
int beast_mm_init(int block_size)
{
    beast_header_t *header;
    beast_block_t *block;
    void *shmaddr;

    if (beast_mm_initialized) {
        return 0;
    }

    /* init memory manager lock */
    mm_lock = (int *)beast_shm_alloc(sizeof(int));
    if (!mm_lock) {
        beast_write_log(beast_log_error,
                        "Unable alloc share memory for memory manager lock");
        return -1;
    }
    *mm_lock = 0;

    /* init share memory for beast */
    if (block_size < BEAST_SEGMENT_DEFAULT_SIZE) {
        beast_mm_block_size = BEAST_SEGMENT_DEFAULT_SIZE;
    } else {
        beast_mm_block_size = block_size;
    }

    shmaddr = beast_mm_block = (void *)beast_shm_alloc(beast_mm_block_size);
    if (!beast_mm_block) {
        beast_write_log(beast_log_error,
                        "Unable alloc share memory for beast");
        beast_shm_free((void *)mm_lock, sizeof(int));
        return -1;
    }

    header = (beast_header_t *)beast_mm_block;
    header->segsize = beast_mm_block_size;
    /* avail size */
    header->avail = beast_mm_block_size
                   - sizeof(beast_header_t)
                   - sizeof(beast_block_t)
                   - beast_mm_alignmem(sizeof(int));

    /* the free list head block node */
    block = _BLOCKAT(sizeof(beast_header_t));
    block->size = 0;
    block->next = sizeof(beast_header_t) + sizeof(beast_block_t);

    /* the avail block */
    block = _BLOCKAT(block->next);
    block->size = header->avail;
    block->next = 0;

    beast_mm_initialized = 1;

    return 0;
}


void *beast_mm_malloc(int size)
{
    int offset;
    void *p = NULL;

    beast_mm_lock();

    offset = beast_mm_allocate(beast_mm_block, size);
    if (offset != -1) {
        p = (void *)(((char *)beast_mm_block) + offset);
    }

    beast_mm_unlock();

    return p;
}


void *beast_mm_calloc(int size)
{
    int offset;
    void *p = NULL;

    beast_mm_lock();

    offset = beast_mm_allocate(beast_mm_block, size);
    if (offset != -1) {
        p = (void *)(((char *)beast_mm_block) + offset);
    }

    beast_mm_unlock();

    if (NULL != p) {
        memset(p, 0, size);
    }

    return p;
}


void beast_mm_free(void *p)
{
    int offset;

    offset = (unsigned int)((char *)p - (char *)beast_mm_block);
    if (offset <= 0) {
        return;
    }

    beast_mm_lock();
    beast_mm_deallocate(beast_mm_block, offset);
    beast_mm_unlock();
}


void beast_mm_flush()
{
    beast_header_t *header;
    beast_block_t *block;
    void *shmaddr;

    beast_mm_lock();

    shmaddr = beast_mm_block;
    header = (beast_header_t *)shmaddr;
    header->avail = beast_mm_block_size
                   - sizeof(beast_header_t)
                   - sizeof(beast_block_t)
                   - beast_mm_alignmem(sizeof(int));

    /* the free list head block node */
    block = _BLOCKAT(sizeof(beast_header_t));
    block->size = 0;
    block->next = sizeof(beast_header_t) + sizeof(beast_block_t);

    /* the avail block */
    block = _BLOCKAT(block->next);
    block->size = header->avail;
    block->next = 0;

    beast_mm_unlock();
}


/*
 * Get the avail's memory space
 */
int beast_mm_availspace()
{
    int size;
    beast_header_t *header = (beast_header_t *)beast_mm_block;

    beast_mm_lock();
    size = header->avail;
    beast_mm_unlock();

    return size;
}


/*
 * Don't locked here, because the segsize not change forever
 */
int beast_mm_realspace()
{
    return ((beast_header_t *)beast_mm_block)->segsize;
}


/*
 * Destroy memory's manager
 */
void beast_mm_destroy()
{
    if (beast_mm_initialized) {
        beast_shm_free((void *)beast_mm_block, beast_mm_block_size);
        beast_shm_free((void *)mm_lock, sizeof(int));
        beast_mm_initialized = 0;
    }
}

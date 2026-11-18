/*
 * state_heap.c  (ForkAde) The memory the host's state asks for (the ROM's blocks, Sega's libraries' textures and
 * meshes, the sound chip's state), in a region of its own at a fixed address, so that a save state keeps it whole: its
 * bytes up to the highest ever used, the allocator's own bookkeeping included (it lives at the region's start). First
 * fit over a free list, each block with its size before and after it (so freed neighbours join); deterministic, as the
 * game is.
 */

#include "state_heap.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define HEAP_BASE 0x20000000u
#define HEAP_SIZE (96u << 20)  /* (a fight uses about 8 MB; the 32-bit address space is shared with its shadow) */
#define ALIGN 16u
#define COMMIT_STEP (4u << 20)

typedef struct {
    uint32_t size;      /* the whole block's bytes, header and footer included; the low bit: in use */
    uint32_t prev_free; /* (a free block's) offsets in the free list, 0: none */
    uint32_t next_free;
    uint32_t pad;
} BLOCK;

typedef struct {
    uint32_t top;       /* the first byte never handed out (an offset from the base) */
    uint32_t committed; /* the bytes committed so far */
    uint32_t free_head; /* the free list's first block, 0: none */
    uint32_t used;      /* bytes in use, for the logs */
} HEAP;

static uint8_t* base;
static HEAP* heap;

#define AT(off) ((BLOCK*)(base + (off)))
#define FOOT(off, size) ((uint32_t*)(base + (off) + (size) - 4))

static int commit_to(uint32_t bytes) {
#ifdef _WIN32
    while (heap->committed < bytes) {
        uint32_t more = heap->committed + COMMIT_STEP;
        if (more > HEAP_SIZE || VirtualAlloc(base + heap->committed, COMMIT_STEP, MEM_COMMIT, PAGE_READWRITE) == NULL) {
            return 0;
        }
        heap->committed = more;
    }
#endif
    return 1;
}

int state_heap_init(void) {
    if (base != NULL) {
        return 1;
    }
#ifdef _WIN32
    base = VirtualAlloc((void*)(uintptr_t)HEAP_BASE, HEAP_SIZE, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (base == NULL || VirtualAlloc(base, COMMIT_STEP, MEM_COMMIT, PAGE_READWRITE) == NULL) {
        fprintf(stderr, "state heap: could not reserve %u MB at %08X\n", HEAP_SIZE >> 20, HEAP_BASE);
        base = NULL;
        return 0;
    }
#endif
    heap = (HEAP*)base;
    memset(heap, 0, sizeof *heap);
    heap->committed = COMMIT_STEP;
    heap->top = ALIGN * ((sizeof(HEAP) + ALIGN - 1) / ALIGN);
    return 1;
}

static void unlink_free(uint32_t off) {
    BLOCK* b = AT(off);
    if (b->prev_free) AT(b->prev_free)->next_free = b->next_free; else heap->free_head = b->next_free;
    if (b->next_free) AT(b->next_free)->prev_free = b->prev_free;
}

static void link_free(uint32_t off, uint32_t size) {
    BLOCK* b = AT(off);
    b->size = size;
    *FOOT(off, size) = size;
    b->prev_free = 0;
    b->next_free = heap->free_head;
    if (heap->free_head) AT(heap->free_head)->prev_free = off;
    heap->free_head = off;
}

void* state_malloc(size_t bytes) {
    if ((base == NULL && !state_heap_init()) || bytes > HEAP_SIZE) {
        return NULL;
    }
    uint32_t need = (uint32_t)(((bytes + sizeof(BLOCK) + 4 + ALIGN - 1) / ALIGN) * ALIGN);
    /* first fit in the free list, split when what is left is worth a block */
    for (uint32_t off = heap->free_head; off; off = AT(off)->next_free) {
        uint32_t size = AT(off)->size;
        if (size >= need) {
            unlink_free(off);
            if (size - need >= sizeof(BLOCK) + ALIGN) {
                link_free(off + need, size - need);
                size = need;
            }
            AT(off)->size = size | 1;
            *FOOT(off, size) = size | 1;
            heap->used += size;
            return (uint8_t*)AT(off) + sizeof(BLOCK);
        }
    }
    uint32_t off = heap->top;
    if (off + need > HEAP_SIZE || !commit_to(off + need)) {
        return NULL;
    }
    heap->top = off + need;
    AT(off)->size = need | 1;
    *FOOT(off, need) = need | 1;
    heap->used += need;
    return (uint8_t*)AT(off) + sizeof(BLOCK);
}

void* state_calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) {
        return NULL;
    }
    void* p = state_malloc(n * size);
    if (p != NULL) {
        memset(p, 0, n * size);
    }
    return p;
}

int state_owns(const void* p) {
    return base != NULL && (const uint8_t*)p >= base && (const uint8_t*)p < base + HEAP_SIZE;
}

void state_free(void* p) {
    if (p == NULL) {
        return;
    }
    if (!state_owns(p)) {
        free(p); /* (from before the heap took over, or from the C library) */
        return;
    }
    uint32_t off = (uint32_t)((uint8_t*)p - base) - (uint32_t)sizeof(BLOCK);
    uint32_t size = AT(off)->size & ~1u;
    heap->used -= size;
    /* joined with the free blocks on either side */
    const uint32_t first = ALIGN * ((sizeof(HEAP) + ALIGN - 1) / ALIGN);
    if (off > first) {
        uint32_t before = *(uint32_t*)(base + off - 4);
        if (!(before & 1)) {
            uint32_t prev = off - before;
            unlink_free(prev);
            off = prev;
            size += before;
        }
    }
    if (off + size < heap->top) {
        uint32_t after = AT(off + size)->size;
        if (!(after & 1)) {
            unlink_free(off + size);
            size += after;
        }
    }
    if (off + size == heap->top) {
        heap->top = off; /* the end given back */
        return;
    }
    link_free(off, size);
}

void* state_heap_base(void) {
    return base;
}

uint32_t state_heap_top(void) {
    return heap != NULL ? heap->top : 0;
}

/* (a save state put back: the region up to its top as it was; the committed pages are made to cover it) */
int state_heap_restore(const void* bytes, uint32_t top) {
    if (base == NULL && !state_heap_init()) {
        return 0;
    }
    if (!commit_to(top)) {
        return 0;
    }
    uint32_t committed = heap->committed;
    memcpy(base, bytes, top);
    heap->committed = committed; /* (the pages committed are this run's own) */
    return 1;
}

uint32_t state_heap_committed(void) {
    return heap != NULL ? heap->committed : 0;
}

uint32_t state_heap_reserved(void) {
    return HEAP_SIZE;
}

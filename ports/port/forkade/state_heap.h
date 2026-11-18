/*
 * state_heap.h  (ForkAde) The memory the host's state asks for, in a region a save state keeps whole (state_heap.c)
 */

#ifndef FORKADE_STATE_HEAP_H
#define FORKADE_STATE_HEAP_H

#include <stddef.h>
#include <stdint.h>

int state_heap_init(void);
void* state_malloc(size_t bytes);
void* state_calloc(size_t n, size_t size);
void state_free(void* p);
int state_owns(const void* p);
void* state_heap_base(void);
uint32_t state_heap_top(void);
int state_heap_restore(const void* bytes, uint32_t top);
uint32_t state_heap_committed(void);
uint32_t state_heap_reserved(void);

#endif

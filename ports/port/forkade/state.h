/*
 * state.h  (ForkAde) Save states between frames (state.c)
 */

#ifndef FORKADE_STATE_H
#define FORKADE_STATE_H

#include <stddef.h>
#include <stdint.h>

size_t forkade_state_size(void);
size_t forkade_state_save(void* out, size_t room); /* the bytes written, 0 if they do not fit */
int forkade_state_load(const void* in, size_t size);
uint64_t forkade_state_host_hash(void);
int forkade_state_test(unsigned frame);            /* MVC2_STATETEST=SAVE:LOAD; 1 saved, 2 loaded */
int forkade_state_save_file(const char* path);
int forkade_state_load_file(const char* path);

#endif

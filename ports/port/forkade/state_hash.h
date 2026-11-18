/*
 * state_hash.h  (ForkAde) A hash of the game's state after each frame (MVC2_HASHLOG=FILE), for the checks
 */

#ifndef FORKADE_STATE_HASH_H
#define FORKADE_STATE_HASH_H

#include <stddef.h>
#include <stdint.h>

uint64_t forkade_hash(const void* data, size_t bytes);
uint64_t forkade_ram_hash(void);
uint64_t forkade_sound_hash(void);
void forkade_hash_frame(unsigned long frame);
void forkade_hash_close(void);

#endif

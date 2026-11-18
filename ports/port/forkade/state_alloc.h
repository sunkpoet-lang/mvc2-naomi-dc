/*
 * state_alloc.h  (ForkAde) Put first in each module whose memory is part of the game's state (CMakeLists.txt, -include):
 * its allocations come from the state heap (state_heap.c), which a save state keeps whole.
 */

#ifndef FORKADE_STATE_ALLOC_H
#define FORKADE_STATE_ALLOC_H

#include <stdlib.h>
#include <malloc.h>

#include "state_heap.h"

#define host_try_malloc state_malloc
#define host_try_calloc state_calloc
#define malloc state_malloc
#define calloc state_calloc
#define free state_free

#endif

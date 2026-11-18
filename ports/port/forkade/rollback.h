/*
 * rollback.h  (ForkAde) Saves of the last frames in memory, cheap enough for each frame (rollback.c)
 */

#ifndef FORKADE_ROLLBACK_H
#define FORKADE_ROLLBACK_H

#include <stdint.h>

int forkade_rb_init(int slots);       /* how many frames back it can go */
int forkade_rb_save(uint32_t frame);  /* after the frame */
int forkade_rb_load(uint32_t frame);  /* back to a frame saved and not dropped; the later saves dropped */
void forkade_rb_stats(uint32_t* pages_saved, uint32_t* pages_loaded);
int forkade_rb_test(uint32_t frame, unsigned* clock); /* MVC2_RBTEST */
void forkade_rb_reset(void);           /* the memory put back otherwise: the saves dropped */
int forkade_rb_ready(void);

#endif

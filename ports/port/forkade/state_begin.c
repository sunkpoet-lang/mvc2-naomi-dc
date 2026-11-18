/*
 * state_begin.c  (ForkAde) Where the host's state begins: linked after the drawing layer (render/) and before the board,
 * Sega's libraries and the sound side, so that its names and the host span's end bracket the .data and .bss a save
 * state keeps (port/forkade/state.c). The drawing layer's buffers and caches stay out of it.
 */

char forkade_state_data_begin[4] = { 1 };
char forkade_state_bss_begin[4];

/*
 * bar.h  (ForkAde) The menu bar along the top of the window, as ForkAde's other games have it (bar.cpp, over
 * recomp-common's menubar.h): Game, Video, Audio, Input, Features, Help; F10 or Alt opens it, a right click opens the
 * same menus where the pointer is. The front end says what is on now (FORKADE_BAR_STATE), hands it the events, does
 * what was chosen (an id below) and has it draw itself over the window.
 */

#ifndef FORKADE_BAR_H
#define FORKADE_BAR_H

#include "../sdl/sdl_front.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    FB_NONE = 0,
    FB_QUICK_SAVE = 1, FB_QUICK_LOAD, FB_SLOT_BEFORE, FB_SLOT_NEXT, FB_SCREENSHOT, FB_RESET, FB_TEST_MENU, FB_SWITCH,
    FB_QUIT, FB_FULLSCREEN, FB_RENDERER_GPU, FB_RENDERER_SOFT, FB_TIMES, FB_CONTROLS, FB_UNLOCK, FB_MUTE, FB_LOUDER,
    FB_QUIETER, FB_ABOUT, FB_KEY, FB_REPLAY_RECORD, FB_REPLAY_PLAY, FB_REPLAY_STOP, FB_REPLAY_FOLDER,
    FB_SAVE = 100,      /* + the slot, 1 to 9 */
    FB_LOAD = 120,      /* + the slot */
    FB_SLOT = 140,      /* + the slot: the slot quick save and load use */
    FB_SCALING = 160,   /* + 0 aspect, 1 integer, 2 stretch */
    FB_VIEWER = 200,    /* + recomp::replay::Command: the replay viewer's (replays.h) */
};

typedef struct {
    int fullscreen;
    int scaling;            /* 0 aspect, 1 integer, 2 stretch */
    int gpu, gpu_works;     /* the GPU draws; it can */
    int times;              /* F3's frame times shown */
    int unlock;             /* every character open */
    int slot;               /* 1 to 9 */
    int slot_used[10];      /* a save state kept in that slot */
    int arcade;             /* the arcade's version (its test menu) */
    int can_switch;         /* both versions in this executable */
    int online;             /* what would change the match is dim */
    int muted, volume;      /* volume in percent */
    int replay_playing, replay_recording, replay_paused, replay_hud; /* (replays.h) */
} FORKADE_BAR_STATE;

/* what the menus hold; again whenever any of it changes (cheap when nothing did) */
void forkade_bar_configure(const FORKADE_BAR_STATE* state);
/* the window's size in pixels, and whether it is fullscreen (then the bar shows only when opened) */
void forkade_bar_layout(int width, int height, int fullscreen);
/* the pixels at the top the picture leaves to the bar (0 in fullscreen) */
int forkade_bar_strip(void);
/* an event, its pointer already in the renderer's pixels (SDL_ConvertEventToRenderCoordinates with no logical
   presentation): 1 when the bar took it; *chosen the id chosen, or FB_NONE */
int forkade_bar_event(const SDL_Event* e, int* chosen);
int forkade_bar_open(void);
/* the bar and its open menus over the window (renderer's pixels, no logical presentation) */
void forkade_bar_render(SDL_Renderer* renderer);
/* the renderer was made again (its textures went with the old one) */
void forkade_bar_forget(void);
/* (the checks) a bar's menu opened as F10 and the arrows would: the menu's place, then Down so many times, then Right */
void forkade_bar_test_open(int menu, int downs, int right);

/* the sound's level, 0 to 100 in steps of 5 (recomp-common's volume.h) */
int forkade_volume_step(int percent, int steps);
float forkade_volume_gain(int percent);

#ifdef __cplusplus
}
#endif

#endif

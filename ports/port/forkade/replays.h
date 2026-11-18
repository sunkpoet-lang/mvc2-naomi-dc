/*
 * replays.h  (ForkAde) MvC2's replays (replays.cpp): every versus match kept as a .fareplay (fareplay.h), recording by
 * hand, and playback through ForkAde's replay viewer (recomp-common/replay_viewer.h) with its HUD and the players'
 * strip at the top ("DANNY 1 VS 3 RODRIGO"). The arcade version's only.
 */

#ifndef FORKADE_REPLAYS_H
#define FORKADE_REPLAYS_H

#include <stdint.h>

#include "naomi_input.h"
#include "../sdl/sdl_front.h"

#ifdef __cplusplus
extern "C" {
#endif

/* what the front end does for it (main_sdl.c): one frame of the game with these controls, drawn (into the picture
   the next show() presents, not shown yet) when seen, its sound played when heard */
void mvc2_front_run_frame(const HOST_INPUT* in, int seen, int heard);

/* where the match replays are kept (the folder is made), and the players' names (the launcher's or online's) */
void forkade_replay_setup(const char* folder);
void forkade_replay_names(const char* one, const char* two);

/* after each frame the game ran by itself (not a replay's): the controls it had. Keeps a match as it is fought */
void forkade_replay_after_frame(const HOST_INPUT* used);

/* by hand (Game > Replay): from now until stopped, to PATH */
int forkade_replay_record(const char* path);
void forkade_replay_record_stop(void);
int forkade_replay_recording(void);

/* playing one: 0 and why in error when it cannot be played (another build, damaged, not this game) */
int forkade_replay_play(const char* path, char* error, int room);
int forkade_replay_playing(void);
void forkade_replay_stop(void);         /* the game put back as it was before */
/* a tick of the screen while playing: 1 when a frame was shown */
int forkade_replay_tick(void);
/* an event while playing: 1 when the viewer took it. Pointer events in the overlay's pixels (320 by 240, half the picture's) */
int forkade_replay_event(const SDL_Event* e, float picture_x, float picture_y);
/* the viewer's commands (recomp::replay::Command's values), from the menu bar */
void forkade_replay_command(int command);
int forkade_replay_paused(void);
int forkade_replay_hud_shown(void);
/* what is drawn over the picture (the strip, the HUD): ABGR pixels, w by h (320 by 240, half the picture's), cleared first;
   0 when there is nothing to draw */
int forkade_replay_overlay(uint32_t* pixels, int w, int h);
/* the game closing: a match being kept written, and the writing waited for */
void forkade_replay_quit(void);
/* the last thing to say (an error, "no longer matches"), or "" */
const char* forkade_replay_note(void);

#ifdef __cplusplus
}
#endif

#endif

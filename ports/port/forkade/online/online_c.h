/*
 * online_c.h  (ForkAde) MvC2's online play for the front end (port_mvc2.cpp, over ForkAde's session online.cpp and
 * GekkoNet): two players, rollback, spectators, the players' names and the in-game chat. The arcade version's only.
 *
 *   --gekko-host [PORT]           player 1, waiting on that UDP port (7845)
 *   --gekko-join HOST:PORT        player 2 (with --gekko-spectate, or --gekko-watch HOST:PORT: a spectator)
 *   --gekko-delay N               the local controls held back N frames (0-8; 1)
 *   --gekko-spectators N, --gekko-spectator-delay N, --nick NAME
 *   --gekko-stress                no network: GekkoNet's stress session, rolling back every frame
 *   --gekko-checkpoint-log FILE   each saved frame and the board's fingerprint (the checks')
 */

#ifndef FORKADE_ONLINE_C_H
#define FORKADE_ONLINE_C_H

#include <stdint.h>

#include "naomi_input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* before the game starts: whether the command line asks for online play (the board is then made alike on both sides:
   host_online, free play, every character open) */
int forkade_online_wanted(int argc, char** argv);
/* the session started, once the game has run its first frames (its sound's RAM is part of a state): 0 and why */
int forkade_online_start(char* error, int room);
int forkade_online_active(void);
/* one tick: this side's controls (the stress session: both players'). Frames run to be seen (0 or more) */
int forkade_online_tick(const HOST_INPUT* local);
int forkade_online_ended(void);
int forkade_online_failed(void);
const char* forkade_online_message(void);
void forkade_online_stop(void);
/* the strip (names and streaks), what the session says, the chat: ABGR pixels, w by h, cleared first; 0 none */
int forkade_online_overlay(uint32_t* pixels, int w, int h);
/* the chat's keys (T opens a line, Enter sends, Esc cancels): 1 when taken */
int forkade_online_key(int key, const char* text);

#ifdef __cplusplus
}
#endif

#endif

// (ForkAde) MvC2's port as ForkAde's online session (online.cpp) sees it: the members of the CPS port's and sf3comp's
// Port the session uses, made over MvC2's front end (port_mvc2.cpp; main_sdl.c runs the window). One, the game's.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "chat_link.h"
#include "match_link.h"
#include "online_mvc2.h"

namespace port {

struct Port {
    struct Options {
        bool hashNoPicture = false, hashNoSound = false, headless = false;
        std::string replayPlay;
    } opt;
    struct Menu {
        void Close(); // (the menu bar's)
    } menu;
    std::unique_ptr<cps2recomp::Machine> machine = std::make_unique<cps2recomp::Machine>();
    uint64_t frame = 0; // frames run (the session's)
    uint64_t tick = 0;
    uint16_t pad[4] = {};
    bool paused = false;
    int steps = 0;
    int coinFrames = 0, start2Frames = 0;
    // the match as the window shows it: who has come in (Start), the win streaks
    bool cameIn = false, otherIn = false;
    int streak[2] = {}, roundsWon[2] = {};
    bool roundCounted = false;
    int roundQuiet = 0;
    MatchLink *gekkoMatch = nullptr;
    ChatLink *gekkoChat = nullptr;
    bool gekkoOnline = false, gekkoArcadeWaiting = false;
    MatchLink *Match() const { return gekkoMatch; }

    // a frame of the session's (`again`: run again in a rollback, neither seen nor heard)
    bool RunOnlineFrame(std::span<const uint16_t> players, bool again);
    void ArcadeFrame();         // (the host's own play while it waits for a challenger)
    uint32_t StateHash() const; // (the board's fingerprint the two sides compare)
    std::string BuildIdentity() const;
    void SoundtrackFollow() {}
    void ChatTick();

    // (the chat as the window shows it)
    struct Line {
        std::string text;
        bool own = false;
        uint64_t until = 0;
    };
    std::vector<Line> chat;
};

Port &ThePort();

// (port_mvc2.cpp) The session's states: a token of rollback.c's saves, or the whole state (portable: a spectator's)
bool MvC2Save(std::vector<uint8_t> &state, bool portable);
bool MvC2Load(std::span<const uint8_t> state);
void MvC2Whole(std::vector<uint8_t> &state);

} // namespace port

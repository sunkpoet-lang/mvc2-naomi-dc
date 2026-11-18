// (ForkAde) What ForkAde's online session (online.cpp, copied from sf3comp 8e7f280, itself from the CPS fork) takes
// from the CPS port, in MvC2's terms, as sf3comp's online_sf3.h does for it: the CPS names the copy uses
// (cps2recomp::), the game's features that change a match (all characters open), the round result (the fight's, from
// vs_sys_work: one fight is a match), the controls' coin and Start, and the hello's identity (the gameplay identity).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "replay_identity.h"
#include "miniz.h"

#define GAME_ID "mvsc2"

namespace cps2recomp {

// The controls as the session carries them: the JVS bits (naomi_input.h) and the coin in player 1's free low bit
constexpr uint16_t kStart = 0x8000;
constexpr uint16_t kCoin = 0x0001;

struct Feature {
    const char *name, *title, *category;
};
struct Game {
    std::vector<Feature> features;
};
Game &TheGame();
int FeatureSetting(const char *name);
inline int FeatureSetting(const std::string &name) { return FeatureSetting(name.c_str()); }
void SetFeatureForNow(const char *name, int value);
inline bool OnlineOff(const std::string &) { return false; } // (nothing of MvC2's is each player's own)
inline void SetOnline(bool) {}

namespace battle {
    // 0 while a fight is fought or none has ended, 1 or 2 the side that won, 3 a draw (port_mvc2.cpp)
    int RoundResultNow();
    inline int RoundsToWin() { return 1; } // (a fight is the match)
} // namespace battle

using MatchResult = int (*)();
inline MatchResult OnlineMatchResult() { return nullptr; }

inline uint32_t OnlineBuildCrc() {
    const char *identity = MVC2_REPLAY_IDENTITY;
    return static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char *>(identity), std::strlen(identity)));
}
inline std::string OnlineVersion() {
    // (MVC2_TEST_VERSION: the checks' way to be another build)
    if (const char *test = std::getenv("MVC2_TEST_VERSION")) {
        return test;
    }
    return std::string(MVC2_VERSION_TEXT) + "+" + std::string(MVC2_REPLAY_IDENTITY).substr(0, 12);
}

// (the CPS port's machine: what online.cpp touches of it, its sound queue, which MvC2 plays as it goes)
struct Machine {
    std::vector<int16_t> audio;
};

} // namespace cps2recomp

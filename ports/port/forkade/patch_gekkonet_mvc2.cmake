# (ForkAde) GekkoNet for MvC2, on top of sf3comp's patch (patch_gekkonet_sf3.cmake, a copy of sf3comp's
# cps3recomp/tools/patch_gekkonet.cmake): MvC2's whole state is 52 MB, too big for GekkoNet's buffers of one size, so
# the session keeps tokens (port/forkade/online_mvc2.cpp: rollback.c's saves) and only the state a spectator joining a
# match in progress gets is whole:
#  - a hook (gekko_forkade_portable) the game sets: it turns the token GekkoNet saved as portable into the whole state,
#    and says how big a whole state may be;
#  - the host takes a spectator's state only from that portable save, never from the session's tokens;
#  - a spectator takes states up to that size;
#  - the state's chunks sent at a pace a relay carries (10 every 16 ms), and the spectators' inputs kept for 2 minutes
#    meanwhile.
# The packets are the same as sf3comp's patch makes them.
#   cmake -DSOURCE=<gekkonet source dir> -P patch_gekkonet_mvc2.cmake
if(NOT SOURCE)
    message(FATAL_ERROR "SOURCE: GekkoNet's source folder")
endif()
set(backend_cpp "${SOURCE}/GekkoLib/src/backend.cpp")
set(session_cpp "${SOURCE}/GekkoLib/src/game_session.cpp")
set(backend_h "${SOURCE}/GekkoLib/include/private/backend.h")
file(READ "${backend_cpp}" cpp)
if(cpp MATCHES "ForkAde: MvC2 portable state")
    message(STATUS "GekkoNet already patched for MvC2's portable state")
    return()
endif()
file(READ "${session_cpp}" ses)
file(READ "${backend_h}" hdr)

function(swap var old new)
    string(FIND "${${var}}" "${old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "patch_gekkonet_mvc2: not found in GekkoNet: ${old}")
    endif()
    string(REPLACE "${old}" "${new}" result "${${var}}")
    set(${var} "${result}" PARENT_SCOPE)
endfunction()

# The hook
swap(cpp "#include <zlib.h> // (ForkAde: sf3comp spectator state)"
"#include <zlib.h> // (ForkAde: sf3comp spectator state)

// (ForkAde: MvC2 portable state) The game's: a portable save's token made the whole state (in *out, kept by the game
// until the next call), and the largest a whole state may be
typedef int (*GekkoForkadeExpand)(const unsigned char* token, unsigned int len, const unsigned char** out, unsigned int* out_len);
static GekkoForkadeExpand g_forkade_expand = nullptr;
static unsigned int g_forkade_most = 0;
extern \"C\" void gekko_forkade_portable(GekkoForkadeExpand expand, unsigned int most) {
    g_forkade_expand = expand;
    g_forkade_most = most;
}
extern \"C\" int gekko_forkade_portable_on(void) {
    return g_forkade_expand != nullptr;
}
static unsigned int ForkadeMost(unsigned int state_size) {
    return g_forkade_expand != nullptr && g_forkade_most > state_size ? g_forkade_most : state_size;
}")

swap(cpp "    if (!state || state_size == 0 || state_size > _state_size) {
        return;
    }

    for (auto& player : spectators) {"
"    if (g_forkade_expand != nullptr && state != nullptr && state_size != 0) { // (ForkAde: MvC2 portable state)
        const unsigned char* whole = nullptr;
        unsigned int whole_size = 0;
        if (!g_forkade_expand(state, state_size, &whole, &whole_size) || whole == nullptr || whole_size == 0 ||
            whole_size > ForkadeMost(_state_size)) {
            return;
        }
        state = whole;
        state_size = whole_size;
    } else if (!state || state_size == 0 || state_size > _state_size) {
        return;
    }

    for (auto& player : spectators) {")

swap(cpp "        body->total_size > _state_size || body->state.empty() ||"
"        body->total_size > ForkadeMost(_state_size) || body->state.empty() || // (ForkAde: MvC2 portable state)")

swap(cpp "    if (!UnpackSpectatorState(state, _state_size)) { // (ForkAde)"
"    if (!UnpackSpectatorState(state, ForkadeMost(_state_size))) { // (ForkAde; MvC2 portable state: its size)")

# The host's spectator state from the portable save only
swap(ses "#include \"session/game_session.h\""
"#include \"session/game_session.h\"

extern \"C\" int gekko_forkade_portable_on(void); // (ForkAde: MvC2 portable state, backend.cpp)")
swap(ses "        StateEntry* state = nullptr;
        for (Frame frame = state_frame; frame >= oldest_state; frame--) {"
"        StateEntry* state = nullptr;
        // (ForkAde: MvC2 portable state) the session's saves are tokens: only the portable save will do
        for (Frame frame = state_frame; !gekko_forkade_portable_on() && frame >= oldest_state; frame--) {")

# Sent slower than sf3comp's: MvC2's state is some 10 MB packed, and 32 chunks every 16 ms (1.8 MB/s) swamped a
# relay (n0's) until neither the spectator nor the players' own packets got through; 10 (about 560 KB/s)
swap(cpp "        constexpr u32 kChunksPerSend = 32;"
"        constexpr u32 kChunksPerSend = 10; // (ForkAde: MvC2 portable state, paced for a relay)")

# The spectators' inputs kept for 2 minutes, not 10 s: a state some 10 MB packed takes longer than 10 s over a relay,
# and one older than the inputs kept is taken again and sent from the start, round and round
swap(hdr "        const u32 MAX_SPECTATOR_INPUT_QUEUE_SIZE = 600;"
"        const u32 MAX_SPECTATOR_INPUT_QUEUE_SIZE = 7200; // (ForkAde: MvC2 portable state, a relay's 2 minutes)")

file(WRITE "${backend_h}" "${hdr}")
file(WRITE "${backend_cpp}" "${cpp}")
file(WRITE "${session_cpp}" "${ses}")
message(STATUS "GekkoNet patched for MvC2's portable state")

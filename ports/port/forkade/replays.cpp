// replays.cpp  (ForkAde) MvC2's replays: replays.h.
//
// A replay is the arcade version's save state as a match began (state.c) and both players' controls frame by frame
// after it, with a checksum of the main RAM every 60 frames, in ForkAde's .fareplay format 2 (fareplay.h, backend
// "mvc2", the gameplay identity CMake makes from replay_identity.txt). A versus match (two players: Sys->vs_flag) is
// kept from its fighters' entrance to a few seconds after its end; Game > Replay records from when it is chosen.
//
// Playback is ForkAde's replay viewer (recomp-common/replay_viewer.h) over a board made of the port's save states:
//   - the viewer's rewind window (30 frames kept and put back last first): rollback.c's saves, a token each;
//   - its lasting marks (every 30 frames, 64 at most) and the board's own keyframes (every 600): the 4 KB pages of a
//     whole state that differ from the replay's first state (in a fight about 1 MB of the 52), each self-contained.

#include "replays.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "fareplay.h"
#include "recomp-common/replay_viewer.h"
#include "recomp-common/text.h"
#include "replay_identity.h"

extern "C" {
#include "naomi_host.h"
#include "rollback.h"
#include "state.h"
#include "state_hash.h"
}

namespace {

using recomp::replay::Command;

constexpr uint32_t kPage = 4096;
constexpr uint32_t kHashEvery = 60;
constexpr uint32_t kKeyEvery = 600;     // (the board's own keyframes)
constexpr uint32_t kTail = 240;         // frames kept after a match ends
constexpr uint32_t kLeast = 600;        // a match shorter than this is not kept
constexpr uint32_t kLongest = 60 * 60 * 20;
constexpr int kKeepFiles = 40;
constexpr uint16_t kCoinBit = 0x0001;   // (a coin, in player 1's free low bits)

// The fight's state (include/vs_sys.h, include/system.h)
enum { kVsAppear = 2, kVsFight = 4, kVsEndWait = 5 };
struct Fight {
    int st = 0, winner = -1, ended = 0, versus = 0;
    bool known = false;
};
Fight ReadFight() {
    Fight f;
    const HOST_PROGRAM* p = naomi_program();
    if (p == nullptr || p->vs_sys == nullptr || p->sys == nullptr || *p->sys == nullptr) {
        return f;
    }
    const auto* v = reinterpret_cast<const uint8_t*>(p->vs_sys);
    const auto* s = reinterpret_cast<const uint8_t*>(*p->sys);
    f.st = v[0x00];
    f.winner = static_cast<int8_t>(v[0x3e]);
    f.ended = v[0x46] != 0 && f.st >= kVsEndWait;
    f.versus = s[0x8b] != 0;
    f.known = true;
    return f;
}

uint32_t Checksum() {
    const uint64_t h = forkade_ram_hash() ^ (forkade_sound_hash() * 31);
    return static_cast<uint32_t>(h ^ (h >> 32));
}

std::vector<uint8_t> SaveNow() {
    std::vector<uint8_t> s(forkade_state_size());
    s.resize(forkade_state_save(s.data(), s.size()));
    return s;
}

std::string Now() {
    char text[40];
    std::time_t t = std::time(nullptr);
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%S%z", std::localtime(&t));
    return text;
}

// ---- recording ----

struct Recording {
    bool on = false, manual = false;
    std::string path;
    port::Replay replay;
    uint32_t frames = 0;
    uint16_t last[2] = {0xFFFF, 0xFFFF};
    bool lastCoin = false;
    uint32_t endedAt = 0;
    bool ended = false;
    int winner = -1;
    int scoreBefore[2] = {0, 0};
};

std::string folder;
std::string names[2] = {"", ""};
int score[2] = {0, 0}; // (matches won this session, as the online strip counts them)
Recording rec;
int lastSt = 0;
std::vector<std::thread> writers;

void Prune() {
    std::error_code e;
    std::vector<std::filesystem::directory_entry> mine;
    for (const auto& entry : std::filesystem::directory_iterator(folder, e)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("mvc2_", 0) == 0 && entry.path().extension() == ".fareplay") {
            mine.push_back(entry);
        }
    }
    if (static_cast<int>(mine.size()) <= kKeepFiles) {
        return;
    }
    std::sort(mine.begin(), mine.end(), [](const auto& a, const auto& b) { return a.path().filename() < b.path().filename(); });
    for (size_t i = 0; i + kKeepFiles < mine.size(); i++) {
        std::filesystem::remove(mine[i].path(), e);
    }
}

void Begin(bool manual, const std::string& path) {
    rec = Recording{};
    rec.on = true;
    rec.manual = manual;
    rec.path = path;
    port::Replay& r = rec.replay;
    r.backend = 0;
    r.players = 2;
    r.state = SaveNow();
    r.info.game = "mvsc2";
    r.info.title = "Marvel vs. Capcom 2";
    r.info.backend = "mvc2";
    r.info.identity = MVC2_REPLAY_IDENTITY;
    r.info.version = MVC2_VERSION_TEXT;
    r.info.build = MVC2_REPLAY_IDENTITY;
    r.info.forkade = MVC2_VERSION_TEXT;
    r.info.inputBytes = 2;
    r.info.inputPlayers = 2;
    r.info.started = Now();
    r.info.players = {names[0].empty() ? "Player 1" : names[0], names[1].empty() ? "Player 2" : names[1]};
    rec.scoreBefore[0] = score[0];
    rec.scoreBefore[1] = score[1];
    r.checks.emplace_back(0, Checksum());
}

void Finish() {
    if (!rec.on) {
        return;
    }
    rec.on = false;
    if (!rec.manual && rec.frames < kLeast) {
        return;
    }
    port::Replay r = std::move(rec.replay);
    r.info.frames = rec.frames;
    r.info.result = rec.winner == 0 || rec.winner == 1;
    r.info.wins = {score[0], score[1]};
    r.info.features = "unlock=" + std::to_string(host_unlock ? 1 : 0) + ",score0=" + std::to_string(rec.scoreBefore[0]) +
                      ",score1=" + std::to_string(rec.scoreBefore[1]) + ",winner=" + std::to_string(rec.winner);
    if (r.checks.empty() || r.checks.back().first != rec.frames) {
        r.checks.emplace_back(rec.frames, Checksum());
    }
    std::string path = rec.path;
    if (path.empty()) {
        if (folder.empty()) {
            return;
        }
        char name[64];
        std::time_t t = std::time(nullptr);
        std::strftime(name, sizeof name, "mvc2_%Y%m%d_%H%M%S.fareplay", std::localtime(&t));
        path = (std::filesystem::path(folder) / name).string();
    }
    // (packing 52 MB takes a moment: on its own thread, the game goes on; forkade_replay_quit waits for them)
    writers.emplace_back([r = std::move(r), path]() {
        std::string error;
        if (!r.Save(path, error)) {
            host_log("replay: could not write %s: %s", path.c_str(), error.c_str());
        } else {
            Prune();
        }
    });
}

// ---- playback ----

class Board;
struct Playing {
    bool on = false;
    port::Replay replay;
    std::vector<uint8_t> own; // (the game before it, put back when it stops)
    uint32_t at = 0;
    std::map<uint32_t, std::vector<uint8_t>> keys; // the board's keyframes (page differences)
    uint32_t generation = 1;  // (rollback.c's saves since its last reset; a token of another is stale)
    int scoreBefore[2] = {0, 0};
    std::string note;
    uint64_t tick = 0;
    int lastSt = 0;
    std::vector<uint8_t> scratch;
};
Playing play;
recomp::replay::Viewer viewer;
std::string lastNote;

int Feature(const std::string& features, const char* name, int otherwise) {
    const std::string key = std::string(name) + "=";
    size_t at = 0;
    while (at < features.size()) {
        const size_t end = std::min(features.find(',', at), features.size());
        if (features.compare(at, key.size(), key) == 0) {
            return atoi(features.c_str() + at + key.size());
        }
        at = end + 1;
    }
    return otherwise;
}

// The pages of a whole state that differ from the replay's first (size, then each: page number, its bytes)
std::vector<uint8_t> Difference() {
    std::vector<uint8_t>& now = play.scratch;
    now.resize(forkade_state_size());
    now.resize(forkade_state_save(now.data(), now.size()));
    const std::vector<uint8_t>& base = play.replay.state;
    std::vector<uint8_t> out(9);
    out[0] = 'D';
    const uint32_t size = static_cast<uint32_t>(now.size());
    std::memcpy(&out[1], &size, 4);
    uint32_t pages = 0;
    for (uint32_t off = 0; off < size; off += kPage) {
        const uint32_t n = std::min(kPage, size - off);
        if (off + n <= base.size() && std::memcmp(now.data() + off, base.data() + off, n) == 0) {
            continue;
        }
        const uint32_t page = off / kPage;
        const size_t at = out.size();
        out.resize(at + 4 + n);
        std::memcpy(&out[at], &page, 4);
        std::memcpy(&out[at + 4], now.data() + off, n);
        pages++;
    }
    std::memcpy(&out[5], &pages, 4);
    return out;
}

bool RestoreDifference(const std::vector<uint8_t>& d) {
    if (d.size() < 9 || d[0] != 'D') {
        return false;
    }
    uint32_t size, pages;
    std::memcpy(&size, &d[1], 4);
    std::memcpy(&pages, &d[5], 4);
    const std::vector<uint8_t>& base = play.replay.state;
    std::vector<uint8_t>& now = play.scratch;
    now.assign(size, 0);
    std::memcpy(now.data(), base.data(), std::min<size_t>(size, base.size()));
    size_t at = 9;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t page;
        std::memcpy(&page, &d[at], 4);
        const uint32_t off = page * kPage, n = std::min(kPage, size - off);
        std::memcpy(now.data() + off, &d[at + 4], n);
        at += 4 + n;
    }
    const bool ok = forkade_state_load(now.data(), now.size()) != 0;
    forkade_rb_reset();
    play.generation++;
    return ok;
}

HOST_INPUT InputsAt(uint32_t frame) {
    HOST_INPUT in{};
    auto it = play.replay.inputs.upper_bound(frame);
    if (it != play.replay.inputs.begin()) {
        --it;
        in.player[0] = it->second[0] & ~kCoinBit;
        in.player[1] = it->second[1];
        in.coin = (it->second[0] & kCoinBit) != 0;
    }
    return in;
}

class Board : public recomp::replay::Board {
public:
    uint32_t Frame() const override { return play.at; }
    uint32_t Length() const override { return static_cast<uint32_t>(play.replay.info.frames); }
    void RunFrame(bool seen, bool sound) override {
        const HOST_INPUT in = InputsAt(play.at);
        mvc2_front_run_frame(&in, seen ? 1 : 0, sound ? 1 : 0);
        play.at++;
        // (the markers: the fight's start and its end)
        const Fight f = ReadFight();
        if (f.known && f.st != play.lastSt) {
            if (f.st == kVsFight || (f.st == kVsEndWait && f.ended)) {
                viewer.AddMarker(play.at);
            }
            play.lastSt = f.st;
        }
        for (const auto& [frame, hash] : play.replay.checks) {
            if (frame == play.at && hash != Checksum() && play.note.empty()) {
                play.note = "No longer matches (frame " + std::to_string(frame) + ")";
                host_log("replay: %s", play.note.c_str());
            }
        }
        if (play.at % kKeyEvery == 0 && !play.keys.count(play.at)) {
            play.keys[play.at] = Difference();
        }
    }
    void Keep(std::vector<uint8_t>& state, bool lasting) override {
        if (lasting) {
            state = Difference();
            return;
        }
        // (the rewind window: a save of rollback.c's, by its frame)
        if (!forkade_rb_save(play.at)) {
            state = Difference();
            return;
        }
        state.assign(9, 0);
        state[0] = 'T';
        std::memcpy(&state[1], &play.generation, 4);
        std::memcpy(&state[5], &play.at, 4);
    }
    void PutBack(const std::vector<uint8_t>& state, uint32_t frame) override {
        if (!state.empty() && state[0] == 'T') {
            uint32_t generation, at;
            std::memcpy(&generation, &state[1], 4);
            std::memcpy(&at, &state[5], 4);
            if (generation == play.generation && forkade_rb_load(at)) {
                play.at = at;
                play.lastSt = ReadFight().st;
                return;
            }
            // (a save since dropped: there again from a keyframe, unseen)
            GoBack(frame);
            while (play.at < frame) {
                RunFrame(false, false);
            }
            return;
        }
        if (RestoreDifference(state)) {
            play.at = frame;
            play.lastSt = ReadFight().st;
        }
    }
    void GoBack(uint32_t frame) override {
        auto it = play.keys.upper_bound(frame);
        if (it != play.keys.begin()) {
            --it;
            RestoreDifference(it->second);
            play.at = it->first;
        } else {
            forkade_state_load(play.replay.state.data(), play.replay.state.size());
            forkade_rb_reset();
            play.generation++;
            play.at = 0;
        }
        play.lastSt = ReadFight().st;
    }
};
Board board;

// ---- the strip and the HUD over the picture ----

std::string Cut(const std::string& name) {
    size_t letters = 0, at = 0;
    for (; at < name.size(); at++) {
        if ((static_cast<unsigned char>(name[at]) & 0xC0) != 0x80 && letters++ == 12) {
            break;
        }
    }
    return name.substr(0, at);
}

} // namespace

extern "C" {

void forkade_replay_setup(const char* where) {
    folder = where != nullptr ? where : "";
    if (!folder.empty()) {
        std::error_code e;
        std::filesystem::create_directories(folder, e);
    }
}

void forkade_replay_names(const char* one, const char* two) {
    names[0] = one != nullptr ? one : "";
    names[1] = two != nullptr ? two : "";
}

void forkade_replay_after_frame(const HOST_INPUT* used) {
    const Fight f = ReadFight();
    const bool any = getenv("MVC2_REPLAY_ANY") != nullptr; // (the checks: a fight against the computer kept too)
    if (f.known && !rec.on && (f.versus || any) && f.st == kVsAppear && lastSt != kVsAppear) {
        Begin(false, "");
        lastSt = f.st;
        return; // (the state is after this frame: its controls are the next frame's)
    }
    if (f.known) {
        lastSt = f.st;
    }
    if (!rec.on) {
        return;
    }
    const uint16_t pads[2] = {static_cast<uint16_t>((used->player[0] & 0xFFF0) | (used->coin ? kCoinBit : 0)),
                              static_cast<uint16_t>(used->player[1] & 0xFFF0)};
    if (pads[0] != rec.last[0] || pads[1] != rec.last[1]) {
        rec.replay.inputs[rec.frames] = {pads[0], pads[1], 0, 0};
        rec.last[0] = pads[0];
        rec.last[1] = pads[1];
    }
    rec.frames++;
    if (rec.frames % kHashEvery == 0) {
        rec.replay.checks.emplace_back(rec.frames, Checksum());
    }
    if (rec.manual) {
        return;
    }
    if (f.known && f.ended && !rec.ended) {
        rec.ended = true;
        rec.endedAt = rec.frames;
        rec.winner = f.winner;
        if (f.winner == 0 || f.winner == 1) {
            score[f.winner]++;
        }
    }
    if ((rec.ended && rec.frames >= rec.endedAt + kTail) || (f.known && f.st < kVsAppear && rec.frames > 60) ||
        rec.frames >= kLongest) {
        Finish();
    }
}

int forkade_replay_record(const char* path) {
    if (play.on) {
        return 0;
    }
    if (rec.on) {
        rec.on = false; // (a match's automatic recording gives way)
    }
    Begin(true, path != nullptr ? path : "");
    return 1;
}

void forkade_replay_record_stop(void) {
    if (rec.on && rec.manual) {
        Finish();
    }
}

int forkade_replay_recording(void) {
    return rec.on && rec.manual;
}

int forkade_replay_play(const char* path, char* error, int room) {
    port::Replay r;
    std::string why;
    auto fail = [&](const std::string& text) {
        if (error != nullptr && room > 0) {
            std::snprintf(error, static_cast<size_t>(room), "%s", text.c_str());
        }
        lastNote = text;
        return 0;
    };
    if (!r.Load(path, why)) {
        return fail(why);
    }
    if (r.info.backend != "mvc2" || r.info.game != "mvsc2") {
        return fail("not a Marvel vs. Capcom 2 replay");
    }
    if (r.info.identity != MVC2_REPLAY_IDENTITY) {
        return fail("a replay of another build of the game (" + r.info.identity.substr(0, 12) + ")");
    }
    if (rec.on) {
        rec.on = false;
    }
    if (play.on) {
        forkade_replay_stop();
    }
    play = Playing{};
    play.own = SaveNow();
    play.replay = std::move(r);
    play.scoreBefore[0] = Feature(play.replay.info.features, "score0", 0);
    play.scoreBefore[1] = Feature(play.replay.info.features, "score1", 0);
    if (!forkade_rb_ready() && !forkade_rb_init(34)) {
        return fail("could not keep the frames to go back to");
    }
    if (!forkade_state_load(play.replay.state.data(), play.replay.state.size())) {
        forkade_state_load(play.own.data(), play.own.size());
        return fail("its state does not fit this build");
    }
    forkade_rb_reset();
    play.on = true;
    play.at = 0;
    play.lastSt = ReadFight().st;
    viewer.Begin(static_cast<uint32_t>(play.replay.info.frames));
    lastNote.clear();
    return 1;
}

int forkade_replay_playing(void) {
    return play.on;
}

void forkade_replay_stop(void) {
    if (!play.on) {
        return;
    }
    viewer.End();
    forkade_state_load(play.own.data(), play.own.size());
    forkade_rb_reset();
    play = Playing{};
}

int forkade_replay_tick(void) {
    if (!play.on) {
        return 0;
    }
    play.tick++;
    // (MVC2_VIEWERSCRIPT=TICK:COMMAND;...: the checks' commands at their ticks)
    if (const char* script = getenv("MVC2_VIEWERSCRIPT")) {
        static const struct {
            const char* name;
            Command command;
        } kNames[] = {{"pause", Command::Pause}, {"play", Command::Play}, {"faster", Command::Faster},
                      {"rewind", Command::Rewind}, {"slower", Command::Slower}, {"normal", Command::Normal},
                      {"back", Command::StepBack}, {"forward", Command::StepForward}, {"jumpback", Command::JumpBack},
                      {"jumpforward", Command::JumpForward}, {"start", Command::ToStart}, {"end", Command::ToEnd},
                      {"a", Command::MarkA}, {"b", Command::MarkB}, {"clear", Command::ClearLoop},
                      {"counter", Command::FrameCounter}, {"pin", Command::PinHud}, {"hud", Command::ToggleHud}};
        const std::string all = script;
        size_t from = 0;
        while (from < all.size()) {
            const size_t end = std::min(all.find(';', from), all.size());
            const std::string item = all.substr(from, end - from);
            from = end + 1;
            const size_t colon = item.find(':');
            if (colon == std::string::npos || strtoull(item.c_str(), nullptr, 10) != play.tick) {
                continue;
            }
            const std::string name = item.substr(colon + 1);
            if (name.rfind("seek", 0) == 0) {
                viewer.SeekTo(static_cast<uint32_t>(strtoul(name.c_str() + 4, nullptr, 10)), play.tick);
            } else {
                for (const auto& one : kNames) {
                    if (name == one.name) {
                        viewer.Do(one.command, play.tick);
                    }
                }
            }
        }
    }
    return viewer.Tick(board, play.tick) ? 1 : 0;
}

int forkade_replay_event(const SDL_Event* e, float px, float py) {
    if (!play.on) {
        return 0;
    }
    const uint64_t tick = play.tick;
    auto doIt = [&](Command c) {
        viewer.Do(c, tick);
        viewer.Touched(tick);
        return 1;
    };
    if (e->type == SDL_EVENT_KEY_DOWN) {
        const bool first = !e->key.repeat;
        switch (e->key.key) {
        case SDLK_ESCAPE: if (first) forkade_replay_stop(); return 1;
        case SDLK_SPACE:
        case SDLK_DOWN: return first ? doIt(Command::PlayPause) : 1;
        case SDLK_LEFT: return first ? doIt(Command::Rewind) : 1;
        case SDLK_RIGHT: return first ? doIt(Command::Faster) : 1;
        case SDLK_UP: return first ? doIt(Command::Slower) : 1;
        case SDLK_COMMA: return doIt(Command::StepBack);
        case SDLK_PERIOD: return doIt(Command::StepForward);
        case SDLK_LEFTBRACKET: return doIt(Command::JumpBack);
        case SDLK_RIGHTBRACKET: return doIt(Command::JumpForward);
        case SDLK_HOME: return doIt(Command::ToStart);
        case SDLK_END: return doIt(Command::ToEnd);
        case SDLK_PAGEUP: return doIt(Command::PreviousMarker);
        case SDLK_PAGEDOWN: return doIt(Command::NextMarker);
        case SDLK_A: return first ? doIt(Command::MarkA) : 1;
        case SDLK_B: return first ? doIt(Command::MarkB) : 1;
        case SDLK_C: return first ? doIt(Command::ClearLoop) : 1;
        case SDLK_F: return first ? doIt(Command::FrameCounter) : 1;
        case SDLK_V: if (first) viewer.Do(Command::ToggleHud, tick); return 1;
        default: viewer.Touched(tick); return 0;
        }
    }
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        switch (e->gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        case SDL_GAMEPAD_BUTTON_SOUTH: return doIt(Command::PlayPause);
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return doIt(Command::Rewind);
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return doIt(Command::Faster);
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return doIt(Command::Slower);
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return doIt(Command::JumpBack);
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return doIt(Command::JumpForward);
        case SDL_GAMEPAD_BUTTON_WEST: return doIt(Command::PreviousMarker);
        case SDL_GAMEPAD_BUTTON_NORTH: return doIt(Command::NextMarker);
        case SDL_GAMEPAD_BUTTON_BACK: viewer.Do(Command::ToggleHud, tick); return 1;
        case SDL_GAMEPAD_BUTTON_EAST: forkade_replay_stop(); return 1;
        default: viewer.Touched(tick); return 1;
        }
    }
    if (e->type == SDL_EVENT_MOUSE_MOTION) {
        viewer.Touched(tick);
        return 0;
    }
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button == SDL_BUTTON_LEFT && viewer.HudShown(tick)) {
        using Hit = recomp::replay::Viewer::Hit;
        const Hit hit = viewer.HitAt(px, py, 320, 240, static_cast<uint32_t>(play.replay.info.frames));
        switch (hit.kind) {
        case Hit::None: return 0;
        case Hit::Do: viewer.Do(hit.command, tick); break;
        case Hit::Seek: viewer.SeekTo(std::max<uint32_t>(1, hit.frame), tick); break;
        case Hit::Aid: break;
        case Hit::Strip: viewer.Touched(tick); break;
        }
        return 1;
    }
    return 0;
}

void forkade_replay_command(int command) {
    if (play.on) {
        viewer.Do(static_cast<Command>(command), play.tick);
        viewer.Touched(play.tick);
    }
}

int forkade_replay_paused(void) {
    return play.on && viewer.Paused();
}

int forkade_replay_hud_shown(void) {
    return play.on && viewer.HudShown(play.tick);
}

int forkade_replay_overlay(uint32_t* pixels, int w, int h) {
    if (!play.on) {
        return 0;
    }
    std::fill(pixels, pixels + static_cast<size_t>(w) * h, 0u);
    const recomp::text::Canvas canvas{pixels, w, h};
    using recomp::text::Palette;
    // "Name1  N  VS  M  Name2" at the top, as the CPS games' replays have it; the score as at this frame of the match
    int shown[2] = {play.scoreBefore[0], play.scoreBefore[1]};
    const Fight f = ReadFight();
    if (f.known && f.ended && (f.winner == 0 || f.winner == 1)) {
        shown[f.winner]++;
    }
    const auto& who = play.replay.info.players;
    struct Part {
        std::string text;
        recomp::text::Color colour;
    };
    const std::vector<Part> parts = {
        {who.size() > 0 && !who[0].empty() ? Cut(who[0]) : "Player 1", Palette::kPlayer1},
        {std::to_string(shown[0]), Palette::kNumber},
        {"VS", Palette::kVersus},
        {std::to_string(shown[1]), Palette::kNumber},
        {who.size() > 1 && !who[1].empty() ? Cut(who[1]) : "Player 2", Palette::kPlayer2}};
    const float dot = 1; // (the overlay is half the picture, 320 by 240: about the CPS games' 384 by 224)
    const float gap = recomp::text::TextWidth("  ", dot);
    float width = -gap;
    for (const Part& p : parts) {
        width += recomp::text::TextWidth(p.text.c_str(), dot) + gap;
    }
    float x = std::floor((w - width) / 2);
    canvas.Fill(x - 4, 0, width + 8, 10, Palette::kBand);
    for (const Part& p : parts) {
        canvas.Text(x, 1, dot, 0, p.colour, p.text.c_str());
        x += recomp::text::TextWidth(p.text.c_str(), dot) + gap;
    }
    if (!play.note.empty()) {
        const float nw = recomp::text::TextWidth(play.note.c_str(), dot);
        const float nx = std::floor((w - nw) / 2);
        canvas.Fill(nx - 4, 10, nw + 8, 10, Palette::kBand);
        canvas.Text(nx, 11, dot, 0, Palette::kPlayer2, play.note.c_str());
    }
    if (viewer.HudShown(play.tick)) {
        viewer.DrawHud(canvas, play.at, static_cast<uint32_t>(play.replay.info.frames), play.tick);
    }
    // (the band and the HUD's dimmed panels see-through: what Fill blended over black made half transparent again)
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; i++) {
        const uint32_t p = pixels[i];
        if (p != 0 && (p & 0x00FFFFFFu) != 0 && (p & 0xFF) < 40 && ((p >> 8) & 0xFF) < 40 && ((p >> 16) & 0xFF) < 48) {
            pixels[i] = (140u << 24) | (30u << 16) | (22u << 8) | 20u;
        }
    }
    return 1;
}

void forkade_replay_quit(void) {
    if (rec.on) {
        Finish(); // (a match left before its end is kept as far as it went)
    }
    for (std::thread& t : writers) {
        if (t.joinable()) {
            t.join();
        }
    }
    writers.clear();
}

const char* forkade_replay_note(void) {
    return play.on ? play.note.c_str() : lastNote.c_str();
}

} // extern "C"

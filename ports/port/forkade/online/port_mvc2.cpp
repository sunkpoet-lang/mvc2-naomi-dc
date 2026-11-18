// port_mvc2.cpp  (ForkAde) MvC2's port for ForkAde's online session (port.h, online_c.h).
//
// The session (online.cpp, GekkoNet) saves the board after every frame and goes back a few frames when the other
// side's controls were not what it guessed. MvC2's whole state is 52 MB, so a save is a token of rollback.c's (the
// pages written since the last, about 1.4 ms) and only the state a spectator joining a match in progress gets is
// whole (GekkoNet's portable save, patch_gekkonet_mvc2.cmake). Both sides boot alike (host_online: the backup and
// EEPROM the game's defaults, a fixed clock; free play; every character open) and start the session at the same
// frame, so the sides' boards are the same from it on.

#include "port.h"
#include "online.h"
#include "online_c.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "../recomp-common/chat.h"
#include "../recomp-common/text.h"

extern "C" {
#include "naomi_host.h"
#include "../replays.h"
#include "../rollback.h"
#include "../state.h"
#include "../state_hash.h"
int forkade_state_spans(void** at, uint32_t* size, int room);
void gekko_forkade_portable(int (*expand)(const unsigned char*, unsigned int, const unsigned char**, unsigned int*),
                            unsigned int most);
}

namespace cps2recomp {

Game& TheGame() {
    static Game game{{{"unlock", "All characters open", "Game"}}};
    return game;
}

int FeatureSetting(const char* name) {
    return std::strcmp(name, "unlock") == 0 ? host_unlock : 0;
}

void SetFeatureForNow(const char* name, int value) {
    if (std::strcmp(name, "unlock") == 0) {
        host_unlock = value;
    }
}

namespace battle {
int RoundResultNow() {
    const HOST_PROGRAM* p = naomi_program();
    if (p == nullptr || p->vs_sys == nullptr || p->sys == nullptr || *p->sys == nullptr) {
        return 0;
    }
    const auto* v = reinterpret_cast<const uint8_t*>(p->vs_sys);
    const int st = v[0x00], winner = static_cast<int8_t>(v[0x3e]);
    if (v[0x46] == 0 || st < 5) { // (vs_end_flag: the fight ended the match; vs_st 5: on after the fight)
        return 0;
    }
    return winner == 0 ? 1 : winner == 1 ? 2 : 3;
}
} // namespace battle

} // namespace cps2recomp

namespace port {

namespace {
constexpr int kRing = 48; // rollback.c's saves: GekkoNet's 8 frames back, its extra saves a frame, and room
constexpr unsigned kWholeMost = 96u << 20;
uint32_t generation = 1, serial = 0;
std::vector<uint8_t> portable; // (the last portable save, whole)
uint32_t portableSerial = 0;
// (the board as the session began, the same on every side: they boot alike to that frame) A spectator's state is sent
// as the pages that differ from it, about 10 MB packed in a fight instead of 15
std::vector<uint8_t> boot, sent;
constexpr uint32_t kPage = 4096;
constexpr uint32_t kDiffMark = 0x3144564D; // "MVD1"

void Put32(uint8_t* at, uint32_t v) { std::memcpy(at, &v, 4); }
uint32_t Get32(const uint8_t* at) {
    uint32_t v;
    std::memcpy(&v, at, 4);
    return v;
}

// MVD1, the whole state's size, then each page that differs from the boot state: its number and its bytes
void Difference(const std::vector<uint8_t>& now, std::vector<uint8_t>& out) {
    out.assign(8, 0);
    Put32(&out[0], kDiffMark);
    Put32(&out[4], static_cast<uint32_t>(now.size()));
    for (uint32_t off = 0; off < now.size(); off += kPage) {
        const uint32_t n = std::min<uint32_t>(kPage, static_cast<uint32_t>(now.size()) - off);
        if (off + n <= boot.size() && std::memcmp(now.data() + off, boot.data() + off, n) == 0) {
            continue;
        }
        const size_t at = out.size();
        out.resize(at + 4 + n);
        Put32(&out[at], off / kPage);
        std::memcpy(&out[at + 4], now.data() + off, n);
    }
}

bool Undifference(std::span<const uint8_t> d, std::vector<uint8_t>& whole) {
    if (d.size() < 8 || Get32(d.data()) != kDiffMark || boot.empty()) {
        return false;
    }
    const uint32_t size = Get32(d.data() + 4);
    whole.assign(size, 0);
    std::memcpy(whole.data(), boot.data(), std::min<size_t>(size, boot.size()));
    for (size_t at = 8; at + 4 <= d.size();) {
        const uint32_t off = Get32(d.data() + at) * kPage;
        if (off >= size) {
            return false;
        }
        const uint32_t n = std::min<uint32_t>(kPage, size - off);
        if (at + 4 + n > d.size()) {
            return false;
        }
        std::memcpy(whole.data() + off, d.data() + at + 4, n);
        at += 4 + n;
    }
    return true;
}

int Expand(const unsigned char* token, unsigned int len, const unsigned char** out, unsigned int* outLen) {
    if (len == 9 && token[0] == 'P' && Get32(token + 5) == portableSerial && !portable.empty()) {
        if (!boot.empty()) {
            Difference(portable, sent);
        } else {
            sent = portable;
        }
        *out = sent.data();
        *outLen = static_cast<unsigned int>(sent.size());
        return 1;
    }
    return 0;
}
} // namespace

void MvC2Whole(std::vector<uint8_t>& state) {
    state.resize(forkade_state_size());
    state.resize(forkade_state_save(state.data(), state.size()));
}

bool MvC2Save(std::vector<uint8_t>& state, bool whole) {
    if (!forkade_rb_ready() && !forkade_rb_init(kRing)) {
        return false;
    }
    serial++;
    state.assign(9, 0);
    if (whole) {
        MvC2Whole(portable);
        portableSerial = serial;
        state[0] = 'P';
    } else {
        if (!forkade_rb_save(serial)) {
            return false;
        }
        state[0] = 'K';
    }
    Put32(&state[1], generation);
    Put32(&state[5], serial);
    return true;
}

bool MvC2Load(std::span<const uint8_t> state) {
    if (state.size() == 9 && state[0] == 'K') {
        return Get32(&state[1]) == generation && forkade_rb_load(Get32(&state[5]));
    }
    if (state.size() == 9 && state[0] == 'P') {
        if (Get32(&state[5]) != portableSerial || portable.empty()) {
            return false;
        }
        state = portable;
    }
    // (a spectator's: the pages that differ from the boot state)
    std::vector<uint8_t> whole;
    if (state.size() >= 8 && Get32(state.data()) == kDiffMark) {
        if (!Undifference(state, whole)) {
            return false;
        }
        state = whole;
    }
    // (a whole state: the arcade wait's beginning)
    if (!forkade_state_load(state.data(), state.size())) {
        return false;
    }
    if (forkade_rb_ready()) {
        forkade_rb_reset();
    }
    generation++;
    return true;
}

Port& ThePort() {
    static Port port;
    return port;
}

void Port::Menu::Close() {
}

bool Port::RunOnlineFrame(std::span<const uint16_t> players, bool again) {
    HOST_INPUT in{};
    const uint16_t one = players.size() > 0 ? players[0] : 0, two = players.size() > 1 ? players[1] : 0;
    in.player[0] = one & ~cps2recomp::kCoin;
    in.player[1] = two & ~cps2recomp::kCoin;
    in.coin = ((one | two) & cps2recomp::kCoin) != 0;
    mvc2_front_run_frame(&in, again ? 0 : 1, again ? 0 : 1);
    if (!again) {
        frame++;
    }
    {
        // (MVC2_ONLINE_HASH=FILE, the checks': each frame run, by the game's own count, and the board's fingerprint; a
        // guess later taken back is run again, so the last line of a frame is the one that counts)
        static std::FILE* log = nullptr;
        static bool looked = false;
        if (!looked) {
            looked = true;
            if (const char* path = std::getenv("MVC2_ONLINE_HASH")) {
                log = std::fopen(path, "w");
            }
        }
        if (log != nullptr) {
            std::fprintf(log, "%u %08x\n", host_frame(), StateHash());
            std::fflush(log);
        }
    }
    return true;
}

void Port::ArcadeFrame() {
    HOST_INPUT in{};
    in.player[0] = pad[0] & ~cps2recomp::kCoin;
    in.coin = (pad[0] & cps2recomp::kCoin) != 0;
    mvc2_front_run_frame(&in, 1, 1);
}

// The board's fingerprint the sides compare after every save: the game program's .data and .bss whole and the main
// RAM a word in every 256 bytes (the whole RAM's hash is 32 MB a frame: the replays' checksums take it)
uint32_t Port::StateHash() const {
    void* at[8];
    uint32_t size[8];
    const int n = forkade_state_spans(at, size, 8);
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n && i < 2; i++) {
        h = (h ^ forkade_hash(at[i], size[i])) * 1099511628211ull;
    }
    const uint64_t* ram = reinterpret_cast<const uint64_t*>(static_cast<uintptr_t>(NAOMI_RAM_BASE));
    for (size_t w = 0; w < NAOMI_RAM_SIZE / 8; w += 32) {
        h = (h ^ ram[w]) * 1099511628211ull;
    }
    return static_cast<uint32_t>(h ^ (h >> 32));
}

std::string Port::BuildIdentity() const {
    return MVC2_REPLAY_IDENTITY;
}

void Port::ChatTick() {
    if (gekkoChat == nullptr) {
        return;
    }
    for (ChatLink::Heard& line : gekkoChat->ChatHeardLines()) {
        const std::string who = !line.name.empty() ? line.name : gekkoChat->ChatName(1 - gekkoChat->ChatLocal());
        chat.push_back({(who.empty() ? std::string("Other") : who) + ": " + line.text, false, tick + 8 * 60});
        std::fprintf(stderr, "chat: heard at frame %llu: %s: %s\n", static_cast<unsigned long long>(frame),
                     who.c_str(), line.text.c_str());
    }
    while (chat.size() > 6) {
        chat.erase(chat.begin());
    }
}

} // namespace port

// ---- the front end's ----

namespace {
port::OnlineOptions options;
bool wanted = false;
std::unique_ptr<port::Online> online;
std::string typed;
bool typing = false;
uint64_t sayAt = 0;   // (--net-say FRAME:TEXT, the checks': a line said at that frame of the match)
std::string sayText;
}

extern "C" {

int forkade_online_wanted(int argc, char** argv) {
    using Mode = port::OnlineOptions::Mode;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--gekko-host") {
            options.mode = Mode::Host;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                options.port = static_cast<uint16_t>(atoi(next()));
            }
        } else if (a == "--gekko-join" || a == "--gekko-watch") {
            const std::string v = next();
            const size_t colon = v.rfind(':');
            options.mode = a == "--gekko-join" ? Mode::Join : Mode::Watch;
            options.host = v.substr(0, colon);
            if (colon != std::string::npos) {
                options.port = static_cast<uint16_t>(atoi(v.c_str() + colon + 1));
            }
        } else if (a == "--gekko-spectate") {
            options.spectate = true;
        } else if (a == "--gekko-spectators") {
            options.spectators = atoi(next());
        } else if (a == "--gekko-spectator-delay") {
            options.spectatorDelay = atoi(next());
        } else if (a == "--gekko-delay") {
            options.delay = atoi(next());
        } else if (a == "--gekko-stress") {
            options.mode = Mode::Stress;
        } else if (a == "--gekko-checkpoint-log") {
            options.checkpointLog = next();
        } else if (a == "--nick") {
            options.name = next();
        } else if (a == "--net-say") {
            const std::string v = next();
            const size_t colon = v.find(':');
            sayAt = colon != std::string::npos ? strtoull(v.c_str(), nullptr, 10) : 120;
            sayText = colon != std::string::npos ? v.substr(colon + 1) : v;
        }
    }
    if (options.spectate && options.mode == Mode::Join) {
        options.mode = Mode::Watch;
    }
    wanted = options.mode != Mode::Off;
    if (wanted) {
        // (both sides alike from power on)
        host_online = 1;
        host_freeplay = 1;
        host_unlock = 1;
    }
    return wanted ? 1 : 0;
}

int forkade_online_start(char* error, int room) {
    if (!wanted) {
        return 0;
    }
    gekko_forkade_portable(port::Expand, port::kWholeMost);
    port::MvC2Whole(port::boot); // (the board as the session begins: the same on every side)
    online = std::make_unique<port::Online>();
    std::string why;
    port::Port& p = port::ThePort();
    if (!online->Start(p, options, why)) {
        if (error != nullptr && room > 0) {
            std::snprintf(error, static_cast<size_t>(room), "%s", why.c_str());
        }
        online.reset();
        return 0;
    }
    return 1;
}

int forkade_online_active(void) {
    return online != nullptr;
}

int forkade_online_tick(const HOST_INPUT* local) {
    if (!online) {
        return 0;
    }
    uint16_t lanes[2] = {static_cast<uint16_t>((local->player[0] & 0xFFF0) | (local->coin ? cps2recomp::kCoin : 0)),
                         static_cast<uint16_t>(local->player[1] & 0xFFF0)};
    port::Port& p = port::ThePort();
    p.pad[0] = lanes[0];
    if (!sayText.empty() && p.frame >= sayAt && p.gekkoChat != nullptr && p.gekkoChat->ChatReady()) {
        std::string line;
        if (p.gekkoChat->ChatSay(sayText, line) == port::ChatLink::Said::Sent) {
            std::fprintf(stderr, "chat: said at frame %llu: %s\n", static_cast<unsigned long long>(p.frame), line.c_str());
            p.chat.push_back({"You: " + line, true, p.tick + 8 * 60});
            sayText.clear();
        }
    }
    return online->Tick(std::span<const uint16_t>(lanes, 2));
}

int forkade_online_ended(void) {
    return online && online->GetStatus() == port::Online::Status::Ended;
}

int forkade_online_failed(void) {
    return online && online->Failed();
}

const char* forkade_online_message(void) {
    static std::string m;
    m = online ? online->Message() : "";
    return m.c_str();
}

void forkade_online_stop(void) {
    if (online) {
        online->Stop();
        online.reset();
    }
}

int forkade_online_key(int key, const char* text) {
    port::Port& p = port::ThePort();
    if (!online || p.gekkoChat == nullptr || !p.gekkoChat->ChatReady()) {
        return 0;
    }
    if (text != nullptr && typing) {
        typed += text;
        if (typed.size() > 120) {
            typed.resize(120);
        }
        return 1;
    }
    if (!typing) {
        if (key == 't' || key == 'T') {
            typing = true;
            typed.clear();
            return 1;
        }
        return 0;
    }
    if (key == '\r' || key == '\n') {
        std::string line;
        if (p.gekkoChat->ChatSay(typed, line) == port::ChatLink::Said::Sent) {
            p.chat.push_back({"You: " + line, true, p.tick + 8 * 60});
        }
        typing = false;
        typed.clear();
    } else if (key == 27) {
        typing = false;
        typed.clear();
    } else if (key == '\b' && !typed.empty()) {
        typed.pop_back();
    }
    return 1;
}

int forkade_online_overlay(uint32_t* pixels, int w, int h) {
    if (!online) {
        return 0;
    }
    std::fill(pixels, pixels + static_cast<size_t>(w) * h, 0u);
    const recomp::text::Canvas canvas{pixels, w, h};
    using recomp::text::Palette;
    port::Port& p = port::ThePort();
    const port::MatchView match = online->ReadMatch();
    auto cut = [](const std::string& name) {
        size_t letters = 0, at = 0;
        for (; at < name.size(); at++) {
            if ((static_cast<unsigned char>(name[at]) & 0xC0) != 0x80 && letters++ == 12) {
                break;
            }
        }
        return name.substr(0, at);
    };
    auto band = [&](float y, const std::vector<std::pair<std::string, recomp::text::Color>>& parts) {
        const float gap = recomp::text::TextWidth("  ", 1);
        float width = -gap;
        for (const auto& part : parts) {
            width += recomp::text::TextWidth(part.first.c_str(), 1) + gap;
        }
        float x = std::floor((w - width) / 2);
        canvas.Fill(x - 4, y, width + 8, 10, Palette::kBand);
        for (const auto& part : parts) {
            canvas.Text(x, y + 1, 1, 0, part.second, part.first.c_str());
            x += recomp::text::TextWidth(part.first.c_str(), 1) + gap;
        }
    };
    // the players' names and win streaks, "Name1  N  VS  M  Name2", as the CPS games have it online
    if (match.phase == port::MatchView::Phase::Playing) {
        std::vector<std::pair<std::string, recomp::text::Color>> parts;
        const std::string one = !match.names[0].empty() ? cut(match.names[0]) : "Player 1";
        const std::string two = !match.names[1].empty() ? cut(match.names[1]) : "Player 2";
        parts.push_back({one, Palette::kPlayer1});
        parts.push_back({std::to_string(p.streak[0]), Palette::kNumber});
        parts.push_back({"VS", Palette::kVersus});
        parts.push_back({std::to_string(p.streak[1]), Palette::kNumber});
        parts.push_back({two, Palette::kPlayer2});
        band(0, parts);
        if (match.spectating || match.watching > 0) {
            band(10, {{match.spectating ? "WATCHING" : "WATCHING: " + std::to_string(match.watching), Palette::kReadout}});
        }
    }
    // what the session says (waiting, connecting, the other side gone)
    std::string message = online->Message();
    if (!message.empty()) {
        std::transform(message.begin(), message.end(), message.begin(), [](unsigned char c) { return static_cast<char>(toupper(c)); });
        band(std::floor(h / 2.0f - 5), {{message, Palette::kReadout}});
    }
    // the chat at the left, above the gauges
    float y = h - 44.0f;
    auto line = [&](const std::string& text, recomp::text::Color colour) {
        const float width = recomp::text::TextWidth(text.c_str(), 1);
        canvas.Fill(4, y - 1, width + 6, 10, {20, 22, 30, 200});
        canvas.Text(7, y, 1, 0, colour, text.c_str());
        y -= 10;
    };
    if (typing) {
        line("Enter: send   Esc: cancel", {170, 170, 170, 255});
        line("Say: " + typed + ((p.tick / 20) % 2 ? "_" : " "), {255, 210, 80, 255});
    }
    for (auto it = p.chat.rbegin(); it != p.chat.rend(); ++it) {
        if (it->until > p.tick || typing) {
            line(it->text, it->own ? recomp::text::Color{170, 200, 255, 255} : recomp::text::Color{255, 255, 255, 255});
        }
    }
    // (the band and the panels see-through again, as replays.cpp does)
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; i++) {
        const uint32_t px = pixels[i];
        if (px != 0 && (px & 0x00FFFFFFu) != 0 && (px & 0xFF) < 40 && ((px >> 8) & 0xFF) < 40 && ((px >> 16) & 0xFF) < 48) {
            pixels[i] = (140u << 24) | (30u << 16) | (22u << 8) | 20u;
        }
    }
    return 1;
}

} // extern "C"

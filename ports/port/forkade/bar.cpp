// bar.cpp  (ForkAde) The menu bar of MvC2's window (bar.h), over recomp-common's MenuBar (recomp-common/menubar.h,
// GPL-3.0, a copy of the commit in recomp-common/COMMIT; GPL-3.0 and the port's AGPL-3.0 combine, each section 13).
// The menus are made here from what the front end says is on; the bar draws itself in a picture as big as the
// window, kept transparent but for the bar and the menus open, which the renderer puts over the game's picture.

#include "bar.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "recomp-common/menubar.h"
#include "recomp-common/volume.h"

namespace {

recomp::MenuBar bar;
FORKADE_BAR_STATE shown_state;
bool configured = false;
int width = 0, height = 0, dot = 1;
bool fullscreen = false;
bool dirty = true;              // (the picture is to be made again)
bool alt_alone = false;         // (Alt went down and nothing else since: its release opens the bar)
std::vector<uint32_t> canvas, picture;
SDL_Texture* texture = nullptr;
SDL_Renderer* texture_renderer = nullptr;
int texture_w = 0, texture_h = 0;

constexpr int kPictureHeight = 480; // the game's lines

const char* const kKeys[][2] = {
    {"Menu bar", "F10, Alt"}, {"Controls", "F1"}, {"Test menu (arcade)", "F2"}, {"Frame times", "F3"},
    {"Quick save", "F5"}, {"Slot before, next", "F6, F7"}, {"Quick load", "F9"}, {"Fullscreen", "F11, Alt+Enter"},
    {"Screenshot", "F12"}, {"Service", "9"}, {"Quit", "Esc"},
};

recomp::BarEntry Item(const std::string& label, int id, const char* shortcut = "", bool enabled = true,
                      recomp::BarEntry::Mark mark = recomp::BarEntry::NoMark) {
    recomp::BarEntry e;
    e.label = label;
    e.id = id;
    e.shortcut = shortcut;
    e.enabled = enabled;
    e.mark = mark;
    return e;
}

void Build(const FORKADE_BAR_STATE& s) {
    using Mark = recomp::BarEntry::Mark;
    const bool off = s.online != 0; // (online: what would change the match is dim)
    std::vector<recomp::BarMenu> menus = {{"Game", {}}, {"Video", {}}, {"Audio", {}}, {"Input", {}}, {"Features", {}}, {"Help", {}}};

    std::vector<recomp::BarEntry>& game = menus[0].entries;
    recomp::BarEntry save = Item("Save state", 0, "", !off), load = Item("Load state", 0, "", !off), slot = Item("Slot", 0);
    for (int i = 1; i <= 9; i++) {
        const std::string n = "Slot " + std::to_string(i);
        save.children.push_back(Item(n + (s.slot_used[i] ? " (in use)" : ""), FB_SAVE + i));
        load.children.push_back(Item(n, FB_LOAD + i, "", s.slot_used[i] != 0));
        slot.children.push_back(Item(n, FB_SLOT + i, "", true, s.slot == i ? Mark::Dot : Mark::NoMark));
    }
    game.push_back(std::move(save));
    game.push_back(std::move(load));
    game.push_back(std::move(slot));
    game.push_back(recomp::BarEntry::Line());
    game.push_back(Item("Quick save", FB_QUICK_SAVE, "F5", !off));
    game.push_back(Item("Quick load", FB_QUICK_LOAD, "F9", !off && s.slot >= 1 && s.slot <= 9 && s.slot_used[s.slot]));
    game.push_back(Item("Screenshot", FB_SCREENSHOT, "F12"));
    {   // Game > Replay: record by hand, play one, stop; while one plays, the viewer's transport as its keys have it
        recomp::BarEntry replay = Item("Replay", 0, "", !off);
        if (s.replay_playing) {
            replay.children.push_back(Item("Playing a replay", 0, "", false));
        } else if (s.replay_recording) {
            replay.children.push_back(Item("Recording input", 0, "", false));
        }
        replay.children.push_back(Item("Record input...", FB_REPLAY_RECORD, "", !s.replay_playing && !s.replay_recording && s.arcade));
        replay.children.push_back(Item("Play input...", FB_REPLAY_PLAY, "", s.arcade != 0));
        replay.children.push_back(Item("Stop replay/record", FB_REPLAY_STOP, "Esc", s.replay_playing || s.replay_recording));
        replay.children.push_back(Item("Open the replays folder", FB_REPLAY_FOLDER));
        if (s.replay_playing) {
            // (recomp::replay::Command's values)
            enum { PlayPause, Play, Pause, Faster, Rewind, Slower, Normal, StepBack, StepForward, JumpBack, JumpForward,
                   ToStart, ToEnd, PreviousMarker, NextMarker, MarkA, MarkB, ClearLoop, PinHud, FrameCounter, ToggleHud };
            const struct {
                const char* label;
                int command;
                const char* keys;
            } viewer[] = {{s.replay_paused ? "Play" : "Pause", PlayPause, "Down, Space"},
                          {"Rewind (faster each time)", Rewind, "Left"},
                          {"Fast-forward (faster each time)", Faster, "Right"},
                          {"Slow motion (x0.75, x0.5, x0.25)", Slower, "Up"},
                          {"Normal speed", Normal, ""},
                          {"Frame back", StepBack, ","}, {"Frame forward", StepForward, "."},
                          {"5 seconds back", JumpBack, "["}, {"5 seconds forward", JumpForward, "]"},
                          {"To the start", ToStart, "Home"}, {"To the end", ToEnd, "End"},
                          {"Previous marker", PreviousMarker, "PgUp"}, {"Next marker", NextMarker, "PgDn"},
                          {"Loop: mark A, then B", MarkA, "A, B"}, {"Clear the loop", ClearLoop, "C"},
                          {"Frame counter", FrameCounter, "F"},
                          {s.replay_hud ? "Hide the HUD" : "Show the HUD", ToggleHud, "V"}};
            replay.children.push_back(recomp::BarEntry::Line());
            for (const auto& one : viewer) {
                replay.children.push_back(Item(one.label, FB_VIEWER + one.command, one.keys));
            }
        }
        game.push_back(std::move(replay));
    }
    game.push_back(recomp::BarEntry::Line());
    game.push_back(Item("Reset game", FB_RESET, "", !off));
    if (s.arcade) {
        game.push_back(Item("Test menu", FB_TEST_MENU, "F2", !off));
    }
    if (s.can_switch) {
        game.push_back(Item(s.arcade ? "Switch to the Dreamcast version" : "Switch to the arcade version", FB_SWITCH, "", !off));
    }
    game.push_back(recomp::BarEntry::Line());
    game.push_back(Item("Quit", FB_QUIT, "Esc"));

    std::vector<recomp::BarEntry>& video = menus[1].entries;
    video.push_back(Item("Fullscreen", FB_FULLSCREEN, "F11", true, s.fullscreen ? Mark::Check : Mark::NoMark));
    recomp::BarEntry scaling = Item("Scaling", 0);
    const char* const scalings[] = {"Keep the shape", "Whole multiples", "Fill the window"};
    for (int i = 0; i < 3; i++) {
        scaling.children.push_back(Item(scalings[i], FB_SCALING + i, "", true, s.scaling == i ? Mark::Dot : Mark::NoMark));
    }
    video.push_back(std::move(scaling));
    recomp::BarEntry renderer = Item("Renderer", 0);
    renderer.children.push_back(Item("GPU", FB_RENDERER_GPU, "", s.gpu_works != 0, s.gpu ? Mark::Dot : Mark::NoMark));
    renderer.children.push_back(Item("Software", FB_RENDERER_SOFT, "", true, s.gpu ? Mark::NoMark : Mark::Dot));
    video.push_back(std::move(renderer));
    video.push_back(Item("Frame times", FB_TIMES, "F3", true, s.times ? Mark::Check : Mark::NoMark));

    std::vector<recomp::BarEntry>& audio = menus[2].entries;
    audio.push_back(Item("Sound", FB_MUTE, "", true, s.muted ? Mark::NoMark : Mark::Check));
    audio.push_back(Item("Louder (" + std::to_string(s.volume) + "%)", FB_LOUDER, "", s.volume < recomp::kVolumeFull));
    audio.push_back(Item("Quieter", FB_QUIETER, "", s.volume > 0));

    menus[3].entries.push_back(Item("Controls...", FB_CONTROLS, "F1"));

    menus[4].entries.push_back(Item("All characters open", FB_UNLOCK, "", !off, s.unlock ? Mark::Check : Mark::NoMark));

    recomp::BarEntry keys = Item("Keys", 0);
    for (const auto& key : kKeys) {
        keys.children.push_back(Item(key[0], FB_KEY, key[1]));
    }
    menus[5].entries.push_back(std::move(keys));
    menus[5].entries.push_back(Item("About (the port's source)", FB_ABOUT));

    bar.Configure(std::move(menus));
}

void Picture() {
    canvas.assign(static_cast<size_t>(width) * height, 0xFF000000u);
    bar.Draw(canvas.data(), width, height);
    picture.assign(canvas.size(), 0);
    auto keep = [&](int x0, int y0, int w, int h) {
        for (int y = std::max(y0, 0); y < std::min(y0 + h, height); y++) {
            for (int x = std::max(x0, 0); x < std::min(x0 + w, width); x++) {
                picture[static_cast<size_t>(y) * width + x] = canvas[static_cast<size_t>(y) * width + x];
            }
        }
    };
    if (bar.BarShown()) {
        keep(0, 0, width, bar.Height());
    }
    for (const recomp::MenuBar::Box& box : bar.Boxes()) {
        keep(static_cast<int>(box.x), static_cast<int>(box.y), static_cast<int>(box.w + 1), static_cast<int>(box.h + 1));
    }
}

int Take(const recomp::MenuBar::Chosen& c) {
    dirty = true;
    return c.chosen ? c.id : FB_NONE;
}

} // namespace

extern "C" {

void forkade_bar_configure(const FORKADE_BAR_STATE* state) {
    if (configured && std::memcmp(state, &shown_state, sizeof shown_state) == 0) {
        return;
    }
    shown_state = *state;
    configured = true;
    Build(shown_state);
    dirty = true;
}

void forkade_bar_layout(int w, int h, int full) {
    if (w == width && h == height && (full != 0) == fullscreen) {
        return;
    }
    width = std::max(w, 1);
    height = std::max(h, 1);
    fullscreen = full != 0;
    // a dot as many pixels as the window has the game's lines and the bar's dots in it, whole
    dot = std::max(1, height / (kPictureHeight + recomp::MenuBar::kHeightDots));
    bar.SetArea(width, height, dot);
    bar.ShowBar(!fullscreen);
    dirty = true;
}

int forkade_bar_strip(void) {
    return fullscreen ? 0 : bar.Height();
}

int forkade_bar_open(void) {
    return bar.IsOpen();
}

int forkade_bar_event(const SDL_Event* e, int* chosen) {
    *chosen = FB_NONE;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: {
        const SDL_Keycode k = e->key.key;
        alt_alone = (k == SDLK_LALT || k == SDLK_RALT) && !e->key.repeat;
        if (k == SDLK_F10 && !e->key.repeat) {
            if (bar.IsOpen()) {
                bar.Close();
            } else {
                bar.OpenMenu(0);
            }
            dirty = true;
            return 1;
        }
        if (!bar.IsOpen()) {
            return 0;
        }
        unsigned b = k == SDLK_UP ? recomp::kMenuUp : k == SDLK_DOWN ? recomp::kMenuDown : k == SDLK_LEFT ? recomp::kMenuLeft
                     : k == SDLK_RIGHT ? recomp::kMenuRight
                     : (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) ? recomp::kMenuConfirm
                     : (k == SDLK_ESCAPE || k == SDLK_BACKSPACE) ? recomp::kMenuBack : 0;
        if (b != 0) {
            *chosen = Take(bar.Buttons(b));
        }
        return 1; // (while a menu is open, the keys are its)
    }
    case SDL_EVENT_KEY_UP:
        if ((e->key.key == SDLK_LALT || e->key.key == SDLK_RALT) && alt_alone) {
            alt_alone = false;
            if (bar.IsOpen()) {
                bar.Close();
            } else {
                bar.OpenMenu(0);
            }
            dirty = true;
            return 1;
        }
        return bar.IsOpen();
    case SDL_EVENT_MOUSE_MOTION:
        if (bar.Pointer(e->motion.x, e->motion.y)) {
            dirty = true;
        }
        return bar.IsOpen();
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        alt_alone = false;
        if (e->button.button == SDL_BUTTON_RIGHT) {
            bar.OpenPopup(e->button.x, e->button.y);
            dirty = true;
            return 1;
        }
        if (e->button.button == SDL_BUTTON_LEFT && (bar.IsOpen() || bar.Hit(e->button.x, e->button.y))) {
            *chosen = Take(bar.Click(e->button.x, e->button.y));
            return 1;
        }
        return 0;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return bar.IsOpen() || bar.Hit(e->button.x, e->button.y);
    default:
        if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            alt_alone = false;
        }
        return 0;
    }
}

void forkade_bar_render(SDL_Renderer* renderer) {
    if (fullscreen && !bar.IsOpen()) {
        return;
    }
    if (texture != nullptr && (texture_renderer != renderer || texture_w != width || texture_h != height)) {
        SDL_DestroyTexture(texture);
        texture = nullptr;
    }
    if (texture == nullptr) {
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        if (texture == nullptr) {
            return;
        }
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
        texture_renderer = renderer;
        texture_w = width;
        texture_h = height;
        dirty = true;
    }
    if (dirty) {
        Picture();
        SDL_UpdateTexture(texture, nullptr, picture.data(), width * 4);
        dirty = false;
    }
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
}

void forkade_bar_forget(void) {
    texture = nullptr;
    texture_renderer = nullptr;
}

void forkade_bar_test_open(int menu, int downs, int right) {
    bar.OpenMenu(static_cast<size_t>(menu));
    for (int i = 0; i < downs; i++) {
        bar.Buttons(recomp::kMenuDown);
    }
    if (right) {
        bar.Buttons(recomp::kMenuRight);
    }
    dirty = true;
}

int forkade_volume_step(int percent, int steps) {
    return recomp::StepVolume(percent, steps);
}

float forkade_volume_gain(int percent) {
    return recomp::VolumeGain(percent);
}

} // extern "C"

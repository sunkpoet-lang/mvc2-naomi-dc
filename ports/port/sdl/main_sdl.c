/*
 * main_sdl.c  The PC front end on SDL3 (with the GPU renderer) or SDL2: the window, the frame loop, input
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sdl_front.h"
#ifdef MVC2_SDL3
#include <SDL3/SDL_main.h>
#endif

#include "naomi_host.h"
#include "naomi_input.h"
#include "rd.h"
#ifdef MVC2_SDL3
#include "rd_gpu.h"
#endif
#include "input_sdl.h"
#include "menu_sdl.h"
#include "test_mode.h"
#include "../forkade/state_hash.h" /* (ForkAde: MVC2_HASHLOG) */
#include "../forkade/state.h"     /* (ForkAde: save states) */
#include "../forkade/state_heap.h"
#include "../forkade/rollback.h"
#include "../forkade/bar.h"        /* (ForkAde: the menu bar) */
#include "../forkade/replays.h"    /* (ForkAde: replays) */
#include "../forkade/online/online_c.h" /* (ForkAde: online play) */

void sdl_audio_open(void);         /* sdl_audio.c */
void sdl_audio_frame(void);
void sdl_audio_close(void);
void snd_close(void);
void sdl_audio_gain(float gain);   /* (ForkAde) */
void sdl_audio_frame_silent(void);

static SDL_Window* window;
static SDL_Renderer* renderer;
static SDL_Texture* screen;

#ifdef MVC2_SDL3
static SDL_GPUDevice* gpu;
static SDL_Texture* gpu_screen;
#else
#define gpu 0
#endif
static HOST_INPUT input;

const HOST_INPUT* host_input(void) {
    return &input;
}

static unsigned presented;
static int switch_req;      /* F9 */
static int coin_req;
static int test_run;        /* MVC2_FRAMES: the press list is the only input */

/* MVC2_PRESS=n:key,...: player 1 holds the key for 5 frames from frame n; coin and switch happen at n */
static uint32_t pressed(void) {
    static const struct {
        const char* name;
        uint32_t bit;
    } keys[] = {
        { "start", JVS_START }, { "up", JVS_UP }, { "down", JVS_DOWN }, { "left", JVS_LEFT }, { "right", JVS_RIGHT },
        { "b1", JVS_B1 }, { "b2", JVS_B2 }, { "b3", JVS_B3 }, { "b4", JVS_B4 }, { "b5", JVS_B5 }, { "b6", JVS_B6 },
    };
    const char* p = getenv("MVC2_PRESS");
    uint32_t bits = 0;
    while (p != NULL && *p != 0) {
        char name[16];
        unsigned at;
        size_t i;
        if (sscanf(p, "%u:%15[a-z0-9]", &at, name) == 2 && presented == at && strcmp(name, "switch") == 0) {
            switch_req = 1;
        }
        if (sscanf(p, "%u:%15[a-z0-9]", &at, name) == 2 && presented == at && strcmp(name, "coin") == 0) {
            coin_req = 1;
        }
        if (sscanf(p, "%u:%15[a-z0-9]", &at, name) == 2 && presented >= at && presented < at + 5) {
            for (i = 0; i < sizeof keys / sizeof keys[0]; i++) {
                if (strcmp(name, keys[i].name) == 0) {
                    bits |= keys[i].bit;
                }
            }
        }
        p = strchr(p, ',');
        p = p != NULL ? p + 1 : NULL;
    }
    return bits;
}

static void read_input(void) {
    input.player[0] = pressed();
    input.player[1] = 0;
    input.coin = 0;
    input.test = 0;
    if (!test_run) {
        input_read(&input);
        if (key_held(SDL_SCANCODE_9)) {
            input.player[0] |= JVS_SERVICE;
        }
    }
    if (coin_req) {
        input.coin = 1;
        coin_req = 0;
    }
}

static uint32_t frame[RD_WIDTH * RD_HEIGHT];

static void put32(unsigned char* p, uint32_t v) {
    p[0] = v & 0xFF, p[1] = v >> 8 & 0xFF, p[2] = v >> 16 & 0xFF, p[3] = v >> 24;
}

/* SDL2's BMP header (BITMAPV4HEADER), so shots match whichever SDL is used */
static void save_bmp(const char* name, const uint32_t* argb, int w, int h) {
    unsigned char hd[122];
    uint32_t* row = malloc((size_t)w * 4);
    FILE* f = row != NULL ? fopen(name, "wb") : NULL;
    int y, x;
    if (f == NULL) {
        free(row);
        return;
    }
    memset(hd, 0, sizeof hd);
    hd[0] = 'B', hd[1] = 'M';
    put32(hd + 2, sizeof hd + (uint32_t)(w * h * 4));
    put32(hd + 10, sizeof hd);
    put32(hd + 14, 108);
    put32(hd + 18, (uint32_t)w);
    put32(hd + 22, (uint32_t)h);
    hd[26] = 1, hd[28] = 32, hd[30] = 3;
    put32(hd + 34, (uint32_t)(w * h * 4));
    put32(hd + 54, 0x00FF0000), put32(hd + 58, 0x0000FF00), put32(hd + 62, 0x000000FF), put32(hd + 66, 0xFF000000);
    put32(hd + 70, 0x57696E20);             /* "Win " */
    fwrite(hd, sizeof hd, 1, f);
    for (y = h - 1; y >= 0; y--) {
        for (x = 0; x < w; x++) {
            put32((unsigned char*)&row[x], argb[(size_t)y * w + x]);
        }
        fwrite(row, (size_t)w * 4, 1, f);
    }
    fclose(f);
    free(row);
}

/* the GPU's frame read back, at most once a frame */
#ifdef MVC2_SDL3
static int frame_read;
#endif

static void read_frame(void) {
#ifdef MVC2_SDL3
    if (gpu && !frame_read) {
        frame_read = rd_gpu_read(frame);
    }
#endif
}

/* MVC2_SHOT=n,m,...: frames saved as shot_n.bmp */
static void screenshot(void) {
    const char* list = getenv("MVC2_SHOT");
    char want[16], name[32];
    if (list == NULL) {
        return;
    }
    snprintf(want, sizeof want, "%u", presented);
    for (;;) {
        size_t n = strcspn(list, ",");
        if (n == strlen(want) && strncmp(list, want, n) == 0) {
            break;
        }
        if (list[n] == 0) {
            return;
        }
        list += n + 1;
    }
    snprintf(name, sizeof name, "shot_%u.bmp", presented);
#ifdef MVC2_SDL3
    /* MVC2_SHOTVIEW=1: the picture as the window shows it */
    if (gpu && getenv("MVC2_SHOTVIEW") != NULL) {
        int w, h;
        uint32_t* big;
        rd_gpu_frame_size(&w, &h);
        big = malloc((size_t)w * h * 4);
        if (big != NULL && rd_gpu_read_view(big)) {
            save_bmp(name, big, w, h);
        }
        free(big);
        return;
    }
#endif
    read_frame();
    save_bmp(name, frame, RD_WIDTH, RD_HEIGHT);
}

/* both boards: a 27 MHz pixel clock, 858 by 525 */
static double frame_rate = 27000000.0 / (858.0 * 525.0);
static Uint64 next_frame;

static void pace_init(void) {
    const char* fps = getenv("MVC2_FPS");
    if (fps != NULL) {
        frame_rate = atof(fps);
    } else if (getenv("MVC2_FRAMES") != NULL) {
        frame_rate = 0;
    }
}

/* sleep most of the way, then spin the last 2 ms */
static void pace(void) {
    Uint64 now, freq = SDL_GetPerformanceFrequency(), step;
    if (frame_rate <= 0) {
        return;
    }
    step = (Uint64)(freq / frame_rate);
    now = SDL_GetPerformanceCounter();
    if (next_frame == 0 || now > next_frame + step * 4) {
        next_frame = now;   /* after a stall, don't catch up */
    }
    while (now < next_frame) {
        Uint64 left_ms = (next_frame - now) * 1000 / freq;
        if (left_ms > 2) {
            SDL_Delay((Uint32)(left_ms - 2));
        }
        now = SDL_GetPerformanceCounter();
    }
    next_frame += step;
}

#ifdef MVC2_SDL3
static SDL_GPUTexture* gpu_wrapped;     /* gpu_screen's */
static int gpu_wrapped_w, gpu_wrapped_h;

/* the GPU's picture settings from mvc2.cfg: resolution (auto or 1 to 12), aa (1 to 4), sprites */
static int view_res, view_aa = 1, view_sprites = RD_SPRITES_SHARP;
static const char* const sprite_names[] = { "arcade", "sharp", "pixel", "smooth" };

void view_settings(void) {
    const char* r = config_get("resolution");
    const char* a = config_get("aa");
    const char* sp = config_get("sprites");
    int i;
    view_res = r != NULL && atoi(r) > 0 ? atoi(r) : 0;
    view_aa = a != NULL && atoi(a) > 0 ? atoi(a) : 1;
    view_aa = view_aa > 4 ? 4 : view_aa;
    view_sprites = RD_SPRITES_SHARP;
    for (i = 0; i < 4; i++) {
        if (sp != NULL && strcmp(sp, sprite_names[i]) == 0) {
            view_sprites = i;
        }
    }
}

/* the game's picture's size in the window, in pixels */
static int scaling;

/* (ForkAde) where the game's picture goes: below the menu bar's strip, kept to its shape (scaling 0), at a whole
   multiple (1, when it fits) or filling it (2); SDL's logical presentation is off, so the bar has the window's pixels */
static void picture_rect(SDL_FRect* r) {
    int w = 0, h = 0, top;
    float s;
    SDL_GetCurrentRenderOutputSize(renderer, &w, &h);
    top = forkade_bar_strip();
    top = top > h / 2 ? 0 : top;
    h -= top;
    r->x = 0, r->y = (float)top, r->w = (float)w, r->h = (float)h;
    if (scaling == 2 || w <= 0 || h <= 0) {
        return;
    }
    s = (float)w / RD_WIDTH < (float)h / RD_HEIGHT ? (float)w / RD_WIDTH : (float)h / RD_HEIGHT;
    if (scaling == 1 && s >= 1.0f) {
        s = (float)(int)s;
    }
    r->w = RD_WIDTH * s, r->h = RD_HEIGHT * s;
    r->x = (float)(int)((w - r->w) / 2), r->y = (float)(top + (int)((h - r->h) / 2));
}

static void view_size(int* w, int* h) {
    SDL_FRect r;
    picture_rect(&r);
    *w = (int)(r.w + 0.5f), *h = (int)(r.h + 0.5f);
    if (*w < 1 || *h < 1) {
        *w = *h = 0;
    }
}

static SDL_Texture* gpu_frame(void) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUTexture* t;
    int w, h;
    if (cmd == NULL) {
        return gpu_screen;
    }
    view_size(&w, &h);
    rd_gpu_view(view_res, view_aa, w, h, view_sprites);
    t = rd_gpu_end_frame(cmd);
    SDL_SubmitGPUCommandBuffer(cmd);
    rd_gpu_frame_size(&w, &h);
    if (gpu_screen != NULL && (t != gpu_wrapped || w != gpu_wrapped_w || h != gpu_wrapped_h)) {
        SDL_DestroyTexture(gpu_screen);
        gpu_screen = NULL;
    }
    if (gpu_screen == NULL && t != NULL) {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_ABGR8888);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
        SDL_SetPointerProperty(props, SDL_PROP_TEXTURE_CREATE_GPU_TEXTURE_POINTER, t);
        gpu_screen = SDL_CreateTextureWithProperties(renderer, props);
        SDL_DestroyProperties(props);
        if (gpu_screen != NULL) {
            SDL_SetTextureScaleMode(gpu_screen, SDL_SCALEMODE_NEAREST);
        }
        gpu_wrapped = t, gpu_wrapped_w = w, gpu_wrapped_h = h;
    }
    return gpu_screen;
}
#endif

/* F3: the game's and the drawing's time a frame, and the frame rate, averaged over half a second */
enum { TIMES_W = 240, TIMES_H = 12 };
static int times_on;
static SDL_Texture* times_tex;
static Uint64 game_ticks, draw_ticks, present_ticks, times_start;
static unsigned times_frames;

static void times_update(void) {
    static uint32_t text[TIMES_W * TIMES_H];
    char line[64];
    double freq = (double)SDL_GetPerformanceFrequency();
    Uint64 now = SDL_GetPerformanceCounter();
    int i;
    if (times_start == 0) {
        game_ticks = draw_ticks = 0, times_frames = 0, times_start = now;
        return;
    }
    if (now - times_start < freq / 2) {
        return;
    }
    snprintf(line, sizeof line, "GAME %5.1f MS  DRAW %5.1f MS  %5.1f FPS", game_ticks * 1000.0 / freq / times_frames,
             draw_ticks * 1000.0 / freq / times_frames, times_frames * freq / (now - times_start));
    game_ticks = draw_ticks = 0, times_frames = 0, times_start = now;
    if (times_tex == NULL) {
        times_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, TIMES_W, TIMES_H);
        if (times_tex == NULL) {
            return;
        }
        SDL_SetTextureBlendMode(times_tex, SDL_BLENDMODE_BLEND);
#ifdef MVC2_SDL3
        SDL_SetTextureScaleMode(times_tex, SDL_SCALEMODE_NEAREST);
#endif
    }
    for (i = 0; i < TIMES_W * TIMES_H; i++) {
        text[i] = 0xA0000000u;
    }
    menu_text(text, TIMES_W, TIMES_H, 3, 2, line, 0xFFFFFFFFu, 1);
    SDL_UpdateTexture(times_tex, NULL, text, TIMES_W * 4);
}

static void bar_shot(void);
static void window_shot(const char* path);

#ifdef MVC2_SDL3
/* (ForkAde) a replay's strip and HUD, drawn at the picture's 640 by 480 over it */
static SDL_Texture* overlay_tex;
static SDL_Renderer* overlay_renderer;

#define OVERLAY_W (RD_WIDTH / 2)    /* (half the picture: the HUD and the strip as big as the CPS games have them) */
#define OVERLAY_H (RD_HEIGHT / 2)

static void replay_overlay(const SDL_FRect* r) {
    static uint32_t pixels[OVERLAY_W * OVERLAY_H];
    if (!forkade_replay_overlay(pixels, OVERLAY_W, OVERLAY_H) && !forkade_online_overlay(pixels, OVERLAY_W, OVERLAY_H)) {
        return;
    }
    if (overlay_tex == NULL || overlay_renderer != renderer) {
        overlay_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, OVERLAY_W, OVERLAY_H);
        overlay_renderer = renderer;
        if (overlay_tex == NULL) {
            return;
        }
        SDL_SetTextureBlendMode(overlay_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(overlay_tex, SDL_SCALEMODE_NEAREST);
    }
    SDL_UpdateTexture(overlay_tex, NULL, pixels, OVERLAY_W * 4);
    SDL_RenderTexture(renderer, overlay_tex, NULL, r);
}
#endif

static void show(SDL_Texture* t) {
    SDL_RenderClear(renderer);
#ifdef MVC2_SDL3
    {   /* (ForkAde) the picture below the menu bar, the bar over it */
        SDL_FRect r;
        int w = 0, h = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &w, &h);
        forkade_bar_layout(w, h, (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0);
        picture_rect(&r);
        SDL_RenderTexture(renderer, t, NULL, &r);
        if (times_on && times_tex != NULL) {
            float k = r.w / RD_WIDTH;
            SDL_FRect d = { r.x + 4 * k, r.y + 4 * k, TIMES_W * k, TIMES_H * k };
            SDL_RenderTexture(renderer, times_tex, NULL, &d);
        }
        replay_overlay(&r);
        forkade_bar_render(renderer);
        bar_shot();
        {   /* MVC2_WINSHOT=N:FILE,...: the window as drawn at its Nth showing, saved (the checks' pictures) */
            static unsigned shows;
            const char* list = getenv("MVC2_WINSHOT");
            shows++;
            while (list != NULL && *list) {
                unsigned n;
                char path[256];
                if (sscanf(list, "%u:%255[^,]", &n, path) == 2 && n == shows) {
                    window_shot(path);
                }
                list = strchr(list, ',');
                list = list != NULL ? list + 1 : NULL;
            }
        }
    }
#else
    SDL_RenderTexture(renderer, t, NULL, NULL);
    if (times_on && times_tex != NULL) {
        render_at(renderer, times_tex, 4, 4, TIMES_W, TIMES_H);
    }
#endif
    pace();
    SDL_RenderPresent(renderer);
}

/* (ForkAde) how host_present treats this frame: 0 shown, 1 drawn for the next show() (a replay's), 2 not drawn */
static int present_mode;
static SDL_Texture* drawn;  /* (mode 1: the picture drawn) */

void host_present(void) {
    SDL_Texture* t = screen;
    Uint64 start = SDL_GetPerformanceCounter();
    if (present_mode == 2) {
        rd_begin_frame();   /* (and the press list's clock stays: a frame run again has the same controls) */
        return;
    }
#ifdef MVC2_SDL3
    if (gpu) {
        t = gpu_frame();
        frame_read = 0;
    } else
#endif
    {
        rd_end_frame(frame, RD_WIDTH);
    }
    rd_begin_frame();
    presented++;
    screenshot();
    if (!gpu) {
        SDL_UpdateTexture(screen, NULL, frame, RD_WIDTH * 4);
    }
    draw_ticks += SDL_GetPerformanceCounter() - start;
    times_frames++;
    if (times_on) {
        times_update();
    }
    if (present_mode == 1) {
        drawn = t;
        return;
    }
    show(t);
    present_ticks += SDL_GetPerformanceCounter() - start;
}

#ifdef MVC2_SDL3
#define FULLSCREEN SDL_WINDOW_FULLSCREEN    /* at the desktop's size */
#else
#define FULLSCREEN SDL_WINDOW_FULLSCREEN_DESKTOP
#endif

static const char* const scalings[] = { "aspect", "integer", "stretch" };
static int keep_settings;

static void set_scaling(void) {
    int w, h, small;
#ifdef MVC2_SDL3
    static const SDL_RendererLogicalPresentation mode[] = { SDL_LOGICAL_PRESENTATION_LETTERBOX,
                                                           SDL_LOGICAL_PRESENTATION_INTEGER_SCALE,
                                                           SDL_LOGICAL_PRESENTATION_STRETCH };
    /* (ForkAde) placed by picture_rect, below the menu bar (mode and small: gibletto's, unused) */
    (void)mode, (void)small, (void)w, (void)h;
    SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
#else
    SDL_GetRendererOutputSize(renderer, &w, &h);
    small = w < RD_WIDTH || h < RD_HEIGHT;
    SDL_RenderSetLogicalSize(renderer, scaling == 2 ? 0 : RD_WIDTH, scaling == 2 ? 0 : RD_HEIGHT);
    SDL_RenderSetIntegerScale(renderer, scaling == 1 && !small);
#endif
}

/* the largest whole multiple of 640 by 480 in nine tenths of the desktop */
static void first_size(int* w, int* h) {
    SDL_Rect r;
    int k = 2;
#ifdef MVC2_SDL3
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &r)) {
#else
    if (SDL_GetDisplayUsableBounds(0, &r) == 0) {
#endif
        k = r.w * 9 / 10 / RD_WIDTH < r.h * 9 / 10 / RD_HEIGHT ? r.w * 9 / 10 / RD_WIDTH : r.h * 9 / 10 / RD_HEIGHT;
        k = k < 1 ? 1 : k;
    }
    *w = RD_WIDTH * k, *h = RD_HEIGHT * k;
}

#ifdef MVC2_SDL3
/* when a step fails, nothing is kept and the software renderer draws */
static void open_gpu(void) {
    /* Vulkan first: Direct3D 12 makes each new texture and buffer on its own, and a hyper combo's can stall a
       frame by 20 ms or more, enough to run the sound dry */
    static const char* const drivers[] = { "vulkan", NULL };
    int i;
    for (i = 0; i < 2; i++) {
        gpu = SDL_CreateGPUDevice(RD_GPU_SHADER_FORMATS | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV |
                                      SDL_GPU_SHADERFORMAT_MSL, false, drivers[i]);
        if (gpu != NULL) {
            if (rd_gpu_init(gpu)) {
                renderer = SDL_CreateGPURenderer(gpu, window);
                if (renderer != NULL) {
                    host_log("renderer: gpu (%s)", SDL_GetGPUDeviceDriver(gpu));
                    return;
                }
            }
            rd_gpu_quit();
            SDL_DestroyGPUDevice(gpu);
            gpu = NULL;
        }
    }
    host_log("renderer: no gpu (%s), soft", SDL_GetError());
}

/* rd_gpu.c's state starts again with the other program */
static void gpu_restart(int stop) {
    if (!gpu) {
        return;
    }
    if (stop) {
        SDL_DestroyTexture(gpu_screen);
        gpu_screen = NULL;
        rd_gpu_quit();
    } else if (!rd_gpu_init(gpu)) {
        host_log("rd_gpu: could not start again");
    }
}
#else
#define gpu_restart(stop)
#endif

static int make_renderer(int want_gpu) {
#ifdef MVC2_SDL3
    if (want_gpu) {
        open_gpu();
    }
    if (renderer == NULL) {
        renderer = SDL_CreateRenderer(window, NULL);
    }
    if (renderer == NULL) {
        return 0;
    }
    SDL_SetRenderVSync(renderer, 1);
#else
    (void)want_gpu;
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == NULL) {
        return 0;
    }
#endif
    screen = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, RD_WIDTH, RD_HEIGHT);
#ifdef MVC2_SDL3
    SDL_SetTextureScaleMode(screen, SDL_SCALEMODE_NEAREST);     /* SDL3's default is linear */
#endif
    set_scaling();
    return screen != NULL;
}

int renderer_gpu(void) {
    return gpu != 0;
}

int renderer_set_gpu(int on) {
#ifdef MVC2_SDL3
    SDL_Texture** own[] = { &times_tex, &screen, &gpu_screen };
    size_t i;
    if ((gpu != NULL) == on) {
        return on;
    }
    read_frame();
    for (i = 0; i < sizeof own / sizeof own[0]; i++) {
        if (*own[i] != NULL) {
            SDL_DestroyTexture(*own[i]);
            *own[i] = NULL;
        }
    }
    SDL_DestroyRenderer(renderer);
    renderer = NULL;
    if (gpu != NULL) {
        rd_gpu_quit();
        SDL_DestroyGPUDevice(gpu);
        gpu = NULL;
    }
    forkade_bar_forget(); /* (ForkAde: its texture went with the renderer) */
    overlay_tex = NULL;
    if (!make_renderer(on)) {
        host_log("no renderer: %s", SDL_GetError());
    }
    frame_read = 1;     /* frame holds the last picture */
    return gpu != NULL;
#else
    (void)on;
    return 0;
#endif
}

/* sizes in pixels, so a whole multiple stays sharp at any display scaling */
static int open_window(void) {
    const char* size = config_get("window");
    const char* full = config_get("fullscreen");
    const char* scale = config_get("scale");
    const char* r;
    int w, h, i;
    keep_settings = getenv("MVC2_FRAMES") == NULL;
    if (!keep_settings) {
        full = NULL;
    }
    if (size == NULL || sscanf(size, "%dx%d", &w, &h) != 2) {
        first_size(&w, &h);
    }
    w = w < RD_WIDTH / 2 ? RD_WIDTH / 2 : w;
    h = h < RD_HEIGHT / 2 ? RD_HEIGHT / 2 : h;
#ifdef MVC2_SDL3
    window = SDL_CreateWindow(game_title(), w, h,
                              SDL_WINDOW_RESIZABLE | (full != NULL && strcmp(full, "yes") == 0 ? SDL_WINDOW_FULLSCREEN : 0));
    if (window == NULL) {
        return 0;
    }
    SDL_SetWindowMinimumSize(window, RD_WIDTH / 2, RD_HEIGHT / 2);
#else
    window = SDL_CreateWindow(game_title(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                              SDL_WINDOW_RESIZABLE |
                                  (full != NULL && strcmp(full, "yes") == 0 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (window == NULL) {
        return 0;
    }
    SDL_SetWindowMinimumSize(window, RD_WIDTH / 2, RD_HEIGHT / 2);
#endif
    for (i = 0; i < 3; i++) {
        if (scale != NULL && strcmp(scale, scalings[i]) == 0) {
            scaling = i;
        }
    }
    /* the GPU unless mvc2.cfg says soft; a test run draws by rd_soft.c's rules unless it says gpu */
    r = config_get("renderer");
    return make_renderer(r != NULL ? strcmp(r, "soft") != 0 : !test_run);
}

static void toggle_fullscreen(void) {
    int full = !(SDL_GetWindowFlags(window) & FULLSCREEN);
#ifdef MVC2_SDL3
    SDL_SetWindowFullscreen(window, full);
#else
    SDL_SetWindowFullscreen(window, full ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
#endif
    if (keep_settings) {
        config_set("fullscreen", full ? "yes" : "no");
    }
}

/* 1 when the window took the event */
static int window_key(const SDL_Event* e) {
    if (window_resized(e)) {
        set_scaling();
        return 0;
    }
    if (e->type != SDL_EVENT_KEY_DOWN || e->key.repeat) {
        return 0;
    }
    if (key_sym(e) == SDLK_F11 || (key_sym(e) == SDLK_RETURN && key_alt(e))) {
        toggle_fullscreen();
        return 1;
    }
    if (key_sym(e) == SDLK_F3) {
        times_on = !times_on;
        times_start = 0;
        return 1;
    }
    /* (ForkAde) F10 opens the menu bar; the scaling is in its Video menu */
    return 0;
}

static void save_window(void) {
    char size[32];
    const char* was = config_get("window");
    int w, h;
    if (!keep_settings || (SDL_GetWindowFlags(window) & (FULLSCREEN | SDL_WINDOW_MAXIMIZED))) {
        return;
    }
    SDL_GetWindowSize(window, &w, &h);
    snprintf(size, sizeof size, "%dx%d", w, h);
    if (was == NULL || strcmp(was, size) != 0) {
        config_set("window", size);
    }
}

/* the arcade's test menu (F2, the arcade only): its 40 by 30 characters, 16 pixels each, on black */
static int test_open, restart_req;

static int test_key(const SDL_Event* e) {
    static int axis[2];
    if (e->type == SDL_EVENT_KEY_DOWN) {
        SDL_Scancode k = key_scancode(e);
        int first = !e->key.repeat;
        return k == SDL_SCANCODE_UP ? TM_UP : k == SDL_SCANCODE_DOWN ? TM_DOWN : k == SDL_SCANCODE_LEFT ? TM_LEFT
               : k == SDL_SCANCODE_RIGHT ? TM_RIGHT
               : (k == SDL_SCANCODE_RETURN || k == SDL_SCANCODE_KP_ENTER) && first ? TM_OK
               : (k == SDL_SCANCODE_ESCAPE || k == SDL_SCANCODE_F2) && first ? TM_BACK : 0;
    }
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        int b = e->gbutton.button;
        return b == SDL_GAMEPAD_BUTTON_DPAD_UP ? TM_UP : b == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? TM_DOWN
               : b == SDL_GAMEPAD_BUTTON_DPAD_LEFT ? TM_LEFT : b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? TM_RIGHT
               : b == SDL_GAMEPAD_BUTTON_SOUTH ? TM_OK : b == SDL_GAMEPAD_BUTTON_EAST ? TM_BACK : 0;
    }
    if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
        (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
        int y = e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY, way = e->gaxis.value > 24000 ? 1 : e->gaxis.value < -24000 ? -1 : 0;
        int moved = way != 0 && way != axis[y];
        axis[y] = way;
        return !moved ? 0 : y ? (way < 0 ? TM_UP : TM_DOWN) : (way < 0 ? TM_LEFT : TM_RIGHT);
    }
    return 0;
}

static void present_test(void) {
    static uint32_t shown[RD_WIDTH * RD_HEIGHT];
    const TEST_SCREEN* s = test_mode_screen();
    int i, x, y;
    for (i = 0; i < RD_WIDTH * RD_HEIGHT; i++) {
        shown[i] = 0xFF000000u;
    }
    for (y = 0; y < TM_ROWS; y++) {
        for (x = 0; x < TM_COLS; x++) {
            char c[2] = { s->ch[y][x], 0 };
            if (c[0] != ' ') {
                menu_text(shown, RD_WIDTH, RD_HEIGHT, x * 16 + 3, y * 16 + 1, c, 0xFF000000u | s->color[y][x], 2);
            }
        }
    }
    SDL_UpdateTexture(screen, NULL, shown, RD_WIDTH * 4);
    show(screen);
}

static void present_menu(void) {
    static uint32_t shown[RD_WIDTH * RD_HEIGHT];
    read_frame();
    memcpy(shown, frame, sizeof shown);
    menu_draw(shown, RD_WIDTH, RD_HEIGHT);
    SDL_UpdateTexture(screen, NULL, shown, RD_WIDTH * 4);
    show(screen);
}

/* broken at spaces into lines of cols characters; the y below it */
static int draw_wrapped(int x, int y, int cols, const char* text, uint32_t color) {
    char line[128];
    while (*text != 0) {
        int n = (int)strcspn(text, "\n"), cut = n;
        if (n > cols) {
            for (cut = cols; cut > 0 && text[cut] != ' '; cut--) {
            }
            cut = cut > 0 ? cut : cols;
        }
        snprintf(line, sizeof line, "%.*s", cut, text);
        menu_text(frame, RD_WIDTH, RD_HEIGHT, x, y, line, color, 1);
        y += 12;
        text += cut;
        text += *text == ' ' || *text == '\n';
    }
    return y;
}

/* what data is missing, until Enter finds it (1) or Escape quits (0) */
static int missing_screen(int argc, char** argv) {
    if (pick_message()[0] == 0 || getenv("MVC2_FRAMES") != NULL) {
        return 0;
    }
    if (!open_window()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Marvel vs. Capcom 2", pick_message(), NULL);
        return 0;
    }
    SDL_SetWindowTitle(window, "Marvel vs. Capcom 2");
    for (;;) {
        SDL_Event e;
        int i, y;
        for (i = 0; i < RD_WIDTH * RD_HEIGHT; i++) {
            frame[i] = 0xFF101830u;
        }
        y = draw_wrapped(16, 16, 100, "MARVEL VS. CAPCOM 2", 0xFFFFE040u);
        y = draw_wrapped(16, y + 12, 100, pick_message(), 0xFFFFFFFFu);
        draw_wrapped(16, y + 12, 100, "ENTER: LOOK AGAIN    ESCAPE: QUIT", 0xFFA0A0A0u);
        SDL_UpdateTexture(screen, NULL, frame, RD_WIDTH * 4);
        show(screen);
        while (SDL_PollEvent(&e)) {
            if (window_key(&e)) {
                continue;
            }
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && key_sym(&e) == SDLK_ESCAPE)) {
                return 0;
            }
            if (e.type == SDL_EVENT_KEY_DOWN && (key_sym(&e) == SDLK_RETURN || key_sym(&e) == SDLK_KP_ENTER)) {
                if (pick_game(argc, argv)) {
                    SDL_SetWindowTitle(window, game_title());
                    return 1;
                }
                if (pick_message()[0] == 0) {
                    return 0;
                }
            }
        }
    }
}

void* host_try_malloc(size_t bytes) {
    return malloc(bytes);
}

void* host_try_calloc(size_t n, size_t size) {
    return calloc(n, size);
}

void host_out_of_memory(const char* what, size_t bytes) {
    char text[200];
    snprintf(text, sizeof text, "Out of memory: %s of %lu bytes failed.", what, (unsigned long)bytes);
    host_log("stopped: %s", text);
    if (getenv("MVC2_FRAMES") == NULL) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Marvel vs. Capcom 2", text, NULL);
    }
    exit(1);
}

#ifdef MVC2_SDL3
/* ---- (ForkAde) replays (port/forkade/replays.h) ---- */

static int dreamcast(void);

void mvc2_front_run_frame(const HOST_INPUT* in, int seen, int heard) {
    input = *in;
    present_mode = seen ? 1 : 2;
    naomi_run_frame();
    if (heard) {
        sdl_audio_frame();
    } else {
        sdl_audio_frame_silent();
    }
    present_mode = 0;
    forkade_hash_frame(host_frame());
}

/* SDL's file dialogs answer on their own thread: the answer waits here for the loop */
static SDL_Mutex* dialog_lock;
static int dialog_kind;         /* FB_REPLAY_RECORD or FB_REPLAY_PLAY, 0 none */
static const char* pending_replay;  /* (--replay PATH, until the game has started) */
static int online_wanted, online_end_ticks; /* (--gekko-host, --gekko-join...: online_c.h) */
static char dialog_path[1024];

static void SDLCALL dialog_done(void* kind, const char* const* files, int filter) {
    (void)filter;
    SDL_LockMutex(dialog_lock);
    if (files != NULL && files[0] != NULL) {
        snprintf(dialog_path, sizeof dialog_path, "%s", files[0]);
        dialog_kind = (int)(intptr_t)kind;
    }
    SDL_UnlockMutex(dialog_lock);
}

static const SDL_DialogFileFilter replay_filter[] = { { "ForkAde replays (.fareplay)", "fareplay" } };

static void replay_dialog(int kind) {
    const char* folder = host_save_path("replays");
    if (dialog_lock == NULL) {
        dialog_lock = SDL_CreateMutex();
    }
    if (kind == FB_REPLAY_RECORD) {
        SDL_ShowSaveFileDialog(dialog_done, (void*)(intptr_t)kind, window, replay_filter, 1, folder);
    } else {
        SDL_ShowOpenFileDialog(dialog_done, (void*)(intptr_t)kind, window, replay_filter, 1, folder, false);
    }
}

static void replay_play(const char* path) {
    char why[256];
    if (dreamcast()) {
        host_log("replay: the arcade version's only");
        return;
    }
    if (!forkade_replay_play(path, why, sizeof why)) {
        host_log("replay: %s: %s", path, why);
    }
}

static void replay_dialog_take(void) {
    int kind;
    char path[1024];
    if (dialog_lock == NULL) {
        return;
    }
    SDL_LockMutex(dialog_lock);
    kind = dialog_kind;
    snprintf(path, sizeof path, "%s", dialog_path);
    dialog_kind = 0;
    SDL_UnlockMutex(dialog_lock);
    if (kind == FB_REPLAY_RECORD) {
        size_t n = strlen(path);
        if (n < 9 || strcmp(path + n - 9, ".fareplay") != 0) {
            strncat(path, ".fareplay", sizeof path - n - 1);
        }
        forkade_replay_record(path);
    } else if (kind == FB_REPLAY_PLAY) {
        replay_play(path);
    }
}

/* the overlay's pixels (320 by 240) of a point of the window */
static void to_picture(float x, float y, float* px, float* py) {
    SDL_FRect r;
    picture_rect(&r);
    *px = r.w > 0 ? (x - r.x) * (RD_WIDTH / 2) / r.w : 0;
    *py = r.h > 0 ? (y - r.y) * (RD_HEIGHT / 2) / r.h : 0;
}
#endif

#ifdef MVC2_SDL3
/* ---- (ForkAde) the menu bar's actions (port/forkade/bar.h) ---- */

static int state_slot = 1, slot_used[10], slots_checked;
static int muted, volume = 100;

static int dreamcast(void) {
    return strstr(game_title(), "Dreamcast") != NULL;
}

static const char* state_file(int slot) {
    char name[64];
    snprintf(name, sizeof name, "mvc2_%s_slot%d.state", dreamcast() ? "dc" : "arc", slot);
    return host_save_path(name);
}

static void check_slots(void) {
    int i;
    for (i = 1; i <= 9; i++) {
        FILE* f = fopen(state_file(i), "rb");
        slot_used[i] = f != NULL;
        if (f != NULL) {
            fclose(f);
        }
    }
    slots_checked = 1;
}

static void apply_volume(void) {
    sdl_audio_gain(muted ? 0.0f : forkade_volume_gain(volume));
}

static void save_volume(void) {
    char v[16];
    if (keep_settings) {
        snprintf(v, sizeof v, "%d", volume);
        config_set("volume", v);
        config_set("mute", muted ? "yes" : "no");
    }
}

static void load_volume(void) {
    const char* v = config_get("volume");
    const char* m = config_get("mute");
    volume = v != NULL ? forkade_volume_step(atoi(v), 0) : 100;
    muted = m != NULL && strcmp(m, "yes") == 0;
    apply_volume();
}

static void bar_refresh(void) {
    FORKADE_BAR_STATE b;
    memset(&b, 0, sizeof b);
    if (!slots_checked) {
        check_slots();
    }
    b.fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    b.scaling = scaling;
    b.gpu = gpu != NULL;
    b.gpu_works = 1;
    b.times = times_on;
    b.unlock = host_unlock;
    b.slot = state_slot;
    memcpy(b.slot_used, slot_used, sizeof slot_used);
    b.arcade = !dreamcast();
#if defined(MVC2_ARCADE) && defined(MVC2_DC)
    b.can_switch = 1;
#endif
    b.online = forkade_online_active();
    b.muted = muted;
    b.volume = volume;
    b.replay_playing = forkade_replay_playing();
    b.replay_recording = forkade_replay_recording();
    b.replay_paused = forkade_replay_paused();
    b.replay_hud = forkade_replay_hud_shown();
    forkade_bar_configure(&b);
}

/* F12 and Game > Screenshot: the game's last picture, beside the executable */
static void take_screenshot(void) {
    char name[64];
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    strftime(name, sizeof name, "mvc2_%Y%m%d_%H%M%S.bmp", t);
    read_frame();
    save_bmp(host_save_path(name), frame, RD_WIDTH, RD_HEIGHT);
}

static void bar_do(int id, int* quit) {
    if (id >= FB_SAVE + 1 && id <= FB_SAVE + 9) {
        state_slot = id - FB_SAVE;
        id = FB_QUICK_SAVE;
    } else if (id >= FB_LOAD + 1 && id <= FB_LOAD + 9) {
        state_slot = id - FB_LOAD;
        id = FB_QUICK_LOAD;
    } else if (id >= FB_SLOT + 1 && id <= FB_SLOT + 9) {
        state_slot = id - FB_SLOT;
        return;
    } else if (id >= FB_VIEWER && id < FB_VIEWER + 40) {
        forkade_replay_command(id - FB_VIEWER);
        return;
    } else if (id >= FB_SCALING && id <= FB_SCALING + 2) {
        scaling = id - FB_SCALING;
        set_scaling();
        if (keep_settings) {
            config_set("scale", scalings[scaling]);
        }
        return;
    }
    switch (id) {
    case FB_QUICK_SAVE:
        if (forkade_replay_playing()) {
            break;
        }
        if (!forkade_state_save_file(state_file(state_slot))) {
            host_log("could not save the state in slot %d", state_slot);
        }
        check_slots();
        break;
    case FB_QUICK_LOAD:
        if (forkade_replay_playing()) {
            break;
        }
        if (slot_used[state_slot] && !forkade_state_load_file(state_file(state_slot))) {
            host_log("could not load the state in slot %d", state_slot);
        }
        break;
    case FB_SLOT_BEFORE:
        state_slot = state_slot == 1 ? 9 : state_slot - 1;
        break;
    case FB_SLOT_NEXT:
        state_slot = state_slot == 9 ? 1 : state_slot + 1;
        break;
    case FB_SCREENSHOT:
        take_screenshot();
        break;
    case FB_RESET:
        restart_req = 1;
        break;
    case FB_TEST_MENU:
        if (!dreamcast()) {
            test_mode_open("ENTER", "ESC");
            test_open = 1;
        }
        break;
    case FB_SWITCH:
        switch_req = 1;
        slots_checked = 0;
        break;
    case FB_QUIT:
        *quit = 1;
        break;
    case FB_FULLSCREEN:
        toggle_fullscreen();
        break;
    case FB_RENDERER_GPU:
    case FB_RENDERER_SOFT:
        renderer_set_gpu(id == FB_RENDERER_GPU);
        if (keep_settings) {
            config_set("renderer", gpu != NULL ? "gpu" : "soft");
        }
        break;
    case FB_TIMES:
        times_on = !times_on;
        times_start = 0;
        break;
    case FB_CONTROLS:
        menu_open_controls();
        break;
    case FB_UNLOCK:
        host_unlock = !host_unlock;
        if (keep_settings) {
            config_set("unlock", host_unlock ? "all" : "none");
        }
        break;
    case FB_MUTE:
        muted = !muted;
        apply_volume();
        save_volume();
        break;
    case FB_LOUDER:
    case FB_QUIETER:
        volume = forkade_volume_step(volume, id == FB_LOUDER ? 1 : -1);
        apply_volume();
        save_volume();
        break;
    case FB_ABOUT:
        SDL_OpenURL("https://github.com/sunkpoet-lang/mvc2-naomi-dc");
        break;
    case FB_REPLAY_RECORD:
    case FB_REPLAY_PLAY:
        replay_dialog(id);
        break;
    case FB_REPLAY_STOP:
        forkade_replay_record_stop();
        forkade_replay_stop();
        break;
    case FB_REPLAY_FOLDER: {
        char url[1200];
        snprintf(url, sizeof url, "file:///%s", host_save_path("replays"));
        SDL_OpenURL(url);
        break;
    }
    default:
        break;
    }
}

/* the bar's keys while it is closed: 1 when one was taken */
static int bar_key(const SDL_Event* e, int* quit) {
    if (e->type != SDL_EVENT_KEY_DOWN || e->key.repeat) {
        return 0;
    }
    switch (key_sym(e)) {
    case SDLK_F5: bar_do(FB_QUICK_SAVE, quit); return 1;
    case SDLK_F6: bar_do(FB_SLOT_BEFORE, quit); return 1;
    case SDLK_F7: bar_do(FB_SLOT_NEXT, quit); return 1;
    case SDLK_F9: bar_do(FB_QUICK_LOAD, quit); return 1;
    case SDLK_F12: bar_do(FB_SCREENSHOT, quit); return 1;
    default: return 0;
    }
}

/* the window's picture while the bar is open: the game stands still */
static void present_bar(void) {
    read_frame();
    SDL_UpdateTexture(screen, NULL, frame, RD_WIDTH * 4);
    show(screen);
}

/* MVC2_BARTEST=FRAME:MENU:DOWNS:RIGHT and MVC2_BARSHOT=FILE: at that frame a menu of the bar opened (as the keys
   would), and the window as drawn saved to FILE, then the run ends */
static int bar_shot_due, bar_test_done;

static void bar_test(unsigned now) {
    const char* t = getenv("MVC2_BARTEST");
    int f, m, d, r;
    if (bar_test_done || t == NULL || sscanf(t, "%d:%d:%d:%d", &f, &m, &d, &r) != 4 || (unsigned)f != now) {
        return;
    }
    bar_refresh();
    forkade_bar_test_open(m, d, r);
    bar_shot_due = 1;
}

static void bar_shot(void) {
    const char* path = getenv("MVC2_BARSHOT");
    if (!bar_shot_due || path == NULL || !forkade_bar_open()) {
        return;
    }
    bar_shot_due = 0;
    bar_test_done = 1;
    window_shot(path);
}

static void window_shot(const char* path) {
    SDL_Surface *got, *argb;
    got = SDL_RenderReadPixels(renderer, NULL);
    argb = got != NULL ? SDL_ConvertSurface(got, SDL_PIXELFORMAT_ARGB8888) : NULL;
    if (argb != NULL) {
        uint32_t* px = malloc((size_t)argb->w * argb->h * 4);
        int y;
        if (px != NULL) {
            for (y = 0; y < argb->h; y++) {
                memcpy(px + (size_t)y * argb->w, (uint8_t*)argb->pixels + (size_t)y * argb->pitch, (size_t)argb->w * 4);
            }
            save_bmp(path, px, argb->w, argb->h);
            free(px);
        }
    }
    SDL_DestroySurface(argb);
    SDL_DestroySurface(got);
}
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
/* (ForkAde) the command line as UTF-8 (the launcher's nicknames and paths may have accents; the C runtime's argv is
   in the ANSI code page) */
static void utf8_arguments(int* argc, char*** argv) {
    int n, i;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &n);
    char** out;
    if (wide == NULL) {
        return;
    }
    out = calloc((size_t)n + 1, sizeof *out);
    for (i = 0; out != NULL && i < n; i++) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
        out[i] = malloc((size_t)(bytes > 0 ? bytes : 1));
        if (out[i] == NULL) {
            return;
        }
        if (bytes <= 0 || WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, out[i], bytes, NULL, NULL) <= 0) {
            out[i][0] = 0;
        }
    }
    LocalFree(wide);
    if (out != NULL) {
        *argc = n;
        *argv = out;
    }
}
#endif

int main(int argc, char** argv) {
    int quit = 0;
    long frames_left = getenv("MVC2_FRAMES") ? atol(getenv("MVC2_FRAMES")) : -1;

    test_run = frames_left >= 0;
#ifdef _WIN32
    utf8_arguments(&argc, &argv);
#endif

    if (!naomi_map_ram()) {
        fprintf(stderr, "could not map main RAM at %08X\n", NAOMI_RAM_BASE);
        return 1;
    }
    if (!state_heap_init()) {   /* (ForkAde: the save states' region, before anything can take its address) */
        return 1;
    }
    host_divzero_install();
    pace_init();
#ifdef MVC2_SDL3
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | (test_run ? 0 : SDL_INIT_GAMEPAD))) {
#else
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | (test_run ? 0 : SDL_INIT_GAMECONTROLLER)) != 0) {
#endif
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!pick_game(argc, argv) && !missing_screen(argc, argv)) {
        return 1;
    }
    rd_quality(config_get("render"));   /* MVC2_RDQUALITY overrides it */
#ifdef MVC2_SDL3
    view_settings();
#endif
    input_init();
    if (window == NULL && !open_window()) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    if (frames_left < 0) {
        sdl_audio_open();
#ifdef MVC2_SDL3
        load_volume();  /* (ForkAde: the menu bar's Audio) */
#endif
    }
    host_keep_start();
    rd_begin_frame();
    if (!start_game()) {
        return 1;
    }
#ifdef MVC2_SDL3
    {   /* (ForkAde) replays: kept in replays\ beside the executable (MVC2_REPLAYS elsewhere), the players' names from
           the launcher (--name1, --name2), and --replay PATH played at once */
        const char* where = getenv("MVC2_REPLAYS");
        const char *one = NULL, *two = NULL, *replay = NULL;
        int i;
        forkade_replay_setup(where != NULL ? where : host_save_path("replays"));
        for (i = 1; i + 1 < argc; i++) {
            if (strcmp(argv[i], "--name1") == 0) {
                one = argv[++i];
            } else if (strcmp(argv[i], "--name2") == 0) {
                two = argv[++i];
            } else if (strcmp(argv[i], "--replay") == 0) {
                replay = argv[++i];
            }
        }
        forkade_replay_names(one, two);
        pending_replay = replay;    /* (played once the game has started its sound: its RAM is part of a state) */
        online_wanted = forkade_online_wanted(argc, argv); /* (the board made alike before its first frame) */
    }
#endif
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (test_run) {
                quit |= e.type == SDL_EVENT_QUIT;
                continue;
            }
#ifdef MVC2_SDL3
            if (!test_open && !menu_active()) {     /* (ForkAde) the menu bar first */
                SDL_Event c = e;
                int chosen;
                SDL_ConvertEventToRenderCoordinates(renderer, &c);
                if (forkade_bar_event(&c, &chosen)) {
                    if (chosen != FB_NONE) {
                        bar_do(chosen, &quit);
                    }
                    continue;
                }
                if (bar_key(&e, &quit)) {
                    continue;
                }
                if (forkade_online_active()) {      /* the chat's keys */
                    if (e.type == SDL_EVENT_TEXT_INPUT && forkade_online_key(0, e.text.text)) {
                        continue;
                    }
                    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                        SDL_Keycode k = key_sym(&e);
                        int key = k == SDLK_RETURN || k == SDLK_KP_ENTER ? '\r' : k == SDLK_ESCAPE ? 27
                                  : k == SDLK_BACKSPACE ? '\b' : k == SDLK_T ? 't' : 0;
                        if (key != 0 && forkade_online_key(key, NULL)) {
                            continue;
                        }
                    }
                }
                if (forkade_replay_playing()) {     /* the viewer's keys; the game's controls are the replay's */
                    float px = 0, py = 0;
                    if (c.type == SDL_EVENT_MOUSE_BUTTON_DOWN || c.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                        to_picture(c.button.x, c.button.y, &px, &py);
                    }
                    forkade_replay_event(&c, px, py);
                    if (e.type != SDL_EVENT_QUIT && !window_resized(&e)) {
                        continue;
                    }
                }
            }
#endif
            input_event(&e);
            if (window_key(&e)) {
                continue;
            }
            if (test_open && e.type != SDL_EVENT_QUIT) {
                int r = test_mode_keys(test_key(&e));
                if (r != TM_OPEN) {
                    test_open = 0;
                    restart_req = r == TM_RESTART;
                }
                continue;
            }
            if (e.type == SDL_EVENT_KEY_DOWN && key_scancode(&e) == SDL_SCANCODE_F2 && !e.key.repeat &&
                !menu_active() && strstr(game_title(), "Dreamcast") == NULL) {
                test_mode_open("ENTER", "ESC");
                test_open = 1;
                continue;
            }
            if (menu_event(&e)) {
                continue;
            }
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_KEY_DOWN && key_sym(&e) == SDLK_ESCAPE)) {
                quit = 1;
            }
            /* (ForkAde) F9 is quick load; switching the version is in the bar's Game menu */
        }
        if (test_open) {
            present_test();
            continue;
        }
        if (menu_active()) {
            present_menu();
            continue;
        }
#ifdef MVC2_SDL3
        if (pending_replay != NULL && host_frame() >= 30) {
            replay_play(pending_replay);
            pending_replay = NULL;
        }
        replay_dialog_take();
        if (online_wanted && !forkade_online_active() && host_frame() >= 30) {
            /* (ForkAde) the session, from the same frame on both sides, the board alike (host_online) */
            char why[256];
            if (!forkade_online_start(why, sizeof why)) {
                host_log("online: %s", why);
                quit = 1;
                continue;
            }
            SDL_StartTextInput(window);
        }
        if (forkade_online_active()) {
            read_input();
            forkade_online_tick(&input);
            bar_refresh();
            show(drawn != NULL ? drawn : screen);
            if (forkade_online_ended()) {
                if (online_end_ticks++ == 0) {
                    host_log("online: %s", forkade_online_message());
                }
                if (online_end_ticks > 180 || test_run) {
                    quit = 1;
                }
            }
            if (frames_left > 0 && --frames_left == 0) {
                quit = 1;
            }
            continue;
        }
        if (forkade_replay_playing() && !forkade_bar_open()) {
            forkade_replay_tick();
            bar_refresh();
            show(drawn != NULL ? drawn : screen);
            if (frames_left > 0 && --frames_left == 0) {
                quit = 1;
            }
            continue;
        }
        bar_refresh();
        if (forkade_bar_open()) {
            present_bar();
            if (bar_test_done) {
                quit = 1;   /* (MVC2_BARTEST: its picture taken) */
            }
            continue;
        }
#endif
        read_input();
        if (switch_req || restart_req) {
            int other = switch_req;
            switch_req = restart_req = 0;
            gpu_restart(1);
            if (other ? !switch_game() : (host_restart(), !start_game())) {
                quit = 1;
                continue;
            }
            gpu_restart(0);
            rd_quality(config_get("render"));   /* reset by the switch */
            SDL_SetWindowTitle(window, game_title());
            rd_begin_frame();
        }
        {
            Uint64 start = SDL_GetPerformanceCounter(), presenting = present_ticks;
            naomi_run_frame();
            sdl_audio_frame();
            game_ticks += SDL_GetPerformanceCounter() - start - (present_ticks - presenting);
            forkade_hash_frame(host_frame()); /* (ForkAde: MVC2_HASHLOG) */
#ifdef MVC2_SDL3
            forkade_replay_after_frame(&input);        /* (ForkAde: a match kept as it is fought) */
#endif
            {   /* (ForkAde: MVC2_STATETEST, the press list's clock goes back with the state) */
                static unsigned presented_saved;
                int t = forkade_state_test(host_frame());
                if (t == 1) {
                    presented_saved = presented;
                } else if (t == 2) {
                    presented = presented_saved;
                }
            }
            forkade_rb_test(host_frame(), &presented); /* (ForkAde: MVC2_RBTEST) */
#ifdef MVC2_SDL3
            bar_test(host_frame());                    /* (ForkAde: MVC2_BARTEST) */
#endif
        }
        if (frames_left > 0 && --frames_left == 0) {
            quit = 1;
        }
    }
#ifdef MVC2_SDL3
    forkade_online_stop();  /* (ForkAde) */
    forkade_replay_quit();
#endif
    forkade_hash_close();
    save_window();
    snd_close();
    sdl_audio_close();
    host_stub_report();
#ifdef MVC2_SDL3
    if (gpu) {
        SDL_DestroyRenderer(renderer);
        rd_gpu_quit();
        SDL_DestroyGPUDevice(gpu);
    }
#endif
    SDL_Quit();
    return 0;
}

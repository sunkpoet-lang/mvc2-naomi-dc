/*
 * state.c  (ForkAde) Save states, taken between frames (the game waiting in nlWaitVSync): the NAOMI's main RAM, the
 * game program's .data and .bss, the host's state (from state_begin.c to the host span's end: the board, Sega's
 * libraries, the sound side), the state heap (state_heap.c: those modules' memory and the game's coroutine and its
 * stack) and the sound chip's RAM. The drawing layer and the ROM's cache are left out: the one draws again from the
 * state, the other reads the same ROM.
 *
 * For rollback (the same run) and for replays' checkpoints; the executable has a fixed base, so a state also loads in
 * another run of the same build.
 */

#include "state.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "naomi_host.h"
#include "state_hash.h"
#include "state_heap.h"
#include "../snd/arm7.h"
#include "../snd/aica.h"

extern char forkade_state_data_begin[], forkade_state_bss_begin[];
extern char host_span_data_end[], host_span_bss_end[];

#define MAGIC 0x3253564Du /* "MVS2" */
#define VERSION 1u

typedef struct {
    uint32_t magic, version, parts, pad;
} HEADER;

typedef struct {
    void* at;
    uint32_t size;
} PART;

static int parts(PART* p) {
    int n = 0;
    p[n++] = (PART){ (void*)(uintptr_t)NAOMI_RAM_BASE, NAOMI_RAM_SIZE };
#ifdef MVC2_ARCADE
    p[n++] = (PART){ arc_program.span.data, (uint32_t)(arc_program.span.data_end - arc_program.span.data) };
    p[n++] = (PART){ arc_program.span.bss, (uint32_t)(arc_program.span.bss_end - arc_program.span.bss) };
#endif
#ifdef MVC2_DC
    p[n++] = (PART){ dc_program.span.data, (uint32_t)(dc_program.span.data_end - dc_program.span.data) };
    p[n++] = (PART){ dc_program.span.bss, (uint32_t)(dc_program.span.bss_end - dc_program.span.bss) };
#endif
    p[n++] = (PART){ forkade_state_data_begin, (uint32_t)(host_span_data_end - forkade_state_data_begin) };
    p[n++] = (PART){ forkade_state_bss_begin, (uint32_t)(host_span_bss_end - forkade_state_bss_begin) };
    p[n++] = (PART){ aica_ram, aica_ram != NULL ? aica_ram_size() : 0 };
    p[n++] = (PART){ state_heap_base(), state_heap_top() }; /* last: its size is the saved state's own */
    return n;
}

/* (for the checks: where each part starts) */
void forkade_state_parts(void (*fn)(int i, const void* at, uint32_t size)) {
    PART p[16];
    int n = parts(p);
    for (int i = 0; i < n; i++) {
        fn(i, p[i].at, p[i].size);
    }
}

size_t forkade_state_size(void) {
    PART p[16];
    int n = parts(p);
    size_t size = sizeof(HEADER);
    for (int i = 0; i < n; i++) {
        size += 4 + p[i].size;
    }
    return size;
}

size_t forkade_state_save(void* out, size_t room) {
    PART p[16];
    int n = parts(p);
    size_t size = forkade_state_size();
    if (size > room) {
        return 0;
    }
    uint8_t* w = out;
    HEADER h = { MAGIC, VERSION, (uint32_t)n, 0 };
    memcpy(w, &h, sizeof h);
    w += sizeof h;
    for (int i = 0; i < n; i++) {
        memcpy(w, &p[i].size, 4);
        memcpy(w + 4, p[i].at, p[i].size);
        w += 4 + p[i].size;
    }
    return size;
}

int forkade_state_load(const void* in, size_t size) {
    PART p[16];
    int n = parts(p);
    const uint8_t* r = in;
    const uint8_t* end = r + size;
    HEADER h;
    if (size < sizeof h) {
        return 0;
    }
    memcpy(&h, r, sizeof h);
    r += sizeof h;
    if (h.magic != MAGIC || h.version != VERSION || h.parts != (uint32_t)n) {
        return 0;
    }
    /* checked whole before anything is written */
    const uint8_t* q = r;
    for (int i = 0; i < n; i++) {
        uint32_t s;
        if (end - q < 4) {
            return 0;
        }
        memcpy(&s, q, 4);
        if ((i < n - 1 && s != p[i].size) || (uint32_t)(end - q - 4) < s) {
            return 0;
        }
        q += 4 + s;
    }
    for (int i = 0; i < n; i++) {
        uint32_t s;
        memcpy(&s, r, 4);
        if (i == n - 1) {
            if (!state_heap_restore(r + 4, s)) {
                return 0;
            }
        } else {
            memcpy(p[i].at, r + 4, s);
        }
        r += 4 + s;
    }
    return 1;
}

uint64_t forkade_state_host_hash(void) {
    PART p[16];
    int n = parts(p);
    uint64_t h = 0;
    for (int i = 1; i < n; i++) { /* (the RAM has its own) */
        h = h * 31 + forkade_hash(p[i].at, p[i].size);
    }
    return h;
}

/* MVC2_STATETEST=SAVE:LOAD: a state saved after frame SAVE and loaded after frame LOAD, once; the frames after it
   run again from SAVE, and the hash log shows whether they come out the same */
static void* test_state;
static size_t test_size;
static unsigned test_save, test_load;
static int test_checked, test_done;

int forkade_state_save_file(const char* path);
int forkade_state_load_file(const char* path);

/* (MVC2_STATEFILE=PATH: the test goes through that file) */
int forkade_state_test(unsigned frame) {
    if (!test_checked) {
        test_checked = 1;
        const char* t = getenv("MVC2_STATETEST");
        if (t == NULL || sscanf(t, "%u:%u", &test_save, &test_load) != 2 || test_load <= test_save) {
            test_done = 1;
        }
    }
    {   /* MVC2_STATELOADAT=FRAME:PATH: a state from a file (another run's) loaded after that frame */
        static int loaded_at;
        const char* t = getenv("MVC2_STATELOADAT");
        unsigned at;
        char path[512];
        if (!loaded_at && t != NULL && sscanf(t, "%u:%511s", &at, path) == 2 && frame == at) {
            loaded_at = 1;
            int ok = forkade_state_load_file(path);
            fprintf(stderr, "state test: %s %s after frame %u\n", ok ? "loaded" : "could not load", path, frame);
            return ok ? 2 : 0;
        }
    }
    if (test_done) {
        return 0;
    }
    if (frame == test_save && test_state == NULL) {
        test_size = forkade_state_size();
        test_state = malloc(test_size);
        clock_t t0 = clock();
        int saved = 0;
        for (int i = 0; i < 20 && test_state != NULL; i++) { /* (20 times, for the timing) */
            saved = getenv("MVC2_STATEFILE") != NULL ? forkade_state_save_file(getenv("MVC2_STATEFILE"))
                                                    : forkade_state_save(test_state, test_size) == test_size;
        }
        double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 20;
        if (!saved) {
            fprintf(stderr, "state test: the save failed\n");
            test_done = 1;
            return 0;
        }
        fprintf(stderr, "state test: saved after frame %u, %u KB, %.2f ms\n", frame, (unsigned)(test_size >> 10), ms);
        return 1;
    }
    if (frame == test_load && test_state != NULL) {
        clock_t t0 = clock();
        int ok = 1;
        for (int i = 0; i < 20 && ok; i++) { /* (20 times, for the timing) */
            ok = getenv("MVC2_STATEFILE") != NULL ? forkade_state_load_file(getenv("MVC2_STATEFILE"))
                                                 : forkade_state_load(test_state, test_size);
        }
        double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 20;
        fprintf(stderr, "state test: %s after frame %u, %.2f ms\n", ok ? "loaded" : "the load failed", frame, ms);
        test_done = 1;
        return ok ? 2 : 0;
    }
    return 0;
}

/* to a file and back (training's save slots, a replay's checkpoints) */
int forkade_state_save_file(const char* path) {
    size_t n = forkade_state_size();
    void* st = malloc(n);
    FILE* f = NULL;
    int ok = st != NULL && forkade_state_save(st, n) == n && (f = fopen(path, "wb")) != NULL &&
             fwrite(st, 1, n, f) == n;
    if (f != NULL && fclose(f) != 0) {
        ok = 0;
    }
    free(st);
    return ok;
}

int forkade_state_load_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* st = n > 0 ? malloc((size_t)n) : NULL;
    int ok = st != NULL && fread(st, 1, (size_t)n, f) == (size_t)n && forkade_state_load(st, (size_t)n);
    fclose(f);
    free(st);
    return ok;
}

/* (rollback.c) the parts copied whole, not watched: the programs' and the host's .data and .bss */
int forkade_state_spans(void** at, uint32_t* size, int room) {
    PART p[16];
    int n = parts(p), k = 0;
    for (int i = 1; i < n - 2 && k < room; i++) { /* not the RAM, the sound RAM or the heap */
        at[k] = p[i].at;
        size[k++] = p[i].size;
    }
    return k;
}

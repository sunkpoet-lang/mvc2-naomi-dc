/*
 * rollback.c  (ForkAde) Saves of the last frames in memory, for rollback: cheap enough to take after every frame.
 *
 * The big parts of a state (the main RAM, the sound RAM, the state heap) are allocated with MEM_WRITE_WATCH, so
 * Windows says which 4 KB pages were written since the last save. A shadow copy holds each as it was at the newest
 * save. A save takes, for each page written since, the shadow's page (the page as it was at the save before) into
 * that save's undo log, and the page as it is now into the shadow. Going back to save K: the pages written since the
 * newest save are put back from the shadow, then each newer save's undo log, newest first, back to K. The small parts
 * (the programs' and the host's .data and .bss, about 4 MB) are copied whole with each save.
 */

#include "rollback.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#include "naomi_host.h"
#include "state.h"
#include "state_heap.h"
#include "../snd/arm7.h"
#include "../snd/aica.h"

#define PAGE 4096u
#define MAX_SPANS 8

int forkade_state_spans(void** at, uint32_t* size, int room);

typedef struct {
    uint8_t* base;     /* watched memory */
    uint32_t size;     /* its reserved bytes (the heap's committed grow) */
    uint8_t* shadow;   /* reserved as big, committed as it is used */
    uint32_t shadow_committed;
} REGION;

typedef struct {
    uint8_t region;
    uint32_t page;     /* its offset in the region */
} UNDO_PAGE;

typedef struct {
    uint32_t frame;
    uint8_t* spans;     /* the small parts, whole */
    UNDO_PAGE* undo;    /* the pages this save changed, as they were at the save before */
    uint8_t* undo_data;
    uint32_t undo_count, undo_room;
} SLOT;

static REGION region[3];
static int nregions;
static void* span_at[MAX_SPANS];
static uint32_t span_size[MAX_SPANS];
static int nspans;
static uint32_t spans_total;
static SLOT* slot;
static int nslots, first, count; /* a ring: the oldest at first */
static void** written;            /* GetWriteWatch's answer */
static uint32_t written_room;
static uint32_t stat_saved, stat_loaded;

static int shadow_cover(REGION* r, uint32_t bytes) {
    if (bytes <= r->shadow_committed) {
        return 1;
    }
    bytes = (bytes + 0xFFFFFu) & ~0xFFFFFu;
    if (bytes > r->size) {
        bytes = r->size;
    }
    if (VirtualAlloc(r->shadow + r->shadow_committed, bytes - r->shadow_committed, MEM_COMMIT, PAGE_READWRITE) == NULL) {
        return 0;
    }
    r->shadow_committed = bytes;
    return 1;
}

static uint32_t region_used(const REGION* r, int i) {
    return i == 2 ? state_heap_committed() : r->size; /* (the heap: what is committed) */
}

static int add_region(void* base, uint32_t size) {
    REGION* r = &region[nregions];
    r->base = base;
    r->size = size;
    r->shadow = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE);
    r->shadow_committed = 0;
    if (r->shadow == NULL) {
        return 0;
    }
    nregions++;
    if (size / PAGE > written_room) {
        written_room = size / PAGE;
    }
    return 1;
}

int forkade_rb_init(int slots) {
    if (slot != NULL || aica_ram == NULL || state_heap_base() == NULL) {
        return slot != NULL;
    }
    if (!add_region((void*)(uintptr_t)NAOMI_RAM_BASE, NAOMI_RAM_SIZE) || !add_region(aica_ram, 8u << 20) ||
        !add_region(state_heap_base(), state_heap_reserved())) {
        return 0;
    }
    written = malloc(sizeof *written * written_room);
    nspans = forkade_state_spans(span_at, span_size, MAX_SPANS);
    spans_total = 0;
    for (int i = 0; i < nspans; i++) {
        spans_total += span_size[i];
    }
    slot = calloc((size_t)slots, sizeof *slot);
    if (written == NULL || slot == NULL) {
        return 0;
    }
    nslots = slots;
    for (int i = 0; i < slots; i++) {
        if ((slot[i].spans = malloc(spans_total)) == NULL) {
            return 0;
        }
    }
    /* the shadow as the memory is now, its watch cleared */
    for (int i = 0; i < nregions; i++) {
        REGION* r = &region[i];
        uint32_t used = region_used(r, i);
        if (!shadow_cover(r, used)) {
            return 0;
        }
        memcpy(r->shadow, r->base, used);
        ResetWriteWatch(r->base, r->size);
    }
    first = count = 0;
    return 1;
}

static int undo_add(SLOT* s, int reg, uint32_t page, const uint8_t* old) {
    if (s->undo_count == s->undo_room) {
        uint32_t room = s->undo_room ? s->undo_room * 2 : 256;
        UNDO_PAGE* u = realloc(s->undo, sizeof *u * room);
        if (u == NULL) {
            return 0;
        }
        s->undo = u;
        uint8_t* d = realloc(s->undo_data, (size_t)PAGE * room);
        if (d == NULL) {
            return 0;
        }
        s->undo_data = d;
        s->undo_room = room;
    }
    s->undo[s->undo_count].region = (uint8_t)reg;
    s->undo[s->undo_count].page = page;
    memcpy(s->undo_data + (size_t)PAGE * s->undo_count, old, PAGE);
    s->undo_count++;
    return 1;
}

int forkade_rb_save(uint32_t frame) {
    if (slot == NULL) {
        return 0;
    }
    if (count == nslots) { /* the oldest dropped */
        first = (first + 1) % nslots;
        count--;
    }
    SLOT* s = &slot[(first + count) % nslots];
    s->frame = frame;
    s->undo_count = 0;
    for (int i = 0; i < nregions; i++) {
        REGION* r = &region[i];
        ULONG_PTR n = written_room;
        DWORD gran;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, r->base, r->size, written, &n, &gran) != 0 ||
            !shadow_cover(r, region_used(r, i))) {
            return 0;
        }
        for (ULONG_PTR k = 0; k < n; k++) {
            uint32_t off = (uint32_t)((uint8_t*)written[k] - r->base);
            if (!shadow_cover(r, off + PAGE) || !undo_add(s, i, off, r->shadow + off)) {
                return 0;
            }
            memcpy(r->shadow + off, r->base + off, PAGE);
        }
        stat_saved += (uint32_t)n;
    }
    uint8_t* w = s->spans;
    for (int i = 0; i < nspans; i++) {
        memcpy(w, span_at[i], span_size[i]);
        w += span_size[i];
    }
    count++;
    return 1;
}

int forkade_rb_load(uint32_t frame) {
    int k;
    for (k = count - 1; k >= 0 && slot[(first + k) % nslots].frame != frame; k--) {
    }
    if (slot == NULL || k < 0) {
        return 0;
    }
    stat_loaded = 0;
    /* back to the newest save: the pages written since, from the shadow */
    for (int i = 0; i < nregions; i++) {
        REGION* r = &region[i];
        ULONG_PTR n = written_room;
        DWORD gran;
        if (GetWriteWatch(0, r->base, r->size, written, &n, &gran) != 0) {
            return 0;
        }
        for (ULONG_PTR j = 0; j < n; j++) {
            uint32_t off = (uint32_t)((uint8_t*)written[j] - r->base);
            if (off < r->shadow_committed) {
                memcpy(r->base + off, r->shadow + off, PAGE);
            }
        }
        stat_loaded += (uint32_t)n;
    }
    /* then each newer save undone, newest first */
    for (int j = count - 1; j > k; j--) {
        SLOT* s = &slot[(first + j) % nslots];
        for (uint32_t u = s->undo_count; u-- > 0;) {
            REGION* r = &region[s->undo[u].region];
            uint32_t off = s->undo[u].page;
            const uint8_t* old = s->undo_data + (size_t)PAGE * u;
            memcpy(r->base + off, old, PAGE);
            memcpy(r->shadow + off, old, PAGE);
        }
        stat_loaded += s->undo_count;
    }
    for (int i = 0; i < nregions; i++) {
        ResetWriteWatch(region[i].base, region[i].size); /* (its own writes) */
    }
    const uint8_t* rd = slot[(first + k) % nslots].spans;
    for (int i = 0; i < nspans; i++) {
        memcpy(span_at[i], rd, span_size[i]);
        rd += span_size[i];
    }
    count = k + 1; /* the saves after it dropped */
    return 1;
}

void forkade_rb_stats(uint32_t* pages_saved, uint32_t* pages_loaded) {
    *pages_saved = stat_saved;
    *pages_loaded = stat_loaded;
    stat_saved = 0;
}

/* MVC2_RBTEST=FROM:TO:EVERY:DEPTH: a save after each frame from FROM to TO, and every EVERY frames a rollback of DEPTH
   frames, the frames then run again (the hash log has them twice: they must match); the times go to stderr at TO.
   CLOCK is the front end's press clock, taken back with the state. */
int forkade_rb_test(uint32_t frame, unsigned* clock) {
    static int checked, on;
    static unsigned from, to, every, depth, last_back;
    static unsigned clock_at[256];
    static LARGE_INTEGER freq;
    static double save_ms, load_ms, worst_save, worst_load;
    static unsigned saves, loads;
    if (!checked) {
        checked = 1;
        const char* t = getenv("MVC2_RBTEST");
        on = t != NULL && sscanf(t, "%u:%u:%u:%u", &from, &to, &every, &depth) == 4 && every > depth && depth < 200;
        QueryPerformanceFrequency(&freq);
    }
    if (!on || frame < from || frame > to) {
        return 0;
    }
    if (slot == NULL && !forkade_rb_init((int)depth + 2)) {
        fprintf(stderr, "rollback test: could not start\n");
        on = 0;
        return 0;
    }
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    int ok = forkade_rb_save(frame);
    QueryPerformanceCounter(&b);
    double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)freq.QuadPart;
    save_ms += ms;
    worst_save = ms > worst_save ? ms : worst_save;
    saves++;
    clock_at[frame % 256] = *clock;
    if (!ok) {
        fprintf(stderr, "rollback test: the save after frame %u failed\n", frame);
        on = 0;
        return 0;
    }
    if (frame == to) {
        fprintf(stderr, "rollback test: %u saves, %.3f ms each (worst %.3f); %u rollbacks of %u frames, %.3f ms each "
                "(worst %.3f)\n", saves, save_ms / saves, worst_save, loads, depth, loads ? load_ms / loads : 0.0,
                worst_load);
        on = 0;
        return 0;
    }
    if (frame - from >= depth && frame % every == 0 && frame > last_back) {
        last_back = frame;
        QueryPerformanceCounter(&a);
        ok = forkade_rb_load(frame - depth);
        QueryPerformanceCounter(&b);
        ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)freq.QuadPart;
        load_ms += ms;
        worst_load = ms > worst_load ? ms : worst_load;
        loads++;
        if (!ok) {
            fprintf(stderr, "rollback test: the load of frame %u failed\n", frame - depth);
            on = 0;
            return 0;
        }
        *clock = clock_at[(frame - depth) % 256];
        return 1;
    }
    return 0;
}

/* After the memory was put back some other way (a whole state loaded): the shadow made the memory again, the saves
   dropped */
void forkade_rb_reset(void) {
    if (slot == NULL) {
        return;
    }
    for (int i = 0; i < nregions; i++) {
        REGION* r = &region[i];
        uint32_t used = region_used(r, i);
        if (shadow_cover(r, used)) {
            memcpy(r->shadow, r->base, used);
        }
        ResetWriteWatch(r->base, r->size);
    }
    first = count = 0;
}

int forkade_rb_ready(void) {
    return slot != NULL;
}

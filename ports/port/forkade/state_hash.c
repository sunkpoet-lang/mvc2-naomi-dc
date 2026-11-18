/*
 * state_hash.c  (ForkAde) A hash of the game's state after each frame, for the checks: MVC2_HASHLOG=FILE writes a
 * line per frame, "frame ram sound host", the NAOMI's main RAM, the sound side's RAM and the rest of a save state
 * (state.c), each hashed 64 bits at a time.
 * Two runs with the same inputs must give the same lines; a state saved and loaded must give them again.
 */

#include "state_hash.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "naomi_host.h"
#include "state.h"
#include "../snd/arm7.h"
#include "../snd/aica.h"

static FILE* hash_file;
static int hash_checked;

uint64_t forkade_hash(const void* data, size_t bytes) {
    /* FNV-1a over 64-bit words: quick, and any difference shows */
    const uint64_t* w = (const uint64_t*)data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < bytes / 8; i++) {
        h = (h ^ w[i]) * 1099511628211ull;
    }
    return h;
}

uint64_t forkade_ram_hash(void) {
    return forkade_hash((const void*)(uintptr_t)NAOMI_RAM_BASE, NAOMI_RAM_SIZE);
}

uint64_t forkade_sound_hash(void) {
    return aica_ram != NULL ? forkade_hash(aica_ram, aica_ram_size()) : 0;
}

void forkade_hash_frame(unsigned long frame) {
    if (!hash_checked) {
        hash_checked = 1;
        const char* path = getenv("MVC2_HASHLOG");
        if (path != NULL && *path) {
            hash_file = fopen(path, "w");
        }
    }
    {   /* MVC2_RAMDUMP=FRAME: the state after that frame, each time it comes (a loaded state brings it again) */
        static int dumps;
        const char* d = getenv("MVC2_RAMDUMP");
        if (d != NULL && strtoul(d, NULL, 10) == frame) {
            char name[64];
            snprintf(name, sizeof name, "state_%lu_%d.bin", frame, dumps++);
            size_t n = forkade_state_size();
            void* st = malloc(n);
            FILE* f = fopen(name, "wb");
            if (f != NULL && st != NULL && forkade_state_save(st, n) == n) {
                fwrite(st, 1, n, f); /* the whole state, the RAM first */
            }
            if (f != NULL) {
                fclose(f);
            }
            free(st);
        }
    }
    {   /* MVC2_VSLOG=1: the fight's state when it changes, to stderr */
        static int last = -1;
        const HOST_PROGRAM* p = naomi_program();
        if (getenv("MVC2_VSLOG") != NULL && p != NULL && p->vs_sys != NULL && *p->sys != NULL) {
            const unsigned char* v = (const unsigned char*)p->vs_sys;
            const unsigned char* sy = (const unsigned char*)*p->sys;
            int now = v[0] | v[0x3e] << 8 | v[0x45] << 16 | (v[0x46] & 1) << 24 | (sy[0x8b] & 1) << 25 | (sy[0] & 15) << 26;
            if (now != last) {
                fprintf(stderr, "vs frame %lu: vs_st %d winner %d rounds %d end %d vs_flag %d task %d\n", frame, v[0],
                        (signed char)v[0x3e], v[0x45], v[0x46], sy[0x8b], sy[0]);
                last = now;
            }
        }
    }
    if (hash_file != NULL) {
        fprintf(hash_file, "%lu %016llx %016llx %016llx\n", frame, (unsigned long long)forkade_ram_hash(),
                (unsigned long long)forkade_sound_hash(), (unsigned long long)forkade_state_host_hash());
    }
}

void forkade_hash_close(void) {
    if (hash_file != NULL) {
        fclose(hash_file);
        hash_file = NULL;
    }
}

/*
 * coro_x86.c  (ForkAde) Coroutines for 32-bit Windows without fibers: coro_swap saves ebp, ebx, esi, edi, the x87
 * control word, MXCSR and the thread's stack bounds and SEH head (fs:0, fs:4, fs:8), and swaps stacks. The coroutines
 * and their stacks are in the state heap (port/forkade/state_heap.c), at fixed addresses, so a save state that keeps
 * that heap keeps the game where it waits in nlWaitVSync, its frames and its saved registers (a fiber's live in
 * memory Windows owns).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "coro.h"
#include "../forkade/state_heap.h"

struct coro {
    void* sp;
    void* stack;
    unsigned stack_size;
    void (*fn)(void*);
    void* arg;
};

static coro main_coro;
static coro* current;

void coro_swap(void** save_sp, void* load_sp);
/* what it pushes, from the top: ebp ebx esi edi, x87 control word and MXCSR (8 bytes), fs:0 fs:4 fs:8 */
__asm__(
    ".text\n"
    ".globl _coro_swap\n"
    "_coro_swap:\n"
    "    pushl %ebp\n"
    "    pushl %ebx\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    subl $8, %esp\n"
    "    fnstcw (%esp)\n"
    "    stmxcsr 4(%esp)\n"
    "    pushl %fs:0\n"
    "    pushl %fs:4\n"
    "    pushl %fs:8\n"
    "    movl 40(%esp), %eax\n"
    "    movl %esp, (%eax)\n"
    "    movl 44(%esp), %esp\n"
    "    popl %fs:8\n"
    "    popl %fs:4\n"
    "    popl %fs:0\n"
    "    fldcw (%esp)\n"
    "    ldmxcsr 4(%esp)\n"
    "    addl $8, %esp\n"
    "    popl %edi\n"
    "    popl %esi\n"
    "    popl %ebx\n"
    "    popl %ebp\n"
    "    ret\n");

static void coro_start(void) {
    current->fn(current->arg);
    abort();                                            /* a coroutine never returns */
}

coro* coro_self(void) {
    if (current == NULL) {
        current = &main_coro;
    }
    return current;
}

/* as if it had switched away at coro_start's first instruction */
static void start_on_stack(coro* c, void (*fn)(void*), void* arg) {
    uint32_t* top = (uint32_t*)(((uintptr_t)c->stack + c->stack_size) & ~(uintptr_t)15);
    uint16_t cw;
    uint32_t mxcsr;
    c->fn = fn;
    c->arg = arg;
    memset(c->stack, 0, c->stack_size);
    __asm__ volatile("fnstcw %0" : "=m"(cw));
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    /* coro_start is entered with esp 4 below a 16-byte boundary, as after a call */
    *--top = 0;                                         /* (its return address: none) */
    *--top = (uint32_t)(uintptr_t)coro_start;           /* ret */
    *--top = 0;                                         /* ebp */
    *--top = 0;                                         /* ebx */
    *--top = 0;                                         /* esi */
    *--top = 0;                                         /* edi */
    *--top = mxcsr;
    *--top = cw;
    *--top = 0xFFFFFFFFu;                               /* fs:0, the SEH chain's end */
    *--top = (uint32_t)(uintptr_t)c->stack + c->stack_size; /* fs:4, the stack's base */
    *--top = (uint32_t)(uintptr_t)c->stack;             /* fs:8, its limit */
    c->sp = top;
}

coro* coro_create(void (*fn)(void*), void* arg, unsigned stack_size) {
    coro* c = state_calloc(1, sizeof(coro));
    coro_self();
    if (c == NULL || (c->stack = state_malloc(stack_size)) == NULL) {
        abort();
    }
    c->stack_size = stack_size;
    start_on_stack(c, fn, arg);
    return c;
}

void coro_restart(coro* c, void (*fn)(void*), void* arg) {
    if (c != current && c != &main_coro) {
        start_on_stack(c, fn, arg);
    }
}

void coro_delete(coro* c) {
    if (c != NULL && c != current && c != &main_coro) {
        state_free(c->stack);
        state_free(c);
    }
}

void coro_switch(coro* to) {
    coro* from = coro_self();
    if (to != from) {
        current = to;
        coro_swap(&from->sp, to->sp);
    }
}

/*
 * sel4/Thread.h -- Thread primitives for SemperOS on seL4
 *
 * Uses setjmp/longjmp for cooperative context switching (same semantics
 * as the gem5 backend but with musl's jmp_buf).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#include <setjmp.h>
}
#include <base/Types.h>

namespace m3 {

typedef void (*_thread_func)(void*);

struct Regs {
    jmp_buf jmpbuf;
    word_t rsp;     /* initial stack pointer (set by thread_init) */
    word_t rip;     /* initial entry point (set by thread_init) */
};

enum {
    T_STACK_WORDS = 16384  /* 128 KiB -- worker threads run full WorkLoop dispatch including revoke_rec (32 KiB frame) */
};

void thread_init(_thread_func func, void *arg, Regs *regs, word_t *stack);
extern "C" bool thread_save(Regs *regs);
extern "C" bool thread_resume(Regs *regs);

}

#endif

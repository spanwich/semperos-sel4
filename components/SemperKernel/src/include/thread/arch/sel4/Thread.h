/*
 * sel4/Thread.h -- Thread primitives for SemperOS on seL4
 *
 * Mirrors thread/arch/gem5/Thread.h. On sel4 we run single-threaded
 * (cooperative threading is stubbed out), but we need compatible types.
 */

#pragma once

#ifdef __cplusplus
#include <base/Types.h>

namespace m3 {

typedef void (*_thread_func)(void*);

struct Regs {
    word_t rbx;
    word_t rsp;
    word_t rbp;
    word_t r12;
    word_t r13;
    word_t r14;
    word_t r15;
    word_t rflags;
    word_t rdi;
};

enum {
    T_STACK_WORDS = 512
};

void thread_init(_thread_func func, void *arg, Regs *regs, word_t *stack);
extern "C" bool thread_save(Regs *regs);
extern "C" bool thread_resume(Regs *regs);

}

#endif

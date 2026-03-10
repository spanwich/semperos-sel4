# Task 09: ThreadManager::wait_for() -- Cooperative Multithreading

## What was implemented

1. **Real cooperative thread switching** (replacing stubs):
   - Implemented `thread_save()` using inline x86_64 assembly to save all
     callee-saved registers (rbx, rsp, rbp, r12-r15) plus the return address
     into the `Regs` struct. Returns true on first call (context saved),
     false when resumed.
   - Implemented `thread_resume()` to restore all callee-saved registers and
     jump to the saved return address with eax=0 (makes thread_save return false).
   - Implemented `thread_init()` to set up a new thread's stack with startup
     data (func pointer + arg) and initial register state pointing to a
     trampoline function.
   - The trampoline pops func/arg from the stack and calls `func(arg)`, then
     calls `ThreadManager::stop()` when the function returns.

2. **Worker threads for blocking support**:
   - Created 2 worker threads during `kernel_start()` (before WorkLoop).
   - Worker threads run the WorkLoop, ensuring incoming messages continue to
     be processed while the main thread is blocked in `wait_for()`.
   - Sequence: main thread blocks -> switches to worker -> worker runs
     WorkLoop -> incoming revokeFinish/ACK calls notify() -> yield()
     switches back to main thread.

3. **Increased thread stack size**:
   - Changed `T_STACK_WORDS` from 512 (4 KiB) to 4096 (32 KiB) in
     `thread/arch/sel4/Thread.h`. Worker threads run the full WorkLoop
     dispatch path including SyscallHandler, CapTable revoke_rec (recursive),
     and KernelcallHandler, which requires significant stack depth.

4. **New VPE0 tests**:
   - Test 10: EXCHANGE obtain (VPE0 obtains capability from VPE1).
   - Test 11: Chain revoke depth 10 (alternating delegate/obtain between
     VPE0 and VPE1, then revoke root). Exercises recursive revocation
     with the cooperative ThreadManager.

## Design document

See `docs/TASK09-DESIGN.md` for the full design rationale.

## Files changed

| File | Lines | Change |
|------|-------|--------|
| `components/SemperKernel/src/kernel/arch/sel4/libbase_stubs.cc` | ~100 lines replaced | Real thread_save/resume/init with x86_64 asm |
| `components/SemperKernel/src/include/thread/arch/sel4/Thread.h` | 1 line | T_STACK_WORDS: 512 -> 4096 |
| `components/SemperKernel/src/kernel/arch/sel4/kernel.cc` | +25 lines | Worker thread creation, g_kworkloop global |
| `components/VPE0/VPE0.c` | +100 lines | Tests 10-11 |

## Test results

- Standalone ring buffer tests: 10/10 PASS
- CAmkES build: SUCCESS (x86_64/pc99, build-node0)
- On-target tests: require Docker/QEMU (image available at build-node0/images/)
  - Expected: 11/11 (9 existing + 2 new)

## Technical details

### Thread context switch protocol

```
thread_save(regs):
  1. Save rbx, rsp (caller's), rbp, r12-r15, return addr, rdi to regs
  2. Return true (first call)
  -- when resumed via thread_resume: return false --

thread_resume(regs):
  1. Restore rbx, rsp, rbp, r12-r15, rdi from regs
  2. Load saved return address into rcx
  3. Set eax = 0 (makes thread_save return false)
  4. jmpq *rcx (jumps to saved return address)
```

### New thread startup

```
thread_init(func, arg, regs, stack):
  1. Align stack top to 16 bytes
  2. Push arg, then func onto stack
  3. Set regs->rsp = stack pointer (points at func)
  4. Set regs->rflags = &thread_trampoline

thread_trampoline():
  1. Pop func and arg from stack
  2. Call func(arg)
  3. Call ThreadManager::stop()

thread_resume -> jumps to trampoline -> calls func(arg)
```

## Open questions

1. **Nested blocking**: If the worker thread encounters a blocking operation
   while the main thread is already blocked, it switches to the second worker
   thread. Two workers support one level of nesting. Deeper nesting would
   require more workers. For the current prototype (single-kernel, no remote
   revocation traffic), one level is sufficient.

2. **Stack overflow detection**: No guard pages on worker thread stacks. A
   stack overflow would silently corrupt heap memory. Could add red zone
   checking in debug builds.

3. **On-target verification**: The thread switching was implemented but not
   yet tested on-target. The assembly is straightforward x86_64 callee-saved
   register save/restore, but edge cases (signal delivery, GCC optimization
   levels) should be verified under QEMU.

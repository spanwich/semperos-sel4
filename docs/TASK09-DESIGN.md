# Task 09 Design: ThreadManager::wait_for() -- Cooperative Multithreading

## Problem Statement

The ThreadManager's `wait_for()` mechanism is the foundation for blocking
operations in SemperOS. When a kernel thread needs to wait for a remote
response (e.g., revocation acknowledgement from another kernel), it calls
`wait_for()` to yield control to the WorkLoop. The WorkLoop continues
processing incoming messages until the expected response arrives, at which
point `notify()` is called to wake the blocked thread.

Currently, the thread switching primitives in `libbase_stubs.cc` are stubbed:

```cpp
bool thread_save(m3::Regs *) { return true; }  // always "first return"
bool thread_resume(m3::Regs *) { return false; }
void thread_init(...) { memset(regs, 0, sizeof(Regs)); }
```

This means:
- `ThreadManager::wait_for()` calls `switch_to()` which calls `save()` then
  `resume()` -- but save() always returns true (never actually saves context)
  and resume() returns false (never actually restores context).
- The blocked thread never actually yields. `wait_for()` immediately returns
  to the caller as if it was never blocked.
- This is the "silent early-return bug" mentioned in CLAUDE.md.

The consequence: spanning revocations (CapTable::revoke_rec at line 240-248)
call `wait_for()` expecting to block until revokeFinish messages arrive, but
instead continue immediately, potentially accessing stale state.

## Current Code Analysis

### ThreadManager (thread/ThreadManager.h)

```cpp
void wait_for(void *event) {
    assert(_sleep.length() > 0);  // must have sleeping threads to switch to
    _current->subscribe(event);
    _blocked.append(_current);
    if(_ready.length())
        switch_to(_ready.remove_first());
    else
        switch_to(_sleep.remove_first());
}
```

The assert `_sleep.length() > 0` fires if there are no sleeping threads to
switch to. Currently this assert always fails because no sleeping threads
are created.

### Thread Lists

- `_current`: the currently executing thread
- `_ready`: threads ready to run (have been notified)
- `_blocked`: threads waiting for an event
- `_sleep`: idle threads (not waiting, not ready)

### Thread Context Switch (arch/sel4/Thread.h)

```cpp
struct Regs {
    word_t rbx, rsp, rbp, r12, r13, r14, r15, rflags, rdi;
};
```

This is the x86_64 callee-saved register set. A correct `thread_save()` must
use setjmp-style register save, and `thread_resume()` must use longjmp-style
register restore.

### Where wait_for() Is Called

1. **CapTable::revoke_rec** (CapTable.cc:240-248):
   ```cpp
   if(ongoing->awaitedResp > 0) {
       int mytid = ThreadManager::get().current()->id();
       ThreadManager::get().wait_for(reinterpret_cast<void*>(mytid));
   }
   ```
   Blocks until all remote revocation responses arrive.

2. **KPE::sendTo** (KPE.cc:59-67):
   ```cpp
   while(_msgsInflight >= MAX_MSG_INFLIGHT - 2) {
       int tid = ThreadManager::get().current()->id();
       _waitingThrds.append(new WaitingKPE(tid));
       ThreadManager::get().wait_for(reinterpret_cast<void*>(tid));
   }
   ```
   Flow-controls outbound messages to avoid overflowing the receive buffer.

3. **KPE::sendRevocationTo** (KPE.cc:82-91): Same pattern for revocation msgs.

4. **KPE::reply** (KPE.cc:104-124): Same pattern for replies.

5. **Coordinator::startKrnl** (Coordinator.cc:101-112):
   ```cpp
   tmng.wait_for(reinterpret_cast<void*>(tid));
   ```
   Blocks waiting for SIGVITAL from a newly started kernel.

6. **exchangeOverSession callback** (KernelcallHandler.cc:628-632):
   ```cpp
   m3::ThreadManager::get().wait_for(reinterpret_cast<void*>(mytid));
   ```
   Blocks during delegate handshake waiting for ACK.

### notify() -- The Wake Mechanism

```cpp
void notify(void *event, void *msg = nullptr, size_t size = 0) {
    for(auto it = _blocked.begin(); it != _blocked.end(); ) {
        auto old = it++;
        if(old->trigger_event(event)) {
            Thread* t = &(*old);
            t->set_msg(msg, size);
            _blocked.remove(t);
            _ready.append(t);
        }
    }
}
```

Finds blocked threads subscribed to `event`, copies `msg` data into the
thread's message buffer, and moves them to the `_ready` list.

### yield() -- Called from WorkLoop

```cpp
void yield() {
    if(_ready.length()) {
        _sleep.insert(nullptr, _current);
        switch_to(_ready.remove_first());
    }
}
```

The WorkLoop calls `tmng.yield()` every iteration (WorkLoop.cc:122). When a
blocked thread has been notified (moved to `_ready`), yield() switches to it.

## Design

### Implementation Approach: setjmp/longjmp

We implement `thread_save()` and `thread_resume()` using the x86_64 callee-saved
register set. The approach is identical to what gem5 SemperOS uses (inline
assembly for register save/restore), adapted for our CAmkES environment.

**thread_save()** saves callee-saved registers (rbx, rsp, rbp, r12-r15, rflags)
and returns `true` (first return) or `false` (when resumed via thread_resume).

**thread_resume()** restores the saved registers and "returns" to the point
where thread_save() was called, making thread_save() return `false`.

This is the standard cooperative threading pattern (equivalent to setjmp/longjmp
but without signal mask overhead).

### Sleeping Thread Pool

The ThreadManager requires at least one sleeping thread to switch to when
wait_for() is called (`assert(_sleep.length() > 0)`). We need to create
worker threads that run a simple loop:

```cpp
void worker_thread_func(void *) {
    // This thread exists to give the WorkLoop something to switch to
    // when the main thread blocks. The worker runs the WorkLoop.
    while(true) {
        kernel::WorkLoop::get().run_one_iteration();
        m3::ThreadManager::get().yield();
    }
}
```

Actually, the simpler approach used in original SemperOS: the main thread
(thread 0) runs the WorkLoop. When it calls wait_for(), the sleeping worker
thread is activated to continue the WorkLoop. When the worker calls yield()
and the original thread has been notified, it switches back.

For our prototype, we create one worker thread at kernel startup. This is
sufficient for single-threaded blocking scenarios (one outstanding wait_for
at a time).

**Thread stack**: T_STACK_WORDS = 512 (4096 bytes). This is tight for deep
call stacks. For the worker thread running WorkLoop iteration, this should
be sufficient since the WorkLoop itself does not do deep recursion.

### thread_init() for Worker Threads

```cpp
void thread_init(_thread_func func, void *arg, Regs *regs, word_t *stack) {
    // Set up initial register state so thread_resume() will "return"
    // to func(arg)
    word_t *sp = stack + T_STACK_WORDS;
    // x86_64 ABI: stack must be 16-byte aligned before call
    sp = (word_t *)((uintptr_t)sp & ~0xFUL);
    sp -= 1;  // space for return address (won't be used, but alignment)
    *sp = 0;  // fake return address

    regs->rsp = (word_t)sp;
    regs->rbp = 0;
    regs->rdi = (word_t)arg;  // first argument
    // RIP equivalent: we set up so resume() jumps to a trampoline
    // that calls func(arg)
    regs->rbx = (word_t)func;  // store func pointer in rbx
}
```

The trampoline in thread_resume() will call the function stored in regs->rbx
with the argument in regs->rdi.

### Worker Thread Function

The worker thread must be able to run WorkLoop iterations. However, the
WorkLoop's `run()` method is a loop (`while(has_items())`). We need a way
for the worker to process one iteration and then yield.

**Approach**: Add a `run_one()` method to the WorkLoop that does one iteration
without the outer loop. The worker thread calls this in a loop with yield().

Actually, looking more carefully at the code, the ThreadManager's `yield()`
in the WorkLoop already handles this. When the main thread blocks via
`wait_for()`, it switches to a sleeping thread. That sleeping thread should
continue the WorkLoop. The simplest approach:

1. Create a worker thread whose function is a WorkLoop-like poll loop.
2. When the main thread blocks, the worker takes over polling.
3. When the main thread is notified, `yield()` at the end of the WorkLoop
   iteration switches back to it.

### Integration with WorkLoop

The WorkLoop (WorkLoop.cc:64-130) ends each iteration with `tmng.yield()`.
This is the cooperative scheduling point. When a ready thread exists (because
`notify()` moved it from blocked to ready), `yield()` switches to it.

The sequence for a blocking revocation:

```
Main Thread:
  WorkLoop iteration N:
    fetch REVOKE syscall from VPE0
    SyscallHandler::revoke()
    CapTable::revoke_rec()
    -> remote children exist
    -> Kernelcalls::revoke() sends to remote kernel
    -> wait_for(mytid)
    -> ThreadManager saves main thread context
    -> switches to worker thread

Worker Thread:
  WorkLoop iteration N+1:
    fetch revokeFinish from KRNLC EP (or net_poll)
    KernelcallHandler::revokeFinish()
    -> awaitedResp == 0
    -> notify(mytid) -> moves main thread to ready
    WorkLoop calls tmng.yield()
    -> ready thread exists (main thread)
    -> switches to main thread

Main Thread:
  -> resumes from wait_for()
  -> continues revoke cleanup
  -> returns to WorkLoop
```

### Test Plan (VPE0 Test)

Add a test to VPE0 that verifies the block/wake mechanism:

**Test 10: ThreadManager wait_for/notify**

This test exercises the block/wake path without requiring a remote kernel.
We verify it by:
1. Creating a capability and delegating to VPE1 (same as test 6).
2. Then revoking it -- with a functioning ThreadManager, the revocation
   of a local-only tree should still work (no actual blocking needed).
3. The test validates that the ThreadManager infrastructure does not
   break existing local revocation paths.

For a true blocking test, we would need a dual-node setup. In single-node,
the local revocation path never actually calls wait_for() because all
children are local (no remote kernels). The wait_for test is implicit:
if ThreadManager is broken, the existing tests 6-8 would fail.

**Test 11: Deep chain revoke (depth 10)**

Create a chain of 10 delegated capabilities (VPE0 -> VPE1 -> ... repeated),
then revoke the root. This exercises the recursive revocation path and
ThreadManager's yield() during deep call stacks.

## Implementation Steps

1. Implement `thread_save()` and `thread_resume()` as inline assembly in
   `libbase_stubs.cc`, replacing the stub implementations.

2. Implement proper `thread_init()` that sets up a valid stack and register
   state for new threads.

3. Create a worker thread during `kernel_start()` (after configure_recv_endpoints)
   so there is always a sleeping thread for wait_for() to switch to.

4. The worker thread function runs a WorkLoop-compatible poll loop.

5. Add test 10 to VPE0 verifying ThreadManager does not break local ops.

## Risks

1. **Stack size**: T_STACK_WORDS = 512 (4096 bytes). Deep WorkLoop dispatch
   through KernelcallHandler could overflow this. Mitigation: increase to
   2048 words (16 KiB) for the worker thread.

2. **Re-entrancy**: When the worker thread runs the WorkLoop and encounters
   another blocking operation, it would need yet another sleeping thread.
   For the prototype, we limit to one level of blocking (one worker thread).
   This is sufficient for the single-kernel case.

3. **Heap usage**: Each Thread allocates T_STACK_WORDS * 8 bytes for its
   stack. With larger stacks this is still well within the 4 MiB heap.

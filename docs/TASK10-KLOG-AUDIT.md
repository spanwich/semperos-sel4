# Task 10: KLOG Audit

## KLOG calls on benchmark hot paths

| File | Function | Line | Log level | Hot path? | Action |
|------|----------|------|-----------|-----------|--------|
| SyscallHandler.cc | LOG_SYS (creategate) | 406 | SYSC | Y | KLOG_V |
| SyscallHandler.cc | LOG_SYS (exchange) | 578 | SYSC | Y | KLOG_V |
| SyscallHandler.cc | LOG_SYS (revoke) | 1054 | SYSC | Y | KLOG_V |
| SyscallHandler.cc | SYS_ERROR | 54 | ERR | N (error) | Keep |
| SyscallHandler.cc | SYS_ERROR_DELAYED | 59 | ERR | N (error) | Keep |
| CapTable.cc | revoke continued | 248 | KRNLC | Y | KLOG_V |
| CapTable.cc | revoke continued | 309 | KRNLC | Y | KLOG_V |
| CapTable.cc | revoke error | 133 | ERR | N (error) | Keep |
| DTU.cc | config_recv_local | 236 | EPS | Y | KLOG_V |
| DTU.cc | config_recv_remote | 264 | EPS | Y | KLOG_V |
| DTU.cc | config_send_local | 291 | EPS | Y | KLOG_V |
| DTU.cc | config_send_remote | 311 | EPS | Y | KLOG_V |
| DTU.cc | invalidate failed | 189 | EPS | Y | KLOG_V |
| DTU.cc | errors | various | ERR | N (error) | Keep |
| Kernelcalls.cc | all KRNLC logs | various | KRNLC | Y (if cross-kernel) | KLOG_V |
| WorkLoop.cc | child process logs | 72-77 | VPES | N (host only) | Keep |

## Key finding

SYSC, KRNLC, and EPS log levels are NOT in `KernelLog::level = INFO | ERR`.
The KLOG macro's runtime `if(level & lvl)` check prevents Serial output, but
still evaluates `m3::ThreadManager::get().current()->id()` in the LOG macro
expansion. With KLOG_V and SEMPER_BENCH_MODE, the entire expression is compiled
out to `((void)0)`.

## Impact on benchmark numbers

The ~15ms per-iteration cost was NOT caused by KLOG serial output. The root
cause is seL4 cooperative scheduling overhead on single-core QEMU: each
syscall round-trip requires multiple `seL4_Yield()` context switches through
all 5+ CAmkES component threads. This is the genuine platform cost for
multi-component IPC on single-core emulation.

Chain depth (10/50/100) adds negligible overhead — all benchmarks show
identical ~15ms medians — confirming that the capability tree walk itself
is fast, and the fixed IPC scheduling cost dominates.

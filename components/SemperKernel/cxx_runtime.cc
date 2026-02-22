/*
 * cxx_runtime.cc -- Minimal C++ runtime for SemperKernel on CAmkES/seL4
 *
 * Provides:
 *   - operator new/delete (backed by musl libc malloc/free)
 *   - __cxa_pure_virtual (ABI requirement for abstract classes)
 *   - __cxa_atexit (no-op, kernel never exits)
 */

extern "C" {
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
}

void* operator new(size_t size) {
    void *ptr = malloc(size);
    if (!ptr)
        printf("[CXX] ERROR: malloc(%zu) failed\n", size);
    return ptr;
}

void* operator new[](size_t size) { return operator new(size); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

extern "C" {

void __cxa_pure_virtual() {
    printf("[CXX] FATAL: pure virtual call\n");
    while (1) {}
}

int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0; /* no-op — kernel never exits */
}

} /* extern "C" */

/* Kernel-internal benchmark globals for cap operation rdtsc measurement */
#ifdef SEMPER_BENCH_MODE
#include <stdint.h>
extern "C" uint64_t _cap_bench_t0     = 0;
extern "C" uint64_t _cap_bench_cycles = 0;
#endif

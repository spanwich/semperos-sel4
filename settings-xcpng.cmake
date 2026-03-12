#
# settings-xcpng.cmake -- XCP-ng build preset
#
# Sets cache variables for XCP-ng deployment. Load BEFORE settings.cmake:
#
#   cmake -G Ninja -DPLATFORM=pc99 \
#         -C ../projects/semperos-sel4/settings-xcpng.cmake \
#         -C ../projects/semperos-sel4/settings.cmake \
#         ../projects/semperos-sel4
#
# Or equivalently, pass -D flags directly:
#
#   cmake -G Ninja -DPLATFORM=pc99 \
#         -DSEMPEROS_NO_NETWORK=ON -DSEMPER_BENCH_MODE=ON \
#         -C ../projects/semperos-sel4/settings.cmake \
#         ../projects/semperos-sel4
#
# SPDX-License-Identifier: BSD-2-Clause
#

# Strip DTUBridge + E1000 (no NIC on XCP-ng for local benchmarks)
set(SEMPEROS_NO_NETWORK ON CACHE BOOL "Disable DTUBridge network code paths" FORCE)

# Clean benchmark measurements (no hot-path KLOG evaluation)
set(SEMPER_BENCH_MODE ON CACHE BOOL "Disable verbose hot-path kernel logging for benchmarking" FORCE)

# Enable seL4 TSC-based thread utilisation counters (for future Option F work)
set(KernelBenchmarks "generic" CACHE STRING "" FORCE)

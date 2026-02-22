#
# easy-settings.cmake -- Simplified settings for CAmkES x86_64 QEMU
#
# Usage:
#   mkdir build && cd build
#   cmake -DCMAKE_TOOLCHAIN_FILE=<path-to-seL4>/kernel/gcc.cmake \
#         -G Ninja \
#         -C ../easy-settings.cmake \
#         ..
#   ninja
#

set(PLATFORM "pc99" CACHE STRING "")
set(KernelArch "x86" CACHE STRING "")
set(KernelX86Sel4Arch "x86_64" CACHE STRING "")
set(SIMULATION ON CACHE BOOL "")
set(RELEASE OFF CACHE BOOL "")
set(CAmkESCPP ON CACHE BOOL "")

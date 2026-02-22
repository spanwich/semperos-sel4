#
# settings.cmake -- seL4/CAmkES build settings for SemperOS vDTU prototype
#
# This configures the seL4 kernel and CAmkES for x86_64 QEMU.
#

cmake_minimum_required(VERSION 3.7.2)

set(project_dir "${CMAKE_CURRENT_LIST_DIR}")
set(KernelArch "x86" CACHE STRING "" FORCE)
set(KernelX86Sel4Arch "x86_64" CACHE STRING "" FORCE)
set(KernelPlatform "pc99" CACHE STRING "" FORCE)

# Use MCS kernel for better temporal isolation (optional, can use classic)
# set(KernelIsMCS ON CACHE BOOL "" FORCE)

# Disable kernel printing for production, enable for debug
set(KernelPrinting ON CACHE BOOL "" FORCE)
set(KernelDebugBuild ON CACHE BOOL "" FORCE)

# CAmkES settings
set(CAmkESCPP ON CACHE BOOL "" FORCE)

# Simulation target: QEMU
set(SIMULATION ON CACHE BOOL "" FORCE)
set(RELEASE OFF CACHE BOOL "" FORCE)

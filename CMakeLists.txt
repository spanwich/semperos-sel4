#
# CMakeLists.txt -- Build configuration for SemperOS vDTU CAmkES prototype
#
# Directory structure:
#   components/include/    - Shared headers (vdtu_ring.h)
#   components/VDTUService - vDTU service component
#   components/SemperKernel - SemperOS kernel test stub
#   components/VPE0        - Application VPE test stub
#   src/                   - Shared library sources (vdtu_ring.c)
#   interfaces/            - CAmkES IDL files
#
# Build:
#   cd camkes-vm-examples
#   rm -rf build-semperos && mkdir build-semperos && cd build-semperos
#   cmake -G Ninja \
#         -DPLATFORM=pc99 \
#         -C ../projects/semperos-sel4/settings.cmake \
#         ../projects/semperos-sel4
#   ninja
#

cmake_minimum_required(VERSION 3.7.2)

project(semperos-sel4 C)

# Include settings (sets up seL4/CAmkES build environment)
include(settings.cmake)

# Shared include directory for all components
set(VDTU_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/components/include")

# Ring buffer shared library (compiled into each component)
set(VDTU_RING_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/vdtu_ring.c")

# =========================================================================
#  VDTUService Component
# =========================================================================
DeclareCAmkESComponent(VDTUService
    SOURCES
        components/VDTUService/VDTUService.c
        ${VDTU_RING_SRC}
    INCLUDES
        ${VDTU_INCLUDE_DIR}
)

# =========================================================================
#  SemperKernel Component
# =========================================================================
DeclareCAmkESComponent(SemperKernel
    SOURCES
        components/SemperKernel/SemperKernel.c
        ${VDTU_RING_SRC}
    INCLUDES
        ${VDTU_INCLUDE_DIR}
)

# =========================================================================
#  VPE0 Component
# =========================================================================
DeclareCAmkESComponent(VPE0
    SOURCES
        components/VPE0/VPE0.c
        ${VDTU_RING_SRC}
    INCLUDES
        ${VDTU_INCLUDE_DIR}
)

# =========================================================================
#  CAmkES Assembly
# =========================================================================
DeclareCAmkESRootserver(semperos-sel4.camkes)

# Generate final bootable image
GenerateCAmkESRootserver()

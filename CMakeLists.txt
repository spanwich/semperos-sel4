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

project(semperos-sel4 C CXX)

# Include settings (sets up seL4/CAmkES build environment)
include(settings.cmake)

# Shared include directory for all components
set(VDTU_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/components/include")

# Shared library sources (compiled into each component)
set(VDTU_RING_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/vdtu_ring.c")
set(VDTU_CHANNELS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/vdtu_channels.c")

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
#  SemperKernel Component — SemperOS kernel on seL4/CAmkES
# =========================================================================

# SemperOS kernel source directory
set(SK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/components/SemperKernel")
set(SK_KERNEL "${SK_DIR}/src/kernel")
set(SK_INCLUDE "${SK_DIR}/src/include")

DeclareCAmkESComponent(SemperKernel
    SOURCES
        # CAmkES entry point and C++ runtime
        components/SemperKernel/camkes_entry.c
        components/SemperKernel/cxx_runtime.cc
        components/SemperKernel/cxx_test.cc
        # vDTU shared library
        ${VDTU_RING_SRC}
        ${VDTU_CHANNELS_SRC}
        # arch/sel4/ backend (replaces gem5)
        ${SK_KERNEL}/arch/sel4/kernel.cc
        ${SK_KERNEL}/arch/sel4/DTU.cc
        ${SK_KERNEL}/arch/sel4/Platform.cc
        ${SK_KERNEL}/arch/sel4/VPE.cc
        ${SK_KERNEL}/arch/sel4/PEManager.cc
        ${SK_KERNEL}/arch/sel4/libbase_stubs.cc
        # Core kernel (architecture-independent)
        ${SK_KERNEL}/DTU.cc
        ${SK_KERNEL}/Gate.cc
        ${SK_KERNEL}/WorkLoop.cc
        ${SK_KERNEL}/SyscallHandler.cc
        ${SK_KERNEL}/KernelcallHandler.cc
        ${SK_KERNEL}/Kernelcalls.cc
        ${SK_KERNEL}/Coordinator.cc
        # Capability system
        ${SK_KERNEL}/cap/CapTable.cc
        ${SK_KERNEL}/cap/Capability.cc
        ${SK_KERNEL}/cap/Revocations.cc
        # Communication
        ${SK_KERNEL}/com/RecvBufs.cc
        ${SK_KERNEL}/com/Services.cc
        # Distributed Data Lookup
        ${SK_KERNEL}/ddl/MHTInstance.cc
        ${SK_KERNEL}/ddl/MHTPartition.cc
        ${SK_KERNEL}/ddl/MHTTypes.cc
        # Memory management
        ${SK_KERNEL}/mem/AddrSpace.cc
        ${SK_KERNEL}/mem/MainMemory.cc
        ${SK_KERNEL}/mem/MemoryMap.cc
        ${SK_KERNEL}/mem/Slab.cc
        # PE management (shared base)
        ${SK_KERNEL}/pes/VPE.cc
        ${SK_KERNEL}/pes/PEManager.cc
        ${SK_KERNEL}/pes/KPE.cc
        ${SK_KERNEL}/pes/RKVPE.cc
    INCLUDES
        ${VDTU_INCLUDE_DIR}
        ${SK_INCLUDE}
        ${SK_KERNEL}
    CXX_FLAGS
        -std=c++11 -fno-exceptions -fno-rtti -fno-threadsafe-statics
        -D__sel4__
    LINKER_LANGUAGE
        CXX
)

# =========================================================================
#  VPE0 Component
# =========================================================================
DeclareCAmkESComponent(VPE0
    SOURCES
        components/VPE0/VPE0.c
        ${VDTU_RING_SRC}
        ${VDTU_CHANNELS_SRC}
    INCLUDES
        ${VDTU_INCLUDE_DIR}
)

# =========================================================================
#  VPE1 Component — Passive VPE for EXCHANGE testing (Task 06)
# =========================================================================
DeclareCAmkESComponent(VPE1
    SOURCES
        components/VPE1/VPE1.c
)

# =========================================================================
#  DTUBridge Component — E1000 + lwIP UDP inter-node bridge
# =========================================================================

set(projects_dir "${CMAKE_CURRENT_LIST_DIR}/../")

# Locate lwIP
find_file(LWIP_PATH lwip PATHS ${projects_dir} CMAKE_FIND_ROOT_PATH_BOTH)
if("${LWIP_PATH}" STREQUAL "LWIP_PATH-NOTFOUND")
    message(FATAL_ERROR "Failed to find lwIP in ${projects_dir}")
endif()
mark_as_advanced(FORCE LWIP_PATH)

# Configure lwIP with DTUBridge's lwipopts.h
include(${projects_dir}/util_libs/liblwip/lwip_helpers.cmake)
AddLWIPConfiguration(${CMAKE_CURRENT_LIST_DIR}/components/DTUBridge)

# Minimal lwIP sources (UDP-only, no TCP)
set(LWIP_UDP_SOURCES
    ${LWIP_PATH}/src/core/def.c
    ${LWIP_PATH}/src/core/inet_chksum.c
    ${LWIP_PATH}/src/core/init.c
    ${LWIP_PATH}/src/core/ip.c
    ${LWIP_PATH}/src/core/mem.c
    ${LWIP_PATH}/src/core/memp.c
    ${LWIP_PATH}/src/core/netif.c
    ${LWIP_PATH}/src/core/pbuf.c
    ${LWIP_PATH}/src/core/raw.c
    ${LWIP_PATH}/src/core/stats.c
    ${LWIP_PATH}/src/core/sys.c
    ${LWIP_PATH}/src/core/timeouts.c
    ${LWIP_PATH}/src/core/udp.c
    ${LWIP_PATH}/src/core/ipv4/etharp.c
    ${LWIP_PATH}/src/core/ipv4/icmp.c
    ${LWIP_PATH}/src/core/ipv4/ip4.c
    ${LWIP_PATH}/src/core/ipv4/ip4_addr.c
    ${LWIP_PATH}/src/core/ipv4/ip4_frag.c
    ${LWIP_PATH}/src/netif/ethernet.c
    ${LWIP_PATH}/src/api/err.c
)

DeclareCAmkESComponent(DTUBridge
    SOURCES
        components/DTUBridge/DTUBridge.c
        ${LWIP_UDP_SOURCES}
    INCLUDES
        components/DTUBridge
        ${VDTU_INCLUDE_DIR}
        ${LWIP_PATH}/src/include
        ${projects_dir}/util_libs/liblwip/include
        ${projects_dir}/util_libs/liblwip/include/lwip
    C_FLAGS
        -DNODE_ID=${NODE_ID}
    LIBS
        lwip
)

# =========================================================================
#  CAmkES Assembly
# =========================================================================
DeclareCAmkESRootserver(semperos-sel4.camkes)

# Generate final bootable image
GenerateCAmkESRootserver()

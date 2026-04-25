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

# Bench mode: disable verbose hot-path kernel logging for clean measurements
option(SEMPER_BENCH_MODE "Disable verbose hot-path kernel logging for benchmarking" OFF)
set(SEMPER_BENCH_FLAG "")
if(SEMPER_BENCH_MODE)
    set(SEMPER_BENCH_FLAG "-DSEMPER_BENCH_MODE")
endif()

# Network mode: when ON, strips DTUBridge + E1000 from the build (XCP-ng)
option(SEMPEROS_NO_NETWORK "Disable DTUBridge network code paths" OFF)
set(SEMPEROS_NO_NET_FLAG "")
if(SEMPEROS_NO_NETWORK)
    set(SEMPEROS_NO_NET_FLAG "-DSEMPEROS_NO_NETWORK")
endif()

# Multi-node mode: enables global PE IDs + MHT cross-node wiring (FPT-175)
option(SEMPER_MULTI_NODE "Enable multi-node spanning protocol wiring" OFF)
set(SEMPER_MULTI_NODE_FLAG "")
if(SEMPER_MULTI_NODE)
    set(SEMPER_MULTI_NODE_FLAG "-DSEMPER_MULTI_NODE")
endif()

# Node identity (compile-time constants per image)
set(KERNEL_ID "0" CACHE STRING "Kernel identity (0, 1, 2)")
set(SELF_IP "192.168.100.10" CACHE STRING "This node's IP address")
set(PEER_IP_0 "192.168.100.11" CACHE STRING "First peer IP address")
set(PEER_IP_1 "192.168.100.12" CACHE STRING "Second peer IP address")
# Total nodes in cluster (2 = Docker dual-boot, 3 = full Hille spanning topology)
set(SEMPER_NUM_NODES "2" CACHE STRING "Total nodes in topology (2 or 3)")

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
        ${SK_KERNEL}/capbench.cc
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
    C_FLAGS
        -DKERNEL_ID=${KERNEL_ID}
        -DSEMPER_NUM_NODES=${SEMPER_NUM_NODES}
        ${SEMPEROS_NO_NET_FLAG}
        ${SEMPER_MULTI_NODE_FLAG}
    CXX_FLAGS
        -std=c++11 -fno-exceptions -fno-rtti -fno-threadsafe-statics
        -D__sel4__
        -DSEMPER_KERNEL_ID=${KERNEL_ID}
        -DSEMPER_NUM_NODES=${SEMPER_NUM_NODES}
        ${SEMPER_BENCH_FLAG}
        ${SEMPEROS_NO_NET_FLAG}
        ${SEMPER_MULTI_NODE_FLAG}
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
    C_FLAGS
        ${SEMPER_MULTI_NODE_FLAG}
        -DSEMPER_KERNEL_ID=${KERNEL_ID}
)

# =========================================================================
#  VPE1 Component — Passive VPE for EXCHANGE testing (Task 06)
# =========================================================================
DeclareCAmkESComponent(VPE1
    SOURCES
        components/VPE1/VPE1.c
        ${VDTU_RING_SRC}
        ${VDTU_CHANNELS_SRC}
    INCLUDES
        ${VDTU_INCLUDE_DIR}
    C_FLAGS
        -DSEMPER_KERNEL_ID=${KERNEL_ID}
)

# =========================================================================
#  DTUBridge Component — E1000 + lwIP UDP inter-node bridge
#  (Skipped when SEMPEROS_NO_NETWORK=ON, e.g. XCP-ng local benchmarks)
# =========================================================================

if(NOT SEMPEROS_NO_NETWORK)

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

# HACL* verified crypto (FPT-155: CryptoTransport)
set(HACL_DIR ${CMAKE_CURRENT_LIST_DIR}/hacl-star/dist/gcc-compatible)
set(HACL_KARAMEL_DIR ${CMAKE_CURRENT_LIST_DIR}/hacl-star/dist/karamel)
set(HACL_SOURCES
    ${HACL_DIR}/Hacl_AEAD_Chacha20Poly1305.c  # FPT-155: per-packet AEAD
    ${HACL_DIR}/Hacl_Chacha20.c                # ChaCha20 stream cipher (dep)
    ${HACL_DIR}/Hacl_MAC_Poly1305.c            # Poly1305 MAC (dep)
    ${HACL_DIR}/Hacl_Ed25519.c                 # FPT-157: signing
    ${HACL_DIR}/Hacl_Curve25519_51.c           # FPT-157: key exchange
    ${HACL_DIR}/Hacl_HKDF.c                    # FPT-157: key derivation
    ${HACL_DIR}/Hacl_HMAC.c                    # HKDF dependency
    ${HACL_DIR}/Hacl_Hash_SHA2.c               # HMAC/Ed25519 dependency
    ${HACL_DIR}/Hacl_Hash_Blake2b.c            # HMAC dependency (generic)
    ${HACL_DIR}/Hacl_Hash_Blake2s.c            # HMAC dependency (generic)
    ${HACL_DIR}/Hacl_Hash_MD5.c                # HMAC dependency (generic)
    ${HACL_DIR}/Hacl_Hash_SHA1.c               # HMAC dependency (generic)
    ${HACL_DIR}/Hacl_Hash_SHA3.c               # Ed25519 dependency
    ${HACL_DIR}/Lib_Memzero0.c                 # FPT-166: secure zeroization
)

DeclareCAmkESComponent(DTUBridge
    SOURCES
        components/DTUBridge/DTUBridge.c
        components/DTUBridge/crypto_transport.c
        ${VDTU_RING_SRC}
        ${LWIP_UDP_SOURCES}
        ${HACL_SOURCES}
    INCLUDES
        components/DTUBridge
        ${VDTU_INCLUDE_DIR}
        ${LWIP_PATH}/src/include
        ${projects_dir}/util_libs/liblwip/include
        ${projects_dir}/util_libs/liblwip/include/lwip
        ${HACL_DIR}
        ${HACL_DIR}/internal
        ${HACL_KARAMEL_DIR}/include
        ${HACL_KARAMEL_DIR}/krmllib/dist/minimal
    C_FLAGS
        -DNODE_ID=${NODE_ID}
        -DKERNEL_ID=${KERNEL_ID}
        -DDTUB_SELF_IP=\"${SELF_IP}\"
        -DDTUB_PEER_IP_0=\"${PEER_IP_0}\"
        -DDTUB_PEER_IP_1=\"${PEER_IP_1}\"
        -DSEMPER_NUM_NODES=${SEMPER_NUM_NODES}
        -ffunction-sections -fdata-sections
)

endif(NOT SEMPEROS_NO_NETWORK)

# =========================================================================
#  CAmkES Assembly
# =========================================================================
if(SEMPEROS_NO_NETWORK)
    DeclareCAmkESRootserver(semperos-sel4-xcpng.camkes)
else()
    DeclareCAmkESRootserver(semperos-sel4.camkes)
endif()

# Generate final bootable image
GenerateCAmkESRootserver()

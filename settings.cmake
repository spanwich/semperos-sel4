#
# settings.cmake -- seL4/CAmkES build settings for SemperOS vDTU prototype
#
# This configures the seL4 kernel and CAmkES for x86_64 QEMU.
# Follows the same pattern as http_gateway_x86/settings.cmake.
#

cmake_minimum_required(VERSION 3.7.2)

set(project_dir "${CMAKE_CURRENT_LIST_DIR}/../")
file(GLOB project_modules ${project_dir}/*)
list(
    APPEND
        CMAKE_MODULE_PATH
        ${project_dir}/../kernel
        ${project_dir}/../tools/seL4/cmake-tool/helpers/
        ${project_dir}/../tools/seL4/elfloader-tool/
        ${project_modules}
)

set(SEL4_CONFIG_DEFAULT_ADVANCED ON)
set(CAMKES_CONFIG_DEFAULT_ADVANCED ON)
mark_as_advanced(CMAKE_INSTALL_PREFIX)

include(application_settings)

# Include easy settings for platform configuration
include(${CMAKE_CURRENT_LIST_DIR}/easy-settings.cmake)

# Kernel settings for x86_64 QEMU
set(KernelArch "x86" CACHE STRING "" FORCE)
set(KernelSel4Arch "x86_64" CACHE STRING "" FORCE)

# No hypervisor support (bare-metal CAmkES)
set(KernelX86HypervisorSupport OFF CACHE BOOL "" FORCE)

# Disable huge pages - QEMU TCG mode doesn't support 1GB huge pages
set(KernelHugePage OFF CACHE BOOL "" FORCE)

# Disable PCID - QEMU TCG mode doesn't support PCID well
set(KernelSupportPCID OFF CACHE BOOL "" FORCE)

# Use generic micro-architecture for QEMU TCG compatibility
set(KernelX86MicroArch "generic" CACHE STRING "" FORCE)

set(KernelRootCNodeSizeBits 18 CACHE STRING "" FORCE)

# Enable kernel printing and debug for prototype
set(KernelPrinting ON CACHE BOOL "" FORCE)
set(KernelDebugBuild ON CACHE BOOL "" FORCE)

# capDL settings
set(CapDLLoaderMaxObjects 90000 CACHE STRING "" FORCE)

# CAmkES settings
set(CAmkESCPP ON CACHE BOOL "" FORCE)

# Platform configuration
correct_platform_strings()

find_package(seL4 REQUIRED)
sel4_configure_platform_settings()

ApplyData61ElfLoaderSettings(${KernelPlatform} ${KernelSel4Arch})

# Release settings
if(NOT DEFINED RELEASE)
    set(RELEASE OFF)
endif()
ApplyCommonReleaseVerificationSettings(${RELEASE} FALSE)

# Single node configuration
set(KernelMaxNumNodes 1 CACHE STRING "" FORCE)

# Include CAmkES helpers
find_package(camkes-tool REQUIRED)
camkes_tool_setup_camkes_build_environment()

# Find global components
find_file(GLOBAL_COMPONENTS_PATH global-components.cmake
    PATHS ${project_dir}/global-components/
    CMAKE_FIND_ROOT_PATH_BOTH)
mark_as_advanced(FORCE GLOBAL_COMPONENTS_PATH)
if("${GLOBAL_COMPONENTS_PATH}" STREQUAL "GLOBAL_COMPONENTS_PATH-NOTFOUND")
    message(FATAL_ERROR "Failed to find global-components.cmake in ${project_dir}/global-components/")
endif()
include(${GLOBAL_COMPONENTS_PATH})

# Default heap size for components (SemperOS kernel needs ~4 MiB for buffers)
set(CAmkESDefaultHeapSize 4194304 CACHE STRING "" FORCE)

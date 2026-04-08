---
name: semperos-build
description: Use this agent for building the semperos-sel4 CAmkES system (QEMU or XCP-ng), diagnosing build failures, resolving cmake/ninja issues, and fixing compilation errors. Trigger when you see build errors, missing symbols, linker failures, or need to set up a fresh build directory. Example - "The ninja build is failing with undefined reference to vdtu_ring_send"
tools: Grep, Read, Edit, Write, Glob, Bash
model: sonnet
color: green
---

# SemperOS-seL4 Build Agent

## Purpose

Build and diagnose the semperos-sel4 CAmkES system. This is a seL4 x86_64
CAmkES application with C++11 kernel code, custom SPSC ring buffers, and
an E1000 network driver with lwIP UDP.

## Project Paths

- **camkes-vm-examples root:** `/home/iamfo470/phd/camkes-vm-examples`
- **Project root:** `/home/iamfo470/phd/camkes-vm-examples/projects/semperos-sel4`
- **CAmkES assembly:** `semperos-sel4.camkes` (full, with DTUBridge)
- **XCP-ng assembly:** `semperos-sel4-xcpng.camkes` (stripped, no DTUBridge)
- **Build settings:** `settings.cmake` (full), `easy-settings.cmake` (QEMU dev)
- **XCP-ng settings:** `settings-xcpng.cmake`

## Build Commands

### QEMU Development Build

```bash
cd /home/iamfo470/phd/camkes-vm-examples
mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../kernel/gcc.cmake \
      -G Ninja -C ../projects/semperos-sel4/easy-settings.cmake \
      ../projects/semperos-sel4
ninja
./simulate
```

### XCP-ng ISO Build (no network)

```bash
cd /home/iamfo470/phd/camkes-vm-examples
rm -rf build-xcpng && mkdir build-xcpng && cd build-xcpng
cmake -G Ninja \
    -DPLATFORM=pc99 \
    -DSEMPEROS_NO_NETWORK=ON \
    -DSEMPER_BENCH_MODE=ON \
    -C ../projects/semperos-sel4/settings.cmake \
    ../projects/semperos-sel4
ninja
../projects/semperos-sel4/scripts/make-iso.sh .
```

### Three-Node ISO Build (with DTUBridge)

Use `/build-isos` skill. Or manually per-node:

```bash
rm -rf build-node${N} && mkdir build-node${N} && cd build-node${N}
cmake -G Ninja \
    -DPLATFORM=pc99 \
    -DSEMPEROS_NO_NETWORK=OFF \
    -DSEMPER_BENCH_MODE=ON \
    -DKERNEL_ID=${N} \
    -DSELF_IP=${SELF_IP} \
    -DPEER_IP_0=${PEER_0} \
    -DPEER_IP_1=${PEER_1} \
    -C ../projects/semperos-sel4/settings.cmake \
    ../projects/semperos-sel4
ninja
```

Platform: pc99 (x86_64), simulation mode, debug build with kernel printing.

## Critical Build Knowledge

### C++ in CAmkES

SemperOS kernel is C++11. CAmkES natively supports `.cc` files via
`DeclareCAmkESComponent`. The seL4 toolchain applies `-nostdinc++`, so:

- `<cassert>` -> `<assert.h>`, `<cstring>` -> `<string.h>`, etc.
- `<functional>` -> `base/util/Functional.h` (custom `std::function`/`std::bind`/`std::move`)
- `operator new/delete` -> musl `malloc`/`free` (in `cxx_runtime.cc`)
- Kernel heap: 8 MiB static BSS buffer in `camkes_entry.c`
- `<camkes.h>` is not includable from C++ — CAmkES symbols declared manually
  via `extern "C"` in DTU.cc and camkes_entry.c

### CAmkES Heap Fix (CRITICAL)

CAmkES forces `LibSel4MuslcSysMorecoreBytes=0` in `camkes.cmake`, disabling
musl's static morecore. CAmkES provides a 1 MiB heap via its
`component.common.c` template, but the `CAmkESDefaultHeapSize` cmake variable
override does NOT propagate to the generated config header.

**Fix:** `camkes_entry.c` defines an 8 MiB static buffer and a
`__attribute__((constructor(200)))` function that sets
`morecore_area`/`morecore_size` BEFORE musl init (priority 201) and CAmkES init
(priority 202). This ensures the first malloc call uses the 8 MiB buffer.

### Key Build Files

```
CMakeLists.txt                        <- build entry point
settings.cmake                        <- seL4/CAmkES build settings
easy-settings.cmake                   <- QEMU dev preset
settings-xcpng.cmake                  <- XCP-ng preset
semperos-sel4.camkes                  <- full CAmkES assembly
semperos-sel4-xcpng.camkes            <- XCP-ng stripped assembly
components/SemperKernel/camkes_entry.c <- lifecycle + heap fix
scripts/make-iso.sh                   <- grub-mkrescue wrapper
scripts/grub.cfg                      <- GRUB2 multiboot config
```

### Common Build Issues

1. **Missing toolchain:** Ensure `gcc.cmake` path is correct relative to build dir
2. **Heap exhaustion:** Check 8 MiB buffer in `camkes_entry.c` constructor priority
3. **C++ link errors:** Verify `cxx_runtime.cc` is in CMakeLists.txt sources
4. **E1000 not found:** Check `-nic none` flag + explicit e1000 in QEMU command
5. **ISO creation fails:** Need `grub-mkrescue` and `xorriso` installed

### Anti-Patterns

- Do NOT use VMM / VT-x / hypervisor code (evaluated and rejected)
- Do NOT use Microkit (superseded by CAmkES)
- Do NOT add ARM code paths (x86_64 only)
- Do NOT modify `CAmkESDefaultHeapSize` cmake var — it doesn't propagate
- Do NOT run QEMU directly — use Docker infrastructure

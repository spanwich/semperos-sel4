# XCP-ng Port Audit — sel4_xcpng Reference Project Analysis

**Date:** 2026-03-10
**Purpose:** Determine the minimum changes to produce a bootable semperos-sel4 ISO for XCP-ng with captured benchmark output.

---

## 1. sel4_xcpng Project Structure

```
sel4_xcpng/
├── CMakeLists.txt               # CAmkES build entry (single component)
├── sel4_xcpng.camkes             # Minimal assembly: one HelloXcpng instance
├── settings.cmake                # seL4/CAmkES kernel config (pc99, x86_64, no HVM)
├── README.md                     # Build + deploy instructions
├── components/
│   └── HelloXcpng/
│       └── HelloXcpng.c          # "seL4 booted on XCP-ng!" + yield loop
├── scripts/
│   ├── grub.cfg                  # GRUB2 multiboot: kernel + capdl-loader
│   └── make-iso.sh               # grub-mkrescue wrapper → sel4-xcpng.iso
└── logserver/
    ├── sel4-serial-logger.sh     # socat TCP listener → /var/log/sel4/serial.log
    ├── sel4-serial-logger.service # systemd unit for the logger
    └── setup-logserver.sh        # One-command install on a log server VM
```

Total: 10 files, ~250 lines of code. This is a Phase 1 "proof of boot" project, not a full application.

---

## 2. ISO Build Mechanism

### (a) Tool: `grub-mkrescue` (backed by `xorriso`)

`scripts/make-iso.sh` creates a staging directory, copies the seL4 kernel and capDL loader into `/boot/`, copies `grub.cfg` into `/boot/grub/`, then calls:

```bash
grub-mkrescue -o "$OUTPUT" "$STAGING"
```

Dependencies: `grub-common`, `grub-pc-bin`, `xorriso` (all standard Debian/Ubuntu packages).

### (b) Boot sequence

```
BIOS → GRUB2 (from ISO El Torito) → multiboot1/2 →
  load /boot/kernel-x86_64-pc99     (seL4 kernel ELF, multiboot-compliant)
  load /boot/capdl-loader-image-x86_64-pc99  (rootserver image)
→ seL4 kernel init → elfloader → capDL loader → CAmkES components
```

grub.cfg (line-for-line):
```
serial --unit=0 --speed=115200
terminal_input serial
terminal_output serial
set timeout=3
set default=0
menuentry "seL4 x86_64 (XCP-ng)" {
    multiboot /boot/kernel-x86_64-pc99
    module /boot/capdl-loader-image-x86_64-pc99
    boot
}
```

The `serial` directives route GRUB output to serial. On XCP-ng, this serial line is the HVM serial console, which can be redirected to TCP via `hvm_serial=tcp:<ip>:<port>`.

### (c) Platform target: `x86_64/pc99`

Identical to semperos-sel4. Both projects set:
- `PLATFORM = "pc99"`
- `KernelArch = "x86"`
- `KernelSel4Arch = "x86_64"`
- `KernelX86HypervisorSupport = OFF`

### (d) Memory layout

No hardcoded RAM size. seL4's elfloader detects available memory from the multiboot memory map provided by GRUB. XCP-ng sets VM memory via `xe vm-memory-limits-set`. The reference project uses 512 MiB.

### (e) Multiboot: yes

seL4's x86_64 kernel contains multiboot1 and multiboot2 headers. GRUB loads it via the `multiboot` command. The capDL loader is passed as a `module`.

### (f) Exact build command

```bash
cd camkes-vm-examples
rm -rf build-xcpng && mkdir build-xcpng && cd build-xcpng
cmake -G Ninja \
    -DPLATFORM=pc99 \
    -C ../projects/sel4_xcpng/settings.cmake \
    ../projects/sel4_xcpng
ninja
# ISO:
../projects/sel4_xcpng/scripts/make-iso.sh .
```

Output: `build-xcpng/sel4-xcpng.iso`

---

## 3. TCP Log Transport

### Key finding: NOT an in-guest network stack

The "TCP log" is a **Xen HVM serial redirect**, not a CAmkES component or lwIP-based transport. seL4's standard serial output (printf/KLOG) goes through the seL4 serial driver → Xen HVM serial backend → TCP socket on the host. No code changes inside seL4 are needed.

### (a) Implementation

Host-side only. `logserver/sel4-serial-logger.sh` is a bash script that runs `socat TCP-LISTEN:$PORT,reuseaddr - | while read line; do ...` to capture serial output.

### (b) Network stack: N/A (Xen provides the transport)

The serial-to-TCP bridge is handled by Xen's device model, not by seL4.

### (c) NIC driver: N/A (no in-guest NIC needed for logging)

seL4 writes to its serial port (I/O port 0x3F8). Xen's HVM device model intercepts this and routes it over TCP if configured with `hvm_serial=tcp:`.

### (d) Configuration

On the XCP-ng host, set the VM's HVM serial parameter:
```bash
xe vm-param-set uuid=$VM_UUID platform:hvm_serial=tcp:$LOG_SERVER_IP:$PORT
```

The log server listens on port 9514 by default.

### (e) Protocol: raw serial stream

Line-buffered text. The logger prepends ISO 8601 timestamps:
```
[2026-03-10T14:30:00+00:00] seL4 booted on XCP-ng! Phase 1 success.
```

### (f) Replacement for seL4 serial? No — it IS seL4 serial

`printf()` → seL4 serial driver → Xen serial backend → TCP. Both serial console and TCP work simultaneously when `hvm_serial=tcp:` is set.

### (g) Host-side receiver

`logserver/sel4-serial-logger.sh` uses `socat`. The `setup-logserver.sh` script installs it as a systemd service. Alternatively, `nc -l 9514` works for one-shot captures.

---

## 4. CAmkES Component Structure

Minimal single-component assembly:

```
Assembly:
  └── HelloXcpng (hello)
        control thread, priority 150
        run() → printf("seL4 booted on XCP-ng!") → seL4_Yield() loop
```

- No procedure interfaces (no RPC)
- No dataports (no shared memory)
- No hardware components (no MMIO, no interrupts)
- No notifications

The application component connects to log transport implicitly through seL4's serial driver — `printf()` writes to serial, Xen routes it to TCP.

---

## 5. Compatibility with semperos-sel4

| Aspect | sel4_xcpng | semperos-sel4 | Compatible? | Notes |
|--------|-----------|---------------|-------------|-------|
| seL4 version | Same repo (camkes-vm-examples) | Same repo | Yes | Both build from same kernel source |
| CAmkES version | Same repo | Same repo | Yes | Shared tools/seL4/ |
| Platform target | pc99 (x86_64) | pc99 (x86_64) | Yes | Identical |
| KernelArch | x86 / x86_64 | x86 / x86_64 | Yes | Identical |
| KernelX86HypervisorSupport | OFF | OFF | Yes | Identical |
| KernelHugePage | OFF | OFF | Yes | Identical |
| KernelSupportPCID | OFF | OFF | Yes | Identical |
| KernelX86MicroArch | generic | generic | Yes | Identical |
| KernelRootCNodeSizeBits | 18 | 18 | Yes | Identical |
| CapDLLoaderMaxObjects | 90000 | 90000 | Yes | Identical |
| KernelMaxNumNodes | 1 | 1 | Yes | Identical |
| KernelPrinting | (default/OFF) | ON | Minor | Semperos enables for debug; leave ON for benchmarks |
| KernelDebugBuild | (default) | ON | Minor | Semperos enables for debug; may want OFF for bench |
| CAmkESDefaultHeapSize | (default 1 MiB) | 4 MiB | Irrelevant | Semperos uses static BSS morecore override anyway |
| LibLwip | (not set) | ON | Blocker if DTUBridge included | sel4_xcpng has no NIC; semperos needs lwIP for DTUBridge |
| NIC driver | None | E1000 (82540EM) | **Blocker** | See below |
| Components | 1 (HelloXcpng) | 5 (vdtu, kernel, vpe0, vpe1, dtu_bridge) | N/A | Semperos is much larger |
| GRUB config | multiboot kernel + capDL | N/A (no ISO yet) | N/A | Must adapt grub.cfg filenames |
| ISO build | grub-mkrescue | N/A (no ISO yet) | N/A | Copy make-iso.sh |
| Log transport | Xen hvm_serial TCP redirect | Serial (QEMU -nographic) | Yes | Same printf/serial path |
| NODE_ID | N/A | 0 or 1 | Irrelevant | XCP-ng is single-node for Exp 2A |

### Blockers

**E1000 NIC on XCP-ng**: The semperos-sel4 assembly hardcodes an Intel 82540EM at PCI 0:2.0, MMIO 0xFEB80000, IOAPIC pin 11. On XCP-ng HVM, the default NIC is a Realtek 8139 or Xen netfront (PV). The E1000 MMIO address will differ. DTUBridge init will crash or hang if the E1000 is not present at the expected address.

**Resolution options** (in order of simplicity):
1. **Remove DTUBridge from the assembly for XCP-ng builds.** Exp 2A benchmarks are all local — no cross-node transport needed. Create a `semperos-sel4-xcpng.camkes` without DTUBridge, HWEthDriver, and HWPCIConfig.
2. **Add an e1000 NIC to the XCP-ng VM** and update the MMIO address. XCP-ng/QEMU can emulate e1000, but the address is assigned by the VM's virtual BIOS, not hardcoded.
3. **Make DTUBridge init failure non-fatal.** DTUBridge could catch the PCI probe failure and enter a passive yield loop. Most complexity for least gain.

**Recommendation: Option 1.** A stripped `.camkes` assembly is simplest and avoids any NIC complications. DTUBridge is only needed for cross-node tests, which are not part of Exp 2A.

---

## 6. Port Plan

### [COPY] — Take directly from sel4_xcpng

| Item | Source | Destination | Effort |
|------|--------|-------------|--------|
| GRUB config | `sel4_xcpng/scripts/grub.cfg` | `semperos-sel4/scripts/grub.cfg` | Small |
| ISO build script | `sel4_xcpng/scripts/make-iso.sh` | `semperos-sel4/scripts/make-iso.sh` | Small |
| Serial logger | `sel4_xcpng/logserver/*` (3 files) | `semperos-sel4/logserver/*` | Small |

### [ADAPT] — Modify sel4_xcpng pattern for semperos-sel4

| Item | Change | Effort |
|------|--------|--------|
| grub.cfg filenames | Verify kernel/capDL image names match semperos-sel4 build output (should be identical: `kernel-x86_64-pc99`, `capdl-loader-image-x86_64-pc99`) | Small |
| make-iso.sh output name | Change `sel4-xcpng.iso` → `semperos-sel4.iso` | Small |
| .camkes assembly | Create `semperos-sel4-xcpng.camkes` — strip DTUBridge + HW components, keep VDTUService, SemperKernel, VPE0, VPE1 | Medium |
| settings.cmake | Create `settings-xcpng.cmake` — remove `LibLwip ON`, `NODE_ID`, `KernelDebugBuild ON`; optionally set RELEASE=ON | Medium |
| CMakeLists.txt | Conditionally exclude DTUBridge sources when DTUBridge is not in the assembly (or create a separate cmake for XCP-ng) | Medium |

### [NEW] — No reference exists

| Item | Description | Effort |
|------|-------------|--------|
| XCP-ng runbook | `docs/XCPNG-RUNBOOK.md` with deployment + collection instructions | Small |
| Bench output marker | Ensure `[BENCH]` lines go to serial (they already do via printf) | None (already works) |

### Total effort estimate: Small-Medium

The core work is creating a stripped `.camkes` assembly and a corresponding cmake preset. The ISO build infrastructure is a direct copy. No new CAmkES components or drivers needed.

---

## 7. Open Questions

### XCP-ng VM Configuration
1. **vCPU count**: Exp 2A on QEMU showed 15ms overhead due to single-core cooperative scheduling. How many vCPUs should the XCP-ng VM have? seL4 with `KernelMaxNumNodes=1` uses only 1 core, but additional vCPUs may affect Xen scheduling.
2. **RAM allocation**: 512 MiB is sufficient for the reference project. With 5 components + 4 MiB kernel heap + 2 MiB DMA pool (if DTUBridge kept), 1 GiB is safer.
3. **NIC type**: If DTUBridge is stripped (recommended), no NIC needed. If kept, must configure e1000 and determine the MMIO address assigned by Xen's virtual BIOS.

### Host Network Topology
4. **Log server location**: Where does the socat listener run? Options: (a) on the XCP-ng host itself, (b) on a separate VM on the same host, (c) on a remote machine. Option (a) is simplest.
5. **Firewall**: Port 9514 must be open between seL4 VM and log server.

### Serial Console
6. **xe console vs hvm_serial**: XCP-ng provides both `xe console uuid=$VM_UUID` (interactive) and `hvm_serial=tcp:` (programmatic). Both capture the same serial stream. For benchmark collection, `hvm_serial=tcp:` is preferred because it can be piped to a file.
7. **Serial baud rate**: grub.cfg sets 115200. seL4's serial driver default is 115200. These match, but Xen's HVM serial backend may buffer differently than physical UART.

### Benchmark Validity
8. **Clock source on XCP-ng**: semperos-sel4 uses `rdtsc` for cycle counting. On XCP-ng HVM, TSC may be virtualised (constant_tsc + nonstop_tsc flags). Need to verify TSC reliability under Xen. If TSC is unreliable, may need to use HPET or Xen's sched_clock.
9. **Xen scheduling interference**: Even with 1 vCPU pinned, Xen's credit scheduler may preempt the VM. For clean benchmarks, consider pinning the VM's vCPU to a dedicated physical core and setting the scheduler pool.
10. **KernelDebugBuild**: The QEMU builds use `KernelDebugBuild ON` which enables assertions. For benchmark runs on XCP-ng, should this be OFF to eliminate assertion overhead? Or keep ON for safety during initial bring-up?

### Build Environment
11. **Cross-compilation**: The ISO build should work in the same Docker container used for QEMU builds, since the target is the same (x86_64/pc99). Verify `grub-mkrescue` and `xorriso` are available in the container.
12. **Image naming**: Both projects produce `kernel-x86_64-pc99` and `capdl-loader-image-x86_64-pc99` in `images/`. The make-iso.sh script should work unchanged if pointed at the semperos-sel4 build directory.

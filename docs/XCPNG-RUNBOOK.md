# XCP-ng Deployment Runbook

Boot SemperOS-on-seL4 as an XCP-ng HVM appliance, collect Exp 2A bare-metal
benchmark output over TCP serial.

---

## Hardware Requirements

- **XCP-ng** host (version 8.2 or 8.3)
- **VM config**: 1 vCPU, 512 MiB RAM (minimum), BIOS boot (not UEFI)
- **No NIC required** for local benchmarks (Exp 2A)
- **Log server**: any machine reachable from XCP-ng host on TCP port 9514
  (can be the XCP-ng host itself, a separate VM, or a remote machine)

### Recommended VM settings for clean benchmarks

- Pin vCPU to a dedicated physical core
- Set Xen scheduler credit weight high (or use `xe vm-param-set` scheduler pinning)
- Disable VM watchdog timer if enabled

---

## Build

From the `camkes-vm-examples` directory:

```bash
rm -rf build-xcpng && mkdir build-xcpng && cd build-xcpng

# CMake with XCP-ng options (no DTUBridge, bench mode on)
cmake -G Ninja \
    -DPLATFORM=pc99 \
    -DSEMPEROS_NO_NETWORK=ON \
    -DSEMPER_BENCH_MODE=ON \
    -C ../projects/semperos-sel4/settings.cmake \
    ../projects/semperos-sel4

ninja
```

Verify build artifacts:
```bash
ls images/kernel-x86_64-pc99 images/capdl-loader-image-x86_64-pc99
```

### Create ISO

```bash
../projects/semperos-sel4/scripts/make-iso.sh .
# Output: build-xcpng/semperos-sel4.iso (~18 MiB)
```

Dependencies: `grub-common`, `grub-pc-bin`, `xorriso`
```bash
sudo apt install grub-common grub-pc-bin xorriso
```

---

## Host-Side Log Receiver

### Option A: Quick one-shot capture (recommended for benchmarks)

On the log server machine, before booting the VM:

```bash
nc -l 9514 | tee /tmp/sel4-bench.log
```

Or with `socat` for auto-reconnect:
```bash
socat TCP-LISTEN:9514,reuseaddr,fork - | tee /tmp/sel4-bench.log
```

### Option B: Persistent systemd service

Run the setup script from a Linux VM on the same network:

```bash
cd logserver
./setup-logserver.sh
# Installs socat, copies logger script, enables systemd service
# Logs: /var/log/sel4/serial.log
# Port: 9514
```

---

## Boot on XCP-ng

### 1. Upload ISO to XCP-ng host

```bash
scp build-xcpng/semperos-sel4.iso root@xcpng-host:/var/opt/xen/ISO_Store/
```

### 2. Create VM

```bash
ssh root@xcpng-host

# Create VM from template
xe vm-install template="Other install media" new-name-label="SemperOS-bench"
VM_UUID=$(xe vm-list name-label="SemperOS-bench" --minimal)

# Configure for BIOS boot
xe vm-param-set uuid=$VM_UUID HVM-boot-params:firmware=bios

# Set memory (512 MiB is sufficient)
xe vm-memory-limits-set uuid=$VM_UUID \
    static-min=512MiB dynamic-min=512MiB dynamic-max=512MiB static-max=512MiB

# Set vCPU count (1 is sufficient — seL4 uses KernelMaxNumNodes=1)
xe vm-param-set uuid=$VM_UUID VCPUs-max=1
xe vm-param-set uuid=$VM_UUID VCPUs-at-startup=1
```

### 3. Configure serial output to TCP

```bash
# Point HVM serial to the log server
LOG_SERVER_IP=<ip-of-log-receiver>
xe vm-param-set uuid=$VM_UUID platform:hvm_serial=tcp:${LOG_SERVER_IP}:9514
```

### 4. Attach ISO and boot

```bash
xe vm-cd-add uuid=$VM_UUID cd-name=semperos-sel4.iso device=0
xe vm-start uuid=$VM_UUID
```

### 5. View serial console (alternative to TCP)

```bash
xe console uuid=$VM_UUID
# Ctrl+] to exit
```

### 6. Stop VM after benchmarks complete

```bash
xe vm-shutdown uuid=$VM_UUID --force
```

### Reboot with updated ISO

```bash
# On build machine: rebuild + repack ISO
ninja && ../projects/semperos-sel4/scripts/make-iso.sh .

# Upload new ISO (same name overwrites)
scp build-xcpng/semperos-sel4.iso root@xcpng-host:/var/opt/xen/ISO_Store/

# Reboot VM
xe vm-shutdown uuid=$VM_UUID --force
xe vm-start uuid=$VM_UUID
```

---

## Collect Exp 2A Output

### What to look for

The benchmark output appears in the serial stream as:

```
[BENCH] local_exchange     min=...  med=...  mean=...  max=...  cycles  (...us median) [n=1000]
[BENCH-2A-LOCAL-UNVERIFIED] local_exchange: collected
[BENCH] local_revoke       min=...  med=...  mean=...  max=...  cycles  (...us median) [n=1000]
[BENCH-2A-LOCAL-UNVERIFIED] local_revoke: collected
[BENCH] chain_revoke_10    min=...  med=...  mean=...  max=...  cycles  (...us median) [n=200]
[BENCH-2A-LOCAL-UNVERIFIED] chain_revoke_10: collected
[BENCH] chain_revoke_50    min=...  med=...  mean=...  max=...  cycles  (...us median) [n=200]
[BENCH-2A-LOCAL-UNVERIFIED] chain_revoke_50: collected
[BENCH] chain_revoke_100   min=...  med=...  mean=...  max=...  cycles  (...us median) [n=200]
[BENCH-2A-LOCAL-UNVERIFIED] chain_revoke_100: collected
```

Wait for all 5 `[BENCH-2A-LOCAL-UNVERIFIED]` lines before stopping the VM.
Total wall-clock time estimate: 3-5 minutes from boot (depends on hardware).

### Extract results

```bash
grep '^\[BENCH\]' /tmp/sel4-bench.log > docs/EXP2A-XCPNG-RAW.txt
```

### Verify test suite passed

```bash
grep -E 'Test|PASS|FAIL|=== .* ===' /tmp/sel4-bench.log
# Expect: 11 passed, 0 failed
```

---

## QEMU Local Smoke Test

Before deploying to XCP-ng, verify the ISO boots locally:

```bash
# Direct multiboot (fastest, no GRUB)
timeout 60 qemu-system-x86_64 \
    -machine q35 \
    -cpu qemu64,+rdrand,+fsgsbase,+xsave,+xsaveopt \
    -m 512 \
    -nographic \
    -serial mon:stdio \
    -kernel build-xcpng/images/kernel-x86_64-pc99 \
    -initrd build-xcpng/images/capdl-loader-image-x86_64-pc99 \
    -nic none -no-reboot

# ISO boot (verifies GRUB + multiboot chain)
timeout 120 qemu-system-x86_64 \
    -machine q35 \
    -cpu qemu64,+rdrand,+fsgsbase,+xsave,+xsaveopt \
    -m 512 \
    -nographic \
    -serial mon:stdio \
    -cdrom build-xcpng/semperos-sel4.iso \
    -nic none -boot d -no-reboot
```

Note: QEMU requires `-machine q35` and `-serial mon:stdio` for serial output.
The GRUB menu has a 3-second timeout before auto-booting.

---

## Troubleshooting

### No serial output on XCP-ng

1. Verify `hvm_serial` is set: `xe vm-param-get uuid=$VM_UUID param-name=platform`
2. Verify log server is listening: `ss -tlnp | grep 9514`
3. Try `xe console` — if output appears there but not TCP, the serial redirect
   is misconfigured

### GRUB shows but seL4 doesn't boot

Check that the ISO was built with BIOS GRUB (not EFI). The `grub-mkrescue`
command should produce a BIOS-bootable ISO by default on x86_64 hosts.

### Benchmark numbers look wrong

- Verify `SEMPER_BENCH_MODE=ON` was set at build time
- Check that KernelDebugBuild is OFF for production benchmarks
  (assertions add overhead; rebuild with `-DRELEASE=ON`)
- Verify TSC is reliable: on XCP-ng, check `cat /proc/cpuinfo | grep tsc`
  in a Linux VM on the same host — look for `constant_tsc` and `nonstop_tsc`

### All 5 benchmarks show identical ~15ms

This is expected on single-core QEMU (scheduling overhead). On XCP-ng
bare metal, the numbers should be orders of magnitude lower due to
notification-driven wakeup instead of yield-polling.

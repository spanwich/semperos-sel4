---
name: qemu-smoke-test
description: Boot a semperos-sel4 ISO or multiboot image in QEMU to verify it boots without crashing. Quick sanity check after build.
argument-hint: [build-dir]
allowed-tools: Bash(*), Read, Glob
---

# QEMU Smoke Test

Boot a freshly-built semperos-sel4 image in QEMU and verify it reaches
the test harness output without crashing.

## Arguments

`$ARGUMENTS` is the build directory path. Defaults to `build-xcpng` relative
to the camkes-vm-examples root.

## Procedure

### 1. Locate the build

```
BUILD_DIR=${ARGUMENTS:-/home/iamfo470/phd/camkes-vm-examples/build-xcpng}
```

Check that the kernel and initrd images exist:
- `${BUILD_DIR}/images/kernel-x86_64-pc99`
- `${BUILD_DIR}/images/capdl-loader-image-x86_64-pc99`

If not found, check for an ISO at `${BUILD_DIR}/semperos-sel4.iso`.

### 2. Boot via multiboot (preferred — faster)

If kernel + initrd exist:

```bash
timeout 60 qemu-system-x86_64 \
    -machine q35 -cpu qemu64,+rdrand,+fsgsbase,+xsave,+xsaveopt \
    -m 512 -nographic -serial mon:stdio -nic none -no-reboot \
    -kernel ${BUILD_DIR}/images/kernel-x86_64-pc99 \
    -initrd ${BUILD_DIR}/images/capdl-loader-image-x86_64-pc99
```

### 3. Boot via ISO (fallback — tests GRUB chain)

If only the ISO exists:

```bash
timeout 120 qemu-system-x86_64 \
    -machine q35 -cpu qemu64,+rdrand,+fsgsbase,+xsave,+xsaveopt \
    -m 512 -nographic -serial mon:stdio -nic none -boot d -no-reboot \
    -cdrom ${BUILD_DIR}/semperos-sel4.iso
```

### 4. Verify output

Check the serial output for:
- `[TEST]` lines indicating the test harness started
- No kernel panics or assertion failures
- Clean shutdown or expected test completion

## Report

Print a summary:
- Boot method used (multiboot or ISO)
- Whether test harness output was seen
- Any errors or crashes detected
- Total boot time

## Important Notes

- Requires `-machine q35` and `-serial mon:stdio`
- The `-nic none` flag is critical — suppresses Q35's default e1000e
- Timeout of 60s (multiboot) or 120s (ISO) prevents hangs
- Run one command at a time (per CLAUDE.md shell rules)

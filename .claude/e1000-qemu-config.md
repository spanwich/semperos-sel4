# E1000 & QEMU Configuration — SemperOS-seL4

Load this file when keywords: "E1000", "QEMU", "MAC", "PCI", "MMIO", "IRQ", "DMA"

---

## QEMU Testing

**NEVER run QEMU directly** from `camkes-vm-examples/` — use Docker test infrastructure.

### Single-node

```bash
cd docker && docker compose run --rm node-a
```

### Dual-node (cross-node communication)

```bash
cd docker && docker compose up 2>&1 | tee qemu-dual.log
```

Both nodes use the same image from `build-node0/images/`. Node A listens on
`:10001`, Node B connects after 3s delay. QEMU socket backend provides virtual
L2 Ethernet. Key flag: `-nic none` suppresses Q35's default e1000e so our
explicit e1000 (82540EM) takes PCI slot 2.

### IP addressing (runtime, from MAC)

- MAC ...:01 -> IP 10.0.0.1 (Node 0), peer 10.0.0.2
- MAC ...:02 -> IP 10.0.0.2 (Node 1), peer 10.0.0.1

### E1000 hardware config (CAmkES)

- PCI Bus 0, Dev 2, Fun 0. Vendor 8086, Device 100E. BAR0 at 0xFEB80000
- MMIO: 128 KiB via `seL4HardwareMMIO` (uncached)
- DMA pool: 2 MiB at paddr 0x4000000 (`dma_pool` + `simple_untyped21_pool=4`)
- IRQ: IOAPIC pin 11 via `seL4HardwareInterrupt`
- PCI config: I/O ports 0xCF8-0xCFF via `seL4HardwareIOPort`

### ISO smoke test (XCP-ng build)

```bash
# Direct multiboot (fastest)
timeout 60 qemu-system-x86_64 \
    -machine q35 -cpu qemu64,+rdrand,+fsgsbase,+xsave,+xsaveopt \
    -m 512 -nographic -serial mon:stdio -nic none -no-reboot \
    -kernel build-xcpng/images/kernel-x86_64-pc99 \
    -initrd build-xcpng/images/capdl-loader-image-x86_64-pc99

# ISO boot (tests GRUB chain)
timeout 120 qemu-system-x86_64 \
    -machine q35 -cpu qemu64,+rdrand,+fsgsbase,+xsave,+xsaveopt \
    -m 512 -nographic -serial mon:stdio -nic none -boot d -no-reboot \
    -cdrom build-xcpng/semperos-sel4.iso
```

Note: requires `-machine q35` and `-serial mon:stdio`.

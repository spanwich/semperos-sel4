---
name: iso-builder
description: Build three SemperOS XCP-ng node ISOs with distinct KERNEL_ID and peer IPs, copy to dist/, generate SHA256SUMS, and optionally commit+push.
argument-hint: [commit-message]
allowed-tools: Bash(*), Read, Glob, Grep
---

# Build Three SemperOS Node ISOs

Build all three XCP-ng node images from the current branch. Each node gets a
distinct KERNEL_ID, SELF_IP, and peer IP table baked in at cmake time.

## Node Identity Matrix

| Node | KERNEL_ID | SELF_IP | PEER_IP_0 | PEER_IP_1 |
|------|-----------|---------|-----------|-----------|
| 0 | 0 | 192.168.100.10 | 192.168.100.11 | 192.168.100.12 |
| 1 | 1 | 192.168.100.11 | 192.168.100.10 | 192.168.100.12 |
| 2 | 2 | 192.168.100.12 | 192.168.100.10 | 192.168.100.11 |

## Build Procedure

For each node (0, 1, 2), run from the `camkes-vm-examples` root:

```bash
rm -rf build-node${N} && mkdir build-node${N} && cd build-node${N}
cmake -G Ninja \
    -DPLATFORM=pc99 \
    -DSEMPEROS_NO_NETWORK=OFF \
    -DSEMPER_BENCH_MODE=ON \
    -DKERNEL_ID=${KERNEL_ID} \
    -DSELF_IP=${SELF_IP} \
    -DPEER_IP_0=${PEER_IP_0} \
    -DPEER_IP_1=${PEER_IP_1} \
    -C ../projects/semperos-sel4/settings.cmake \
    ../projects/semperos-sel4
ninja
```

The camkes-vm-examples root is at `/home/iamfo470/phd/camkes-vm-examples`.
The project root is at `/home/iamfo470/phd/camkes-vm-examples/projects/semperos-sel4`.

Build all three nodes sequentially. Wait for each `ninja` to complete before
starting the next.

## Create ISOs

After all three builds succeed, create ISOs and copy to dist/:

```bash
bash scripts/make-iso.sh /path/to/build-node0
cp /path/to/build-node0/semperos-sel4.iso dist/semperos-node0.iso

bash scripts/make-iso.sh /path/to/build-node1
cp /path/to/build-node1/semperos-sel4.iso dist/semperos-node1.iso

bash scripts/make-iso.sh /path/to/build-node2
cp /path/to/build-node2/semperos-sel4.iso dist/semperos-node2.iso
```

`scripts/make-iso.sh` is at the project root. It requires `grub-mkrescue` and
`xorriso` to be installed.

## Generate SHA256 Checksums

```bash
sha256sum dist/semperos-node*.iso | tee dist/SHA256SUMS
```

## Report Results

Print a summary table:

| Image | KERNEL_ID | IP | SHA256 (first 16 chars) |
|-------|-----------|----|-------------------------|

## Commit and Push (if commit message provided)

If `$ARGUMENTS` is non-empty, use it as the commit message:

```bash
git add dist/semperos-node0.iso dist/semperos-node1.iso dist/semperos-node2.iso dist/SHA256SUMS
git commit -m "$ARGUMENTS

Co-Authored-By: Claude Opus 4.6 (1M context) <noreply@anthropic.com>"
git push
```

If `$ARGUMENTS` is empty, just build and report — do not commit or push.

## Important Notes

- Run one command at a time (per CLAUDE.md shell rules)
- Each `ninja` build takes ~60-120 seconds
- The ISO is ~19 MiB per node
- The build uses the full assembly (with DTUBridge/E1000) not the xcpng-stripped one
- SEMPER_BENCH_MODE=ON disables hot-path KLOG for clean benchmarks

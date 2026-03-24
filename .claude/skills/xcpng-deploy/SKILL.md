---
name: xcpng-deploy
description: Deploy SemperOS ISOs to XCP-ng cluster, reboot VMs, collect benchmark output from serial log ports, and save results to timestamped JSON.
argument-hint: [--dry-run | --collect-only | --build-only]
allowed-tools: Bash(*), Read, Write, Glob, Grep
---

# XCP-ng Deployment and Benchmark Collection

Full pipeline: build ISOs, deploy to XCP-ng, collect Exp 2A benchmark results.

## Flags

Parse `$ARGUMENTS` for these flags:
- `--dry-run` — print all commands but do not execute any SSH/xe operations
- `--collect-only` — skip build and deploy, just collect from log ports
- `--build-only` — build ISOs only, do not deploy or collect

If no flag is given, run the full pipeline (build + deploy + collect).

---

## Cluster Configuration

| Parameter | Value |
|-----------|-------|
| XCP-ng host | `root@192.168.40.100` (key-based SSH, no `-i` needed) |
| VM name node0 | `seL4_semperos_node0` |
| VM name node1 | `seL4_semperos_node1` |
| VM name node2 | `seL4_semperos_node2` |
| Serial log host | `192.168.40.106` |
| Log port node0 | `9501` on `192.168.40.106` |
| Log port node1 | `9502` on `192.168.40.106` |
| Log port node2 | `9503` on `192.168.40.106` |
| Loki endpoint | `http://192.168.40.107:3100` |
| Loki VM labels | `sel4-node1` (verify node0/node2 labels at runtime) |
| Completion marker | `[VPE0] === Experiment 2A complete ===` |
| ISO SR name | `ISO-Store` (path: `/var/opt/xen/ISO_Store/`) |

---

## Phase 1: Build ISOs (skip if --collect-only)

Use the `/build-isos` skill or follow the same procedure:

Build all three nodes from `camkes-vm-examples` root (`/home/iamfo470/phd/camkes-vm-examples`).
The project root is `/home/iamfo470/phd/camkes-vm-examples/projects/semperos-sel4`.

For each node N in {0, 1, 2}:

```bash
cd /home/iamfo470/phd/camkes-vm-examples
rm -rf build-node${N}
mkdir build-node${N}
cd build-node${N}
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

Node identity matrix:

| Node | KERNEL_ID | SELF_IP | PEER_IP_0 | PEER_IP_1 |
|------|-----------|---------|-----------|-----------|
| 0 | 0 | 192.168.100.10 | 192.168.100.11 | 192.168.100.12 |
| 1 | 1 | 192.168.100.11 | 192.168.100.10 | 192.168.100.12 |
| 2 | 2 | 192.168.100.12 | 192.168.100.10 | 192.168.100.11 |

Then create ISOs and copy to dist/:

```bash
cd /home/iamfo470/phd/camkes-vm-examples/projects/semperos-sel4
bash scripts/make-iso.sh /home/iamfo470/phd/camkes-vm-examples/build-node${N}
cp /home/iamfo470/phd/camkes-vm-examples/build-node${N}/semperos-sel4.iso dist/semperos-node${N}.iso
```

Generate checksums:

```bash
sha256sum dist/semperos-node*.iso | tee dist/SHA256SUMS
```

---

## Phase 2: Deploy ISOs to XCP-ng (skip if --build-only or --collect-only)

All `xe` commands run via SSH to `root@192.168.40.100`. Run one command at a time.

### Step 2.1: Discover VM UUIDs at runtime

```bash
ssh root@192.168.40.100 "xe vm-list name-label=seL4_semperos_node0 --minimal"
ssh root@192.168.40.100 "xe vm-list name-label=seL4_semperos_node1 --minimal"
ssh root@192.168.40.100 "xe vm-list name-label=seL4_semperos_node2 --minimal"
```

Store each UUID in a variable. If any returns empty, report the error and stop.

### Step 2.2: Discover ISO SR UUID

```bash
ssh root@192.168.40.100 "xe sr-list name-label=ISO-Store --minimal"
```

Note: the SR name is `ISO-Store` (with hyphen), not `ISO_Store`.

### Step 2.3: Find CD VBDs for each VM

Each VM may have multiple CD VBDs. Find the non-empty one:

```bash
ssh root@192.168.40.100 "xe vbd-list vm-uuid=${VM_UUID} type=CD empty=false params=uuid --minimal"
```

If empty (no CD attached), find any CD VBD:

```bash
ssh root@192.168.40.100 "xe vbd-list vm-uuid=${VM_UUID} type=CD params=uuid --minimal"
```

Use `vbd-eject` and `vbd-insert` (not `vm-cd-eject`/`vm-cd-add`) because VMs
may have multiple CD drives.

### Step 2.4: Eject old ISO (hot, while VM is running)

```bash
ssh root@192.168.40.100 "xe vbd-eject uuid=${VBD_UUID}"
```

Ignore errors if already empty.

### Step 2.5: Delete old ISO from ISO SR storage

Remove the previous ISO file from the host filesystem to avoid duplicate VDIs:

```bash
ssh root@192.168.40.100 "rm -f /var/opt/xen/ISO_Store/semperos-node${N}.iso"
```

### Step 2.6: Upload new ISOs

```bash
scp dist/semperos-node0.iso root@192.168.40.100:/var/opt/xen/ISO_Store/semperos-node0.iso
scp dist/semperos-node1.iso root@192.168.40.100:/var/opt/xen/ISO_Store/semperos-node1.iso
scp dist/semperos-node2.iso root@192.168.40.100:/var/opt/xen/ISO_Store/semperos-node2.iso
```

### Step 2.7: Rescan ISO SR and find new VDI UUID

```bash
ssh root@192.168.40.100 "xe sr-scan uuid=${SR_UUID}"
ssh root@192.168.40.100 "xe vdi-list sr-uuid=${SR_UUID} name-label=semperos-node${N}.iso params=uuid --minimal"
```

### Step 2.8: Insert new ISO (hot, while VM is running)

```bash
ssh root@192.168.40.100 "xe vbd-insert uuid=${VBD_UUID} vdi-uuid=${VDI_UUID}"
```

### Step 2.9: Restart VMs

seL4 has no ACPI/PV drivers, so `xe vm-reboot` does not work. Use force
shutdown + start:

```bash
ssh root@192.168.40.100 "xe vm-shutdown uuid=${VM_UUID} --force"
ssh root@192.168.40.100 "xe vm-start uuid=${VM_UUID}"
```

Do this for all three VMs. The serial output starts flowing on reboot —
Loki captures it automatically via the `hvm_serial=tcp:` redirect.

---

## Phase 3: Collect Benchmark Output (skip if --build-only)

### Collection Method: Loki API (primary)

Loki at `http://192.168.40.107:3100` captures all serial output via the
`hvm_serial=tcp:` redirect. Query Loki after the VMs have finished booting
and running benchmarks (~3-5 minutes on bare metal).

### Step 3.1: Wait for completion

Poll Loki for the completion marker on each node. The Loki `vm` label may
differ from the XCP-ng VM name — discover labels first:

```bash
curl -s 'http://192.168.40.107:3100/loki/api/v1/label/vm/values' | python3 -c "import sys,json; print(json.load(sys.stdin)['data'])"
```

Then poll each node until the marker appears (or 600s timeout):

```bash
curl -s -G 'http://192.168.40.107:3100/loki/api/v1/query_range' \
    --data-urlencode 'query={vm="${VM_LABEL}"} |= "Experiment 2A complete"' \
    --data-urlencode 'limit=1' \
    --data-urlencode 'since=10m'
```

### Step 3.2: Verify test results via Loki

```bash
curl -s -G 'http://192.168.40.107:3100/loki/api/v1/query_range' \
    --data-urlencode 'query={vm="${VM_LABEL}"} |= "passed"' \
    --data-urlencode 'limit=5' \
    --data-urlencode 'since=10m'
```

Expect: `11 passed, 0 failed`. If any node fails tests, report and stop.

### Fallback: Direct serial capture (nc)

If Loki is unavailable, connect directly to the serial log ports:

```bash
timeout 600 nc 192.168.40.106 ${PORT} | tee /tmp/node${N}_raw.log
```

Ports: node0=9501, node1=9502, node2=9503 (on 192.168.40.106).
Note: nc must connect BEFORE `vm-start` to capture the full boot.

### Step 3.3: Extract benchmark lines from Loki

```bash
curl -s -G 'http://192.168.40.107:3100/loki/api/v1/query_range' \
    --data-urlencode 'query={vm="${VM_LABEL}"} |= "[BENCH]"' \
    --data-urlencode 'limit=50' \
    --data-urlencode 'since=10m' | \
  python3 -c "import sys,json; d=json.load(sys.stdin); [print(v[1]) for r in d['data']['result'] for v in r['values']]" \
  > /tmp/node${N}_bench.txt
```

### Step 3.4: Parse into JSON

Create a results directory if it does not exist:

```bash
mkdir -p results
```

Build a JSON file at `results/exp2a_YYYYMMDD_HHMMSS.json` with this structure:

```json
{
  "timestamp": "2026-03-23T12:34:56",
  "branch": "<current git branch>",
  "commit": "<current git short hash>",
  "tsc_mhz": {
    "node0": <parsed from TSC frequency line>,
    "node1": <parsed>,
    "node2": <parsed>
  },
  "benchmarks": {
    "node0": {
      "local_exchange_kernel": {"min": N, "med": N, "mean": N, "max": N, "n": N},
      "local_revoke_kernel": {"min": N, "med": N, "mean": N, "max": N, "n": N},
      "chain_revoke_10_kernel": {"min": N, "med": N, "mean": N, "max": N, "n": N},
      "chain_revoke_25_kernel": {"min": N, "med": N, "mean": N, "max": N, "n": N},
      "chain_revoke_50_kernel": {"min": N, "med": N, "mean": N, "max": N, "n": N},
      "chain_revoke_100_kernel": {"min": N, "med": N, "mean": N, "max": N, "n": N}
    },
    "node1": { ... },
    "node2": { ... }
  }
}
```

Parse each `[BENCH]` line using the format:
```
[BENCH] <name>  min=<N>  med=<N>  mean=<N>  max=<N>  cycles  (<F>us median) [n=<N>]
```

Use `python3` or `jq` for JSON construction — whichever is available.

### Step 3.5: Report summary

Print a table to the console:

```
Benchmark                node0 med    node1 med    node2 med    unit
local_exchange_kernel    XXXX         XXXX         XXXX         cycles
local_revoke_kernel      XXXX         XXXX         XXXX         cycles
chain_revoke_10_kernel   XXXX         XXXX         XXXX         cycles
chain_revoke_25_kernel   XXXX         XXXX         XXXX         cycles
chain_revoke_50_kernel   XXXX         XXXX         XXXX         cycles
chain_revoke_100_kernel  XXXX         XXXX         XXXX         cycles
```

Report the path to the saved JSON file.

---

## Important Notes

- Run one command at a time (per CLAUDE.md shell rules)
- Never hardcode VM UUIDs or ISO SR paths — always discover via `xe` commands
- The XCP-ng host SSH is key-based: `ssh root@192.168.40.100` with no `-i` flag
- VMs use `hvm_serial=tcp:` for serial output — the log ports are already configured
- If a VM fails to boot or the completion marker is not seen within 600s, report
  the last 20 lines of the log and stop — do not proceed to JSON parsing
- The `results/` directory is at the project root (semperos-sel4/)

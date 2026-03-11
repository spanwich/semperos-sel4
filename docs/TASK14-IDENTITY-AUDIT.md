# Task 14: Identity Configuration Audit

## (a) KERNEL_ID — hardcoded

`Platform.cc:45` sets `kernelId = 0` in the default `KEnv` constructor.
No cmake variable influences this. All nodes boot as kernel 0.

The `Platform::kernelId()` return value is used in:
- `PEManager.cc`: creator checks (`kernelId() == creatorKernelId()`)
- `DTU.cc`: sender_vpe field in ring sends (`Platform::kernelId()`)
- `VPE.cc`: capability creation
- `kernel.cc:168`: boot log message

## (b) PEER_IP — hardcoded (partially runtime, partially hardcoded)

DTUBridge derives identity from MAC address at runtime (`post_init`, line 639):
```c
my_ip_last = g_drv.mac_addr[5];   // 1 or 2
my_node_id = my_ip_last - 1;      // 0 or 1
peer_ip_last = (my_ip_last == 1) ? 2 : 1;
```

The IP prefix is hardcoded at file scope:
```c
#define IP_PREFIX_A 10
#define IP_PREFIX_B 0
#define IP_PREFIX_C 0
```

So the current scheme is `10.0.0.{1,2}` with exactly 2 nodes.

The `net_net_send()` RPC handler (line 556) also hardcodes the two-node
assumption:
```c
IP4_ADDR(ip_2_ip4(&dest_ip), IP_PREFIX_A, IP_PREFIX_B, IP_PREFIX_C,
         (dest_node == 0) ? 1 : 2);
```

## (c) DTUBridge — single peer only

Current code supports exactly one peer. The `peer_ip_last` variable is scalar.
The ring-buffer outbound path (line 745) sends to a single `peer_ip_last`.
The RPC path (`net_net_send`, line 556) maps dest_node to IP with a two-node
hardcoded table.

There is no peer table or array. Extension to 2 peers requires:
1. Replace `peer_ip_last` with a 2-element array
2. Update `net_net_send()` to index into the array by `dest_node`
3. Update the ring-buffer outbound path to route to the correct peer
   (currently always sends to the single peer — needs dest_node in header)

## (d) Exact code locations

| What | File | Line | Current value |
|------|------|------|---------------|
| kernelId | Platform.cc | 45 | `kernelId = 0` |
| IP prefix | DTUBridge.c | 51-53 | `10.0.0.x` |
| my_ip_last | DTUBridge.c | 57 | derived from MAC[5] |
| peer_ip_last | DTUBridge.c | 58 | `(my_ip_last == 1) ? 2 : 1` |
| RPC dest map | DTUBridge.c | 556-557 | `(dest_node == 0) ? 1 : 2` |
| NODE_ID cmake | CMakeLists.txt | 206 | passed to DTUBridge C_FLAGS |
| NODE_ID default | settings.cmake | 100 | `0` |

## Plan

1. Add `KERNEL_ID` cmake variable, pass to SemperKernel as `-DSEMPER_KERNEL_ID=N`
2. Use `SEMPER_KERNEL_ID` in Platform.cc instead of hardcoded 0
3. Replace MAC-derived IP scheme with compile-time `DTUB_MY_IP` and
   `DTUB_PEER_IP_0`/`DTUB_PEER_IP_1` string defines
4. Add cmake variables for these with XCP-ng defaults (192.168.100.x)
5. Extend DTUBridge peer table to 2 entries

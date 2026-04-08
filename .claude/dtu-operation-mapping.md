# DTU Operation Mapping — SemperOS-seL4

Load this file when keywords: "DTU operation", "SEND", "FETCH_MSG", "ACK_MSG", "WAKEUP_CORE"

---

| DTU Operation | seL4/CAmkES Equivalent |
|---------------|------------------------|
| SEND | `vdtu_ring_send()` to shared dataport |
| FETCH_MSG | `vdtu_ring_fetch()` from shared dataport |
| ACK_MSG | `vdtu_ring_ack()` (advance tail) |
| READ/WRITE (memory EP) | Direct `memcpy()` on shared dataport |
| wait()/HLT | `seL4_Yield()` (cooperative scheduling on single-core QEMU) |
| WAKEUP_CORE | `seL4_Signal()` on notification |
| Endpoint config | RPC to vDTU -> endpoint table update + channel assignment |

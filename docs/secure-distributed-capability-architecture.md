# Secure Distributed Capability Architecture for SD-WAN

**Project:** SemperOS-seL4 — Distributed Capability System on Verified Microkernel
**Status:** Architecture Draft v3 — Three-plane SDN model, March 26 2026
**Supersedes:** v2 (consolidated), v1 (initial)

---

## 1. What This System Is

A secure SD-WAN edge device that combines a distributed capability kernel
(SemperOS), a verified microkernel (seL4), an L2 managed switch (sel4_xcpng),
and cryptographic transport (HACL*) into a single self-contained appliance.

The core thesis: **capabilities ARE the management plane of an SDN.** Traditional
SD-WAN uses ACL tables, firewall rules, and routing policies as the management
plane. We use capabilities, capability groups, and Raft consensus. The control
and data planes are conventional (VLAN switching, packet crypto, L2 forwarding).
The innovation is entirely in HOW the management plane expresses and enforces
policy.

The system addresses security gaps identified in both SemperOS (Hille et al.
2019) and FractOS (Vilanova et al. 2022): neither system authenticates inter-node
messages because both assume a trusted physical fabric. Our system is the first
to run SemperOS's capability protocols over an untrusted network with explicit
authentication.

### 1.1 Thesis Contributions

| # | Contribution | Status |
|---|---|---|
| 1 | SemperOS on real hardware — CAmkES component port with verified vDTU | Substantially complete. Local ops paper-ready |
| 2 | Verified vDTU — EverParse/F*/Z3 capability validity checking | Planned |
| 3 | Two-tier Raft revocation — consensus-backed revocation replacing ThreadManager | Planned. Prerequisite for Contribution 4 |
| 4 | Secure distributed capability system for SD-WAN — authenticated transport, Raft-based admission, direct exchange protocol | Planned. Depends on Contribution 3 |

### 1.2 Design Principles

1. The device IS the switch — no external switch trust required
2. All inter-node communication is authenticated and encrypted inside the
   seL4 trust boundary
3. Node admission requires consensus (Raft quorum vote) — no single node
   can unilaterally admit or evict
4. The transport is untrusted — any IP transport (MPLS, broadband, 4G LTE,
   satellite) works
5. Local capability operations are unaffected by network security — zero
   overhead for intra-node operations
6. Session protocol replaced by direct exchange — simpler, more secure,
   sufficient for SDN use case
7. Clear three-plane separation — management plane owns policy,
   control plane makes decisions, data plane moves packets

---

## 2. Three-Plane Architecture

### 2.1 Plane Overview

```
+====================================================================+
|                     MANAGEMENT PLANE                                |
|                     (policy authority)                              |
|                                                                    |
|  RaftPD                         step-ca (external)                 |
|    Log replication                Certificate issuance             |
|    MHT state machine              CA keys, device certs            |
|    Membership + blocklist                                          |
|    Peer key table               Operator interface                 |
|    Capability groups              Whitelist, eviction              |
|    Cap BLOCK entries              Policy input                     |
|    CA root certificate                                             |
|                                                                    |
|  OWNS: MHT, membership, blocklist, whitelist, peer keys,          |
|        capability groups, BLOCK set, CA root                       |
+====================================================================+
         |                    |                    |
         | Raft commit        | Raft commit        | Raft commit
         | updates            | updates            | updates
         v                    v                    v
+====================================================================+
|                      CONTROL PLANE                                  |
|                      (per-event decisions)                          |
|                                                                    |
|  SyscallHandler               AdmissionAgent                       |
|    Per-exchange routing         Per-admission decision              |
|    CONSUMES: MHT, membership    CONSUMES: whitelist, blocklist,    |
|                                 CA root                            |
|                                                                    |
|  SemperKernel (CapTable)      VDTUService                          |
|    Per-syscall cap operations   Per-endpoint validation             |
|    CONSUMES: MHT                CONSUMES: BLOCK set                |
|                                                                    |
|  EAPAuthenticator (optional)  SwitchFabric                         |
|    Per-port auth decision       Per-frame forwarding decision       |
|    CONSUMES: RADIUS config      CONSUMES: VLAN policy from Raft    |
|    OWNS: port auth state        OWNS: forwarding table (MAC learn) |
|                                                                    |
|  CryptoTransport (key mgmt)                                       |
|    Per-epoch key rotation                                          |
|    CONSUMES: peer Ed25519 keys from Raft                           |
|    OWNS: ephemeral keys, session keys                              |
+====================================================================+
         |                    |                    |
         | configure          | set VLAN           | set keys
         | port auth          | filter             |
         v                    v                    v
+====================================================================+
|                       DATA PLANE                                    |
|                       (packet execution)                            |
|                                                                    |
|  CryptoTransport (encrypt/decrypt)                                 |
|    Per-packet ChaCha20-Poly1305 AEAD                               |
|    CONSUMES: session keys from control plane                       |
|    OWNS: sequence counters (anti-replay)                           |
|                                                                    |
|  E1000Port(s)                                                      |
|    Frame TX/RX, DMA ring buffers                                   |
|    802.1Q VLAN tag insert/strip (hardware offload)                 |
|    Port authorization blocking (drop non-EAPOL when unauthorized)  |
|    CONSUMES: VFTA filter, port_authorized flag from control plane  |
|                                                                    |
|  DTUBridge (lwIP)                                                  |
|    UDP encapsulation/decapsulation                                 |
|                                                                    |
|  RADIUSClient                                                      |
|    UDP transport for RADIUS protocol                               |
|    CONSUMES: server address, shared secret                         |
+====================================================================+
```

### 2.2 Component Classification

| Component | Primary Plane | Role | Owns | Consumes from |
|---|---|---|---|---|
| Raft log + state machine | Management | Policy authority | MHT, membership, blocklist, whitelist, peer keys, capability groups, BLOCK set, CA root | Operator intent |
| step-ca (external Docker) | Management | Certificate issuance | CA keys, device certificates | Operator intent |
| SyscallHandler | Control | Per-exchange routing + authorization | — | MHT (routing), membership (access control) |
| SemperKernel CapTable | Control | Per-syscall capability operations | CapTable entries | MHT (from Raft) |
| AdmissionAgent | Control | Per-admission decision | — | Whitelist, blocklist, CA root (from Raft) |
| VDTUService | Control | Per-endpoint validation | Endpoint state table | BLOCK set (from Raft) |
| EAPAuthenticator | Control | Per-port authentication decision | Port authorization state | RADIUS config, EAP credentials |
| SwitchFabric | Control | Per-frame forwarding + VLAN assignment | Forwarding table (MAC learning) | VLAN policy (from Raft via RPC) |
| CryptoTransport (key mgmt) | Control | Per-epoch key rotation | Ephemeral keys, session keys | Peer Ed25519 keys (from Raft) |
| CryptoTransport (crypto) | Data | Per-packet encrypt/decrypt | Sequence counters | Session keys (from control) |
| E1000Port | Data | Frame TX/RX, VLAN tag, port blocking | DMA ring buffers | VFTA filter, port_authorized flag |
| DTUBridge (lwIP) | Data | UDP encap/decap | — | — |
| RADIUSClient | Data | UDP transport for RADIUS | — | Server address, shared secret |
| VPE0, VPE1, ... | Application | User workload | — | Capabilities from SemperKernel |

### 2.3 Inter-Plane Interfaces

```
Management Plane -> Control Plane:
  Raft commit -> SwitchFabric RPC    (update VLAN table)
  Raft commit -> CryptoTransport     (add/remove peer key)
  Raft commit -> SemperKernel        (update MHT, revoke capabilities)
  Raft commit -> VDTUService         (update BLOCK set)
  Raft commit -> AdmissionAgent      (update whitelist, blocklist)

Control Plane -> Data Plane:
  SwitchFabric -> E1000Port          (set VLAN tag, enable/disable port)
  EAPAuthenticator -> E1000Port      (authorize/deauthorize port)
  CryptoTransport -> DTUBridge       (encrypt/decrypt, accept/reject packet)
  SyscallHandler -> DTUBridge        (route to correct peer via MHT lookup)

Data Plane -> Control Plane:
  E1000Port -> EAPAuthenticator      (EAPOL frames on eapol_rx dataport)
  CryptoTransport -> AdmissionAgent  (initial handshake from unknown peer)
  DTUBridge -> SemperKernel          (decrypted inter-kernel messages on net_ring)
```

### 2.4 What Replaces What

| Original SemperOS (gem5) | This design | Plane |
|---|---|---|
| Static MHT at boot | Raft-replicated MHT | Management |
| No node admission/revocation | Raft AddServer/RemoveServer + blocklist | Management |
| Session protocol (createsrv/createsess) | Direct exchange + Raft capability groups | Management + Control |
| DTU hardware endpoint isolation | seL4 capability-based isolation | Control |
| NoC physical source identity | CryptoTransport Poly1305 MAC | Data |
| NoC physical containment | CryptoTransport encryption + VLAN | Data |
| External managed switch | Integrated SwitchFabric | Control + Data |
| Plaintext inter-kernel DTU | Authenticated-encrypted UDP (HACL*) | Data |

---

## 3. Data Flow Analysis

### 3.1 Operation: New Node Admission

```
ACTOR          PLANE        ACTION                                    STATE CHANGE
-----          -----        ------                                    ------------
Operator       Management   Provisions device with certificate,       Whitelist: +serial
                            adds serial to whitelist (Raft entry)

Device         Data         Boots, E1000Port sends UDP to known       Network: packets flowing
                            Raft member address

CryptoTransport Data        Receives raw UDP from unknown peer,       Temporary session key
                            performs Curve25519 handshake              established

AdmissionAgent Control      Receives join request over temporary      Decision: accept/reject
                            channel. Verifies certificate:
                            - Chain valid? (CONSUMES: CA root)
                            - Expired? (CONSUMES: NTP clock)
                            - Blocklisted? (CONSUMES: blocklist)
                            - Whitelisted? (CONSUMES: whitelist)
                            - Org name match?
                            - Private key possession? (Curve25519)

AdmissionAgent Control      All checks pass. Proposes AddServer       Proposal in Raft log
                            to Raft group

RaftPD         Management   Quorum members independently verify.      COMMITTED:
                            Majority agrees. Commit applied:          MHT: +K2 partition
                            - MHT: add K2's PE partition              Membership: +K2
                            - Membership: add K2                      Peer keys: +K2
                            - Peer key table: add K2's keys           VLAN: port 999->100

Raft->Control  Mgmt->Ctrl   Push state to control plane:
                            - SwitchFabric RPC: move port 999->100
                            - CryptoTransport: add K2 peer key
                            - SemperKernel: MHT update

SwitchFabric   Control      Receives VLAN update. Decides:            Port VLAN assignment
                            K2's port moves to VLAN 100               updated

SwitchFabric   Ctrl->Data   Pushes to E1000Port: set VFTA,           E1000 VLAN filter
               ->E1000Port  update port VLAN tag                      updated

CryptoTransport Control     Receives peer key. Derives session key.   Session key active
                            K2 is now a valid peer.

CryptoTransport Data        Starts accepting/encrypting packets       Packets flowing
                            for K2                                    encrypted
```

### 3.2 Operation: Spanning Exchange (Direct Protocol)

```
ACTOR          PLANE        ACTION                                    STATE CHANGE
-----          -----        ------                                    ------------
VPE0           Application  Calls exchange(target_ddl_key,            Syscall issued
                            cap_selector) — opcode 9

SyscallHandler Control      Receives syscall. Extracts target DDL     Routing decision
                            key. CONSUMES MHT (from Raft).
                            Resolves: target belongs to K1.
                            CONSUMES membership: is K0 admitted? Yes.

SyscallHandler Control      Constructs OBTAIN message with:           Message prepared
                            initiator DDL key, target DDL key,
                            capability type, group_id

CryptoTransport Data        Encrypts OBTAIN with K1's session key.    Ciphertext produced
                            Adds sequence number, nonce.
                            Computes Poly1305 MAC.

DTUBridge      Data         Encapsulates in UDP. Passes to lwIP.      UDP packet ready

SwitchFabric   Control      Selects uplink E1000Port based on         Path selected
                            path policy (cost, latency, availability)

E1000Port      Data         Inserts VLAN tag (hw offload).            Frame on wire
                            Transmits frame.

               --- network transit ---

E1000Port(K1)  Data         Receives frame. Strips VLAN tag.          Frame received

CryptoTransport Data        Looks up sender_id in peer table.         Peer found
(K1)                        Decrypts with K0's session key.
                            Verifies Poly1305 MAC. Checks sequence
                            number (anti-replay).

SyscallHandler Control      Receives decrypted OBTAIN request.        Authorization check
(K1)                        CONSUMES membership: is K0 admitted? Yes.
                            Locates target capability in CapTable.

SemperKernel   Control      Creates child capability. Adds to         CapTable updated
(K1)                        parent's children list. Sets parent
                            DDL key.

CryptoTransport Data        Encrypts reply. Signs. Transmits.         Reply on wire
(K1)

               --- network transit ---

CryptoTransport Data        Decrypts reply. Verifies MAC.             Reply verified
(K0)

SyscallHandler Control      Creates local capability for VPE0.        CapTable updated
(K0)                        Sets parent DDL key pointing to K1.

RaftPD         Management   Logs CAP_EXCHANGE entry with group_id.    Audit trail updated
                            Exchange is now tracked in capability
                            group.

SyscallHandler Control      Replies NO_ERROR to VPE0.                 Syscall complete
(K0)

VPE0           Application  Receives NO_ERROR. Cross-kernel           Exchange done
                            parent-child link exists.
```

### 3.3 Operation: Node Revocation

```
ACTOR          PLANE        ACTION                                    STATE CHANGE
-----          -----        ------                                    ------------
RaftPD         Management   Detects K2 heartbeat failure (~300ms      Failure detected
                            LAN / ~2s WAN). OR: operator proposes
                            explicit eviction.

RaftPD         Management   Proposes RemoveServer(K2).                Proposal in log

RaftPD         Management   Quorum commits (without K2's vote).       COMMITTED:
                            Applied atomically:                       MHT: -K2 partition
                            - MHT: remove K2's partition              Membership: -K2
                            - Membership: remove K2                   Blocklist: +K2 fp
                            - Blocklist: add K2's cert fingerprint    Peer keys: -K2
                            - Peer key table: remove K2

Raft->Control  Mgmt->Ctrl   Push state to control plane:
                            - SemperKernel: revoke_by_pe_range()
                            - SwitchFabric: move K2 port to 999
                            - CryptoTransport: delete K2 peer key

SemperKernel   Control      Walks all CapTables. Revokes every        Capabilities dead
                            capability whose DDL key falls in K2's
                            PE partition. Writes CAP_BLOCK entries
                            to Raft log for each revoked cap.

SwitchFabric   Control      Moves K2's port back to VLAN 999.         Port quarantined

CryptoTransport Control     Deletes K2 from peer key table.           K2 unknown peer

CryptoTransport Data        Any subsequent packet from K2: lookup     K2 packets dropped
                            fails (no peer entry), packet dropped.

E1000Port      Data         VLAN filter updated. K2 tagged frames     K2 frames filtered
                            rejected at hardware level.

               --- K2 attempts re-admission ---

AdmissionAgent Control      K2 presents certificate. Verifies chain:  REJECTED
                            OK. Checks blocklist: K2's fingerprint
                            FOUND. Rejects AddServer proposal.
                            K2 cannot re-admit with same cert.
```

### 3.4 Operation: Hourly Key Rotation

```
ACTOR          PLANE        ACTION                                    STATE CHANGE
-----          -----        ------                                    ------------
NTP            External     Epoch boundary approaching (T-30s)        Clock: new epoch

CryptoTransport Control     Generates new ephemeral Curve25519        New ephemeral key
                            key pair. Signs ephemeral public key      pair generated
                            with Ed25519 long-term key.
                            CONSUMES: own Ed25519 private key.

CryptoTransport Data        Sends signed ephemeral to all peers       Ephemerals exchanged
                            via existing encrypted channel.

CryptoTransport Data        Receives all peers' signed ephemerals.    Peers' ephemerals
                                                                      received

CryptoTransport Control     Verifies each signature against           Signatures verified
                            Raft-known Ed25519 public keys.
                            CONSUMES: peer key table from Raft.
                            Missing peer: log warning, keep old key.

CryptoTransport Control     Derives new session keys for all peers:   New session keys
                            HKDF(Curve25519(my_eph, peer_eph),        active
                            "semperos-ik-v1" || ids || epoch)

CryptoTransport Data        T-0: transition. Accept both old and      Dual-key window
                            new keys for 30 seconds.                  open

CryptoTransport Data        T+30s: zeroize old session keys and       Old keys destroyed
                            old ephemeral private key                 Forward secrecy
                            (Lib_Memzero0). Only new key active.      guaranteed
```

### 3.5 Operation: 802.1X LAN Port Authentication

```
ACTOR          PLANE        ACTION                                    STATE CHANGE
-----          -----        ------                                    ------------
LAN device     External     Plugs into edge device LAN port           Link up

E1000Port      Data         Receives frame. Checks port_authorized    Frame filtered
                            flag = 0 (unauthorized). Checks
                            EtherType: if 0x888E (EAPOL), route
                            to eapol_rx dataport. Otherwise DROP.

EAPAuthenticator Control    Receives EAPOL frame. Starts EAP          EAP state: IDENTITY
                            exchange: sends EAP-Request/Identity.

LAN device     External     Responds with EAP-Response/Identity.      Identity received

EAPAuthenticator Control    Forwards to RADIUSClient.                 RADIUS exchange

RADIUSClient   Data         Sends Access-Request to RADIUS server     UDP to RADIUS
                            (UDP port 1812). Receives response.

EAPAuthenticator Control    RADIUS Access-Accept received.            Decision: AUTHORIZE
                            Determines VLAN assignment from
                            RADIUS attributes.

EAPAuthenticator Control    Tells SwitchFabric: assign port to        Port VLAN set
                            VLAN. Tells E1000Port: set
                            port_authorized = 1.

E1000Port      Data         port_authorized = 1. Frames now           Frames forwarding
                            forwarded normally through SwitchFabric.

SwitchFabric   Control      VLAN-aware forwarding active for this     L2 forwarding
                            port. Unicast, broadcast, flooding        active
                            constrained to assigned VLAN.
```

### 3.6 Operation: Capability Group Revocation

```
ACTOR          PLANE        ACTION                                    STATE CHANGE
-----          -----        ------                                    ------------
Operator       Management   Tenant deprovisioned. Proposes            Proposal in log
                            CAP_GROUP_REVOKE for group
                            "tenant-X-resources"

RaftPD         Management   Quorum commits. Retrieves all             Group resolved
                            (parent_ddl, child_ddl) pairs recorded
                            under group "tenant-X-resources" from
                            capability group table.

Raft->Control  Mgmt->Ctrl   For each capability in the group:
                            SemperKernel.revoke_cap(child_ddl)

SemperKernel   Control      For each child_ddl: walks local           CapTables cleaned
                            CapTable, finds matching capability,
                            calls revoke_rec(). Writes CAP_BLOCK
                            for each revoked capability.

SemperKernel   Control      If child is on a remote kernel:           Inter-kernel revoke
                            sends revoke via CryptoTransport.
                            Remote kernel revokes and replies.

CryptoTransport Data        Encrypts/decrypts revoke messages         Packets on wire
                            between kernels.

RaftPD         Management   Group "tenant-X-resources" removed        Group cleaned up
                            from capability groups table.
                            All capabilities in group are now
                            revoked across all nodes.
```

---

## 4. Node Lifecycle

### 4.1 State Machine

```
UNKNOWN ---- CryptoTransport initial handshake ----> CONNECTED
                                                         |
CONNECTED -- Certificate verified by Raft member --> AUTHENTICATED
                                                         |
AUTHENTICATED -- Raft AddServer committed ----------> ADMITTED
                                                         |
                         +-------------------------------+
                         |                               |
                   Raft heartbeat                  Raft RemoveServer
                   failure detected                committed
                         |                               |
                         v                               v
                      REVOKED <---------------------- REVOKED
                         |
                         |  revoke_by_pe_range() completes
                         |  MHT cleared
                         |  Peer key removed from CryptoTransport
                         |  Certificate fingerprint added to blocklist
                         v
                    DISCONNECTED
                         |
                         |  Cannot re-admit with same credentials
                         |  New credentials required from operator
                         v
                      (end)
```

### 4.2 Admission Details

Pre-provisioned on new node (baked into seL4 image by operator):
- Ed25519 key pair (long-term signing identity)
- Curve25519 key pair (CryptoTransport key exchange)
- X.509 certificate (signed by system CA, contains both public keys)
- CA root certificate (for verifying other nodes)
- Address of at least one existing Raft member (bootstrap contact)

AddServer Raft entry:
```
AddServer {
    node_id:          K2
    ed25519_public:   <32 bytes>
    curve25519_public: <32 bytes>
    cert_fingerprint: <SHA-256 of certificate>
    pe_partition:     <PE ID range assigned to K2>
    timestamp:        <Raft log timestamp>
}
```

### 4.3 Revocation Details

RemoveServer Raft entry:
```
RemoveServer {
    node_id:          K2
    cert_fingerprint: <SHA-256 of K2's certificate>
    reason:           "heartbeat_timeout" | "operator_eviction"
    timestamp:        <Raft log timestamp>
}
```

Timing:
| Event | LAN (150us RTT) | WAN (200ms RTT) |
|---|---|---|
| Raft detects heartbeat failure | ~300ms | ~2s |
| RemoveServer committed | +~150us | +~200ms |
| revoke_by_pe_range() completes | +~1-3ms | +~1-3ms |
| CryptoTransport drops peer key | immediate | immediate |
| Total: capabilities revoked | ~600ms | ~2.5s |

---

## 5. Direct Exchange Protocol

### 5.1 Why Sessions Are Not Needed

SemperOS's session protocol served three functions now provided by other
mechanisms:

| Session function | Our replacement | Plane |
|---|---|---|
| Connection management (DTU endpoint setup) | CryptoTransport authenticated channel | Data |
| Scope for cleanup (group related caps) | Raft capability groups | Management |
| Access control (service veto) | Raft membership check | Control |

### 5.2 Spanning Obtain

1. VPE0 on K0 calls exchange (opcode 9, same as local)
2. SyscallHandler (control) detects remote target via MHT lookup
3. CryptoTransport (data) encrypts OBTAIN message to K1
4. K1 CryptoTransport (data) decrypts and verifies
5. K1 SyscallHandler (control) checks membership, creates child capability
6. K1 CryptoTransport (data) encrypts reply
7. K0 SyscallHandler (control) creates local capability
8. RaftPD (management) logs CAP_EXCHANGE with group_id
9. VPE0 receives NO_ERROR

### 5.3 Spanning Delegate (Two-Way Handshake)

Same as obtain but with Hille et al. Section 4.3.2 handshake to prevent
"Invalid" interference:

1-3: Same (K0 sends DELEGATE to K1)
4: K1 creates C2 but does NOT insert yet. Sends preliminary reply.
5: K0 adds C2 to C1's children list. Sends ACK to K1.
6: K1 receives ACK. Inserts C2. Now live and accessible.
7: K0 replies NO_ERROR.

If C1 is revoked between steps 3 and 5, K0 does not send ACK. C2 is
never inserted.

### 5.4 Capability Groups via Raft

Each exchange tagged with group_id in Raft log:
```
RaftEntry {
    type:           CAP_EXCHANGE,
    initiator:      K0,
    target:         K1,
    parent_ddl_key: ...,
    child_ddl_key:  ...,
    group_id:       "K0-K1-flow-rule-001",
    timestamp:      ...
}
```

Bulk revocation by group:
```
RaftEntry {
    type:     CAP_GROUP_REVOKE,
    group_id: "K0-K1-flow-rule-001",
    timestamp: ...
}
```

Advantages over SemperOS sessions:
- Auditable (every exchange in Raft log)
- Survives node failure (K0 can revoke group even if K1 crashed)
- Consensus-backed (no split-brain on group membership)
- Bulk revocation (one entry revokes entire group across all nodes)

SDN examples:
- Flow rule: all caps for switch port 3 VLAN 100 = one group
- Tenant: all caps for tenant X across all nodes = one group
- Network slice: management vs data plane caps = separate groups

---

## 6. Cryptographic Transport

### 6.1 Key Hierarchy

**Layer 1 — Identity (months or on compromise)**
Ed25519 long-term signing key. Authenticates ephemeral key exchanges.
Never used for encryption. Carried in X.509 certificate.

**Layer 2 — Ephemeral key exchange (hourly)**
Curve25519 ephemeral key pair, fresh each epoch. Signed with Ed25519
before sending to peers. Zeroized after session key derivation
(Lib_Memzero0). Provides perfect forward secrecy.

**Layer 3 — Session key (derived from Layer 2)**
```
session_key = HKDF-SHA256(
    Curve25519(my_ephemeral_private, peer_ephemeral_public),
    "semperos-ik-v1" || my_node_id || peer_node_id || epoch
)
```
Per-peer, per-direction. Used for ChaCha20-Poly1305 AEAD.

### 6.2 Key Rotation (NTP-synchronized, hourly)

NTP is MANDATORY. System halts if sync fails at boot or drifts > 2s.

```
T-30s:  Generate new ephemeral Curve25519. Sign with Ed25519.
        Send signed ephemeral to all peers.
T-15s:  Receive all peers' ephemerals. Verify signatures against
        Raft-known Ed25519 keys.
T-0:    Derive new session keys. Accept both old and new (30s window).
T+30s:  Zeroize old keys. Only new key active.
```

### 6.3 Packet Format

```
+-------------------------------------------------------+
| CryptoTransport header (authenticated, not encrypted)  |
|  sender_id:      uint16                                |
|  receiver_id:    uint16                                |
|  sequence_num:   uint64  (anti-replay)                 |
|  nonce:          [12 bytes]                            |
+--------------------------------------------------------+
| Encrypted payload (ChaCha20-Poly1305 AEAD)             |
|  Original inter-kernel message                         |
+--------------------------------------------------------+
| Poly1305 MAC (16 bytes)                                |
|  Covers header (AAD) + encrypted payload               |
+--------------------------------------------------------+
```

### 6.4 Security Properties

| Attack | Prevention |
|---|---|
| Message forgery | Poly1305 MAC fails, packet dropped |
| Message replay | Sequence number check, packet dropped |
| Message tampering | MAC covers header + payload |
| Eavesdropping | ChaCha20 encryption |
| Source impersonation | Pairwise session key, only real peer produces valid MAC |
| Unknown node | Not in peer table, no session key, dropped |
| Past traffic decryption | Ephemeral keys zeroized, forward secrecy |

### 6.5 Performance

On Intel Xeon E5-2695 v4 (2.1 GHz):
- Per round-trip overhead: ~0.6us
- vs LAN RTT (150us): 0.4% overhead
- vs WAN RTT (200ms): 0.0003% overhead
- Local operations: zero overhead (never touch CryptoTransport)

---

## 7. Raft-Replicated State

### 7.1 Log Entry Types

```
enum RaftEntryType {
    ADD_SERVER,        /* Node admission */
    REMOVE_SERVER,     /* Node eviction */
    MHT_UPDATE,        /* PE partition assignment */
    CAP_BLOCK,         /* Capability revocation - Tier 1 */
    CAP_UNBLOCK,       /* Revocation cleanup */
    CAP_EXCHANGE,      /* Exchange record with group_id */
    CAP_GROUP_REVOKE,  /* Bulk revocation by group */
    UPDATE_ROOT_CA,    /* CA certificate rotation */
}
```

### 7.2 Replicated Data Structures

| Structure | Contents | Plane owner | Consumed by |
|---|---|---|---|
| MHT | PE partition -> kernel ID | Management | SyscallHandler (control) |
| Membership list | Admitted node IDs | Management | SyscallHandler, AdmissionAgent (control) |
| Peer key table | node ID -> Ed25519 + Curve25519 keys | Management | CryptoTransport (control) |
| Node whitelist | Expected cert serial numbers | Management | AdmissionAgent (control) |
| Node blocklist | Revoked cert fingerprints | Management | AdmissionAgent (control) |
| BLOCK set | DDL keys with active BLOCK | Management | VDTUService (control) |
| Capability groups | group_id -> set of exchanges | Management | SemperKernel (control) |
| Root CA cert | X.509 DER bytes | Management | AdmissionAgent (control) |

### 7.3 State Machine

```
fn apply(entry):
    match entry.type:
        ADD_SERVER:
            mht.insert(entry.pe_partition, entry.node_id)
            crypto_transport.add_peer(entry.node_id, entry.keys)
            membership.add(entry.node_id)

        REMOVE_SERVER:
            mht.remove(entry.pe_partition)
            crypto_transport.remove_peer(entry.node_id)
            membership.remove(entry.node_id)
            blocklist.add(entry.cert_fingerprint)
            semper_kernel.revoke_by_pe_range(entry.pe_partition)

        CAP_BLOCK:
            vdtu_service.block_capability(entry.ddl_key)

        CAP_EXCHANGE:
            groups.record(entry.group_id, entry.parent_ddl, entry.child_ddl)

        CAP_GROUP_REVOKE:
            for (parent, child) in groups.get(entry.group_id):
                semper_kernel.revoke_cap(child)
            groups.remove(entry.group_id)

        UPDATE_ROOT_CA:
            trust_store.add(entry.ca_cert)
```

---

## 8. Network Architecture

### 8.1 The Device IS the Switch

The edge device integrates SwitchFabric and E1000Port from sel4_xcpng:
- VLAN enforcement inside seL4 trust boundary
- Raft commits update SwitchFabric directly via CAmkES RPC
- Multiple WAN uplinks managed by SwitchFabric
- Self-contained — no external switch trust

### 8.2 802.1X Dual Role

The edge device plays two 802.1X roles:

**Downstream (LAN ports) — Authenticator:**
Edge device challenges LAN clients (laptops, IoT, printers). EAPAuthenticator
processes EAP exchanges. Authorized devices assigned to appropriate VLAN.
Can mint SemperOS capabilities for authenticated local devices.

**Upstream (WAN ports) — Supplicant:**
Edge device authenticates TO the provider's infrastructure. Provider's switch
challenges our device. EAPSupplicant (new component, not in sel4_xcpng)
responds. This is transport-level admission — the provider lets frames
through. Real security is CryptoTransport end-to-end.

### 8.3 VLAN Architecture

| VLAN | Purpose | Membership |
|---|---|---|
| VLAN 100 | Trusted kernel fabric | All admitted nodes |
| VLAN 200 | Management | Operator access, monitoring |
| VLAN 999 | Quarantine | Nodes before Raft AddServer |

VLANs are defense-in-depth, not primary security. CryptoTransport provides
end-to-end security regardless of VLAN configuration.

### 8.4 Transport Agnosticism

Works over any IP transport (MPLS, broadband, 4G LTE, satellite).
DTUBridge sends UDP; CryptoTransport encrypts; transport is untrusted.
SwitchFabric selects uplink based on policy. Same session keys work
regardless of which uplink carries the packet.

### 8.5 Multi-Site

**Thesis: Approach A** — single global Raft quorum. Commit latency bounded
by WAN RTT.

| Operation | LAN (150us) | WAN (200ms) |
|---|---|---|
| Local exchange | 0.17 us | 0.17 us |
| Spanning exchange | ~300 us | ~200 ms |
| Tier 1 BLOCK commit | ~300 us | ~200 ms |
| Node failure detection | ~300 ms | ~2 s |
| CryptoTransport overhead | ~0.6 us | ~0.6 us |

**Production: Approach B** — per-site Raft with cross-site CA trust.
Future work.

---

## 9. PKI Design

### 9.1 CA Hierarchy

```
Root CA (offline, air-gapped)
  |
  +-- Intermediate CA (step-ca, Docker, online)
        |
        +-- Node0 device certificate
        +-- Node1 device certificate
        +-- Node2 device certificate
```

Thesis: step-ca (Smallstep). Production: swappable to EJBCA Enterprise
or any X.509 CA.

### 9.2 Device Certificate

```
X.509 v3:
  CN = node0.semperos.local
  O  = SemperOS-seL4
  OU = PE-Partition-000-010
  serialNumber = NODE0-2026-001
  Subject Public Key: Ed25519 <32 bytes>
  Custom OID 1.3.6.1.4.1.99999.1.1:
    node_id: 0, raft_voter: true, max_vpes: 192
    curve25519_public: <32 bytes>
  Validity: 1 year
  Signature: Ed25519
```

### 9.3 TLS Engine Abstraction

mbedTLS now, miTLS (Everest) later. Compile-time swap via CMake option.
Narrow interface: create, handshake, get_master_secret, destroy.
Blocker for miTLS: krmllib porting to seL4.

---

## 10. Existing Infrastructure Reuse

### 10.1 From http_gateway_x86

| Component | Reuse |
|---|---|
| HACL* build integration (SHA2, HMAC, Lib_Memzero0) | Direct copy |
| KreMLin runtime headers | Direct copy |
| cmake pattern (-ffunction-sections) | Direct copy |
| mbedTLS X.509 + TLS 1.2 | Adaptation for EAP-TLS |
| HACL* ChaCha20-Poly1305, Curve25519, HKDF, Ed25519 | From hacl-star submodule |

### 10.2 From sel4_xcpng

| Component | Reuse |
|---|---|
| EAPAuthenticator (365 lines) | Adapt EAP-MD5 to EAP-TLS |
| EAPOL framing | Direct copy |
| RADIUS client (323 lines) | Direct copy |
| E1000 VLAN offload (CTRL.VME, VFTA) | Direct copy |
| SwitchFabric VLAN policy | Integration as CAmkES component |
| Port authorization blocking | Direct copy |
| FreeRADIUS Docker infra | Direct copy |

### 10.3 New Components

| Component | Effort |
|---|---|
| CryptoTransport | ~1-2 weeks |
| AdmissionAgent | ~1-2 weeks |
| Direct exchange protocol | ~1-2 weeks |
| revoke_by_pe_range() | ~2-3 days |
| RaftPD CAmkES component | ~3-4 weeks |
| Raft capability groups | ~1 week |
| EAP-TLS upgrade | ~2 weeks |
| EAPSupplicant (upstream auth) | ~1-2 weeks |

---

## 11. Security Analysis

### 11.1 Properties Provided

| Property | Mechanism | Plane |
|---|---|---|
| Node authentication | X.509 cert + Raft AddServer | Mgmt + Control |
| Message authentication | Poly1305 MAC (HACL*) | Data |
| Message confidentiality | ChaCha20 (HACL*) | Data |
| Anti-replay | Sequence numbers | Data |
| Forward secrecy | Ephemeral Curve25519 + Lib_Memzero0 | Control + Data |
| Admission consensus | Raft AddServer | Management |
| Revocation consensus | Raft RemoveServer | Management |
| Anti-re-admission | Cert fingerprint blocklist | Management |
| Revocation completeness | revoke_by_pe_range() | Control |
| Audit trail | Append-only Raft log | Management |
| Intra-node isolation | seL4 capabilities (verified) | Control |
| Network isolation | SwitchFabric VLAN | Control + Data |
| Mandatory clock | NTP halt on drift | Control |
| Node liveness | Raft heartbeat | Management |

### 11.2 CVE-2026-20127 Mapping

| Attack stage | This design | Plane |
|---|---|---|
| Auth bypass | X.509 + Raft AddServer | Mgmt + Control |
| Rogue peer injection | CryptoTransport + Raft membership | Control + Data |
| Software downgrade | seL4 write capability | Control |
| Root escalation | No root in seL4 | Control |
| Log purge | Raft append-only log | Management |
| Multi-node revocation | Raft BLOCK + revoke_by_pe_range() | Management + Control |

### 11.3 Known Gaps (Future Work)

| Gap | Plane affected |
|---|---|
| CA compromise | Management |
| Quorum compromise (BFT-Raft) | Management |
| Remote attestation (TPM) | Control |
| Multi-site federation | Management |
| Private key extraction (SEV/TDX) | Data |
| Certificate lifecycle | Management |
| krmllib on seL4 (miTLS swap) | Data |

---

## 12. Implementation Sequencing

```
Phase 1: CryptoTransport                          [Data plane]
Phase 2: revoke_by_pe_range()                     [Control plane]
Phase 3: RaftPD CAmkES component                  [Management plane]
Phase 4: AdmissionAgent + PKI                     [Control + Management]
Phase 5: Direct exchange protocol                 [Control + Data]
Phase 6: SwitchFabric integration                 [Control + Data]
Phase 7: 802.1X (optional)                        [Control + Data]
```

| Phase | Contribution | Plane |
|---|---|---|
| 1-2 | Contribution 3 prerequisites | Data + Control |
| 3 | Contribution 3 (Raft) | Management |
| 4-5 | Contribution 4 (secure distributed) | All three |
| 6-7 | Contribution 4 (SD-WAN integration) | Control + Data |

---

## 13. Benchmark Plan

| Benchmark | Plane measured |
|---|---|
| CryptoTransport encrypt/decrypt | Data |
| Authenticated spanning exchange | All three (end-to-end) |
| Authenticated spanning revoke | All three (end-to-end) |
| Node admission latency | Management + Control |
| Node revocation latency | Management + Control |
| Local ops overhead | Control (should be zero) |
| Spanning ops vs RTT (Exp 4) | Data (network) + Control (crypto) |

Framing: "Our local operations are directly comparable to Hille et al.
Our spanning operations measure an authenticated protocol — the price of
security on a real network. On any physical realization of SemperOS at
rack scale, this cost is necessary. Our work quantifies it for the first
time."

---

## References

- Hille et al. 2019. SemperOS. USENIX ATC.
- Vilanova et al. 2022. FractOS. EuroSys.
- Asmussen et al. 2016. M3. ASPLOS.
- Project Everest. HACL*, miTLS, EverCrypt.
- Klein et al. 2009. seL4. SOSP.
- Cisco SD-WAN Design Guide. Zero-trust certificate-based admission.
- Smallstep step-ca. Private CA for device identity.
- CVE-2026-20127. SD-WAN authentication bypass attack chain.

# Jira & Confluence — SemperOS-seL4

Load this file when keywords: "Jira", "story", "Confluence", "FPT-", "epic", "sprint"

---

## IDs

- **Atlassian Cloud ID:** `8561a05e-3e48-46c3-a755-2d8ea80e84dc`
- **Jira project:** FPT (Ford's PhD Thesis)
- **Epic:** FPT-133 — Distributed capability systems using seL4 as network controller
- **Confluence space:** FO (Ford's Research Program), space ID 2162691
- **Project Memory page:** 2490370 (living shared memory — both web chat and Claude Code read/update)

## Commit Convention

All commits must reference the Jira key: `FPT-<number>: <description>`
Example: `FPT-155: integrate ChaCha20-Poly1305 in DTUBridge`

## Confluence Design Pages (FO space)

| Page | When to read |
|------|-------------|
| SemperOS-seL4 — Project Memory | Every session start — living shared state |
| SemperOS-seL4 — Architecture | When modifying component wiring or three-plane design |
| SemperOS-seL4 — Security Analysis | When working on CryptoTransport, admission, or threat model |
| SemperOS-seL4 — Revocation Design | When working on FPT-158, FPT-159, or two-tier revocation |
| SemperOS-seL4 — Protocol Analysis | When working on FPT-160, exchange protocol, or session elimination |

## Story Map (snapshot 2026-03-30 — query Jira for live status)

### Completed

| Key | Story |
|-----|-------|
| FPT-152 | SemperOS executes on real x86_64 hardware |
| FPT-153 | vDTU endpoint state machine formally verified |
| FPT-154 | Local capability operations benchmarked on real hardware |

### Spikes

| Key | Story | Status |
|-----|-------|--------|
| FPT-167 | Spike: Raft implementation selection | **Done** — RedisLabs/raft. See `docs/spikes/FPT-167-raft-selection.md` |
| FPT-168 | Spike: Time acquisition and synchronization | To Do (3 days) |

### Active Stories — Contributions 3 & 4

| Key | Story | Priority | Depends On |
|-----|-------|----------|------------|
| FPT-155 | Authenticated encryption over untrusted networks | Highest | — |
| FPT-156 | Raft consensus across 3 XCP-ng nodes | Highest | FPT-155, ~~FPT-167~~ |
| FPT-165 | Node membership and network policy via Raft | Highest | FPT-156 |
| FPT-166 | Synchronized time with Raft-backed clock skew detection | High | FPT-155, FPT-165, FPT-168 |
| FPT-160 | Cross-node capability exchange | High | FPT-165 |
| FPT-159 | Cross-kernel revocation without blocking | High | FPT-165 |
| FPT-158 | Compromised node revocation with anti-re-admission | High | FPT-165 |
| FPT-157 | Node admission via certificate + quorum vote | Medium | FPT-165 |
| FPT-161 | VLAN policy inside seL4 trust boundary | Medium | FPT-165 |
| FPT-162 | Downstream 802.1X EAP-TLS | Low | FPT-157, FPT-161 |
| FPT-163 | End-to-end benchmarks | Low | FPT-158, FPT-160 |
| FPT-164 | Thesis chapter | Lowest | FPT-163 |

### Dependency Chain

```
Spike FPT-167 (DONE)                   Spike FPT-168 (Time/RTC)
       |                                      |
FPT-155 (packet encryption)                   |
       |                                      |
FPT-156 (Raft consensus)                      |
       |                                      |
FPT-165 (Raft membership + policy)            |
       |                                      |
       +---> FPT-157 (admission)              |
       +---> FPT-158 (node revocation)        |
       +---> FPT-159 (cap revocation)         |
       +---> FPT-160 (exchange)               |
       +---> FPT-161 (VLAN)                   |
       |                                      |
       +-------> FPT-166 (time + key rotation)
                    |
          FPT-162 (802.1X) -- from FPT-157 + FPT-161
          FPT-163 (benchmarks) -- from FPT-158 + FPT-160
          FPT-164 (thesis chapter) -- from FPT-163
```

## Workflow

```
TO DO -> DESIGN -> IN PROGRESS -> EVALUATE -> WRITING -> REVIEW -> DONE
```

Any of IN PROGRESS / EVALUATE / WRITING / REVIEW can transition to BLOCKED.
BLOCKED -> DESIGN (re-design) or BLOCKED -> CANCEL (give up).

**Spike convention:** When a story is BLOCKED by a design question, create a
Spike card (issue type: Spike), resolve it, update the Confluence Project
Memory page with the decision, then unblock the original story.

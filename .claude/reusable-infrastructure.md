# Reusable Infrastructure from Sister Projects

Load this file when keywords: "HACL", "sel4_xcpng", "reuse", "sister project", "mbedTLS", "802.1X", "SwitchFabric"

---

| Source project | What it provides | For which contribution |
|---|---|---|
| `projects/http_gateway_x86` | HACL* build integration (vendored C, KreMLin headers, cmake pattern -ffunction-sections), mbedTLS TLS 1.2 + X.509 certificate handling, HACL* SHA2/HMAC/Lib_Memzero0 compiled and tested in CAmkES x86_64 | C4: CryptoTransport (HACL* ChaCha20-Poly1305, Curve25519, HKDF, Ed25519 from hacl-star submodule), AdmissionAgent (mbedTLS for X.509 cert verification) |
| `projects/sel4_xcpng` | Complete 802.1X EAP authenticator (365 lines) + RADIUS client (323 lines) + EAPOL framing, 802.1Q E1000 VLAN hw offload (CTRL.VME, VFTA, tag insert/strip), SwitchFabric VLAN policy engine (access/trunk modes, 391 lines), port authorization blocking at driver level, FreeRADIUS Docker test infrastructure | C4: SwitchFabric integration (CAmkES component), DTUBridge VLAN filtering (E1000 offload), EAPAuthenticator (optional, needs EAP-MD5→EAP-TLS upgrade), EAPSupplicant (new, for upstream provider auth) |

Full design: `docs/secure-distributed-capability-architecture.md`
Inspection reports: see project knowledge files for http_gateway_x86 HACL*
report and sel4_xcpng 802.1X/802.1Q report.

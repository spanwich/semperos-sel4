/* FPT-155: CryptoTransport implementation — ChaCha20-Poly1305 AEAD.
 *
 * Static pre-shared keys derived deterministically from node pair.
 * Nonce = sender_id(1) || 0(3) || sequence_num(4) || 0(4) — unique per packet.
 * Anti-replay: reject packets with seq <= rx_seq_max per peer.
 */

#include "crypto_transport.h"
#include "Hacl_AEAD_Chacha20Poly1305.h"
#include <string.h>
#include <stdio.h>

/* FPT-179 Stage-5: per-reason silent-drop counters for ct_decrypt.
 * Each discriminable rejection path increments a distinct counter so
 * the operator can see which failure mode dominates on XCP-ng. */
static uint32_t g_ct_rej_rcvr   = 0;
static uint32_t g_ct_rej_sender = 0;
static uint32_t g_ct_rej_replay = 0;
static uint32_t g_ct_rej_auth   = 0;
uint32_t ct_get_rej_rcvr(void)   { return g_ct_rej_rcvr; }
uint32_t ct_get_rej_sender(void) { return g_ct_rej_sender; }
uint32_t ct_get_rej_replay(void) { return g_ct_rej_replay; }
uint32_t ct_get_rej_auth(void)   { return g_ct_rej_auth; }

/* Per-peer crypto state */
typedef struct {
    uint8_t  key[CT_KEY_SIZE];
    uint32_t tx_seq;        /* outbound sequence counter */
    uint32_t rx_seq_max;    /* highest inbound sequence seen (anti-replay) */
    int      active;
} ct_peer_t;

static uint8_t g_my_id;
static ct_peer_t g_peers[CT_MAX_NODES];

/* Deterministic PSK derivation: HKDF would be proper, but for the static
 * test keys we just XOR a base key with the sorted node pair.
 * This produces a unique symmetric key per pair: key[a][b] == key[b][a]. */
static void derive_psk(uint8_t a, uint8_t b, uint8_t key[CT_KEY_SIZE])
{
    /* Base key material — fixed test vector */
    static const uint8_t base[CT_KEY_SIZE] = {
        0x53, 0x65, 0x6d, 0x70, 0x65, 0x72, 0x4f, 0x53,  /* "SemperOS" */
        0x2d, 0x73, 0x65, 0x4c, 0x34, 0x2d, 0x50, 0x53,  /* "-seL4-PS" */
        0x4b, 0x2d, 0x76, 0x31, 0x2d, 0x46, 0x50, 0x54,  /* "K-v1-FPT" */
        0x2d, 0x31, 0x35, 0x35, 0x2d, 0x74, 0x65, 0x73,  /* "-155-tes" */
    };

    uint8_t lo = a < b ? a : b;
    uint8_t hi = a < b ? b : a;

    memcpy(key, base, CT_KEY_SIZE);
    /* Mix in the node pair — simple but sufficient for test keys */
    key[0] ^= lo;
    key[1] ^= hi;
    key[2] ^= (lo * 37 + hi * 53);
    key[3] ^= (lo ^ hi ^ 0xA5);
}

int ct_init(uint8_t my_node_id)
{
    g_my_id = my_node_id;
    memset(g_peers, 0, sizeof(g_peers));

    /* Derive PSK for each possible peer */
    for (int i = 0; i < CT_MAX_NODES; i++) {
        if (i == my_node_id)
            continue;
        derive_psk(my_node_id, (uint8_t)i, g_peers[i].key);
        g_peers[i].tx_seq = 0;
        g_peers[i].rx_seq_max = 0;
        g_peers[i].active = 1;
    }

    printf("[CRYPTO] Initialized CryptoTransport for node %u\n", my_node_id);
    return 0;
}

/* FPT-179: reset a peer's inbound replay state after a detected reboot
 * (signalled externally via HELLO epoch change). Clears rx_seq_max so
 * the peer's post-reboot tx_seq=1 packets are no longer rejected as
 * replay. Our tx_seq is intentionally NOT reset: it continues ascending
 * and will exceed the peer's (freshly zeroed) rx_seq_max on their first
 * RX from us. The PSK and active flag are unchanged — we are not
 * re-keying, just clearing the sequence-window for the peer's new life. */
void ct_reset_peer(uint8_t peer_id)
{
    if (peer_id >= CT_MAX_NODES)
        return;
    printf("[CRYPTO] peer %u rx_seq_max reset (was %u)\n",
           peer_id, g_peers[peer_id].rx_seq_max);
    g_peers[peer_id].rx_seq_max = 0;
}

/* Build 12-byte nonce from sender_id + sequence_num.
 * Layout: [sender_id:1][0:3][seq_be:4][0:4] = 12 bytes.
 * Unique per (sender, seq) pair — never reused with same key. */
static void build_nonce(uint8_t sender_id, uint32_t seq, uint8_t nonce[CT_NONCE_SIZE])
{
    memset(nonce, 0, CT_NONCE_SIZE);
    nonce[0] = sender_id;
    /* Big-endian sequence at offset 4 */
    nonce[4] = (uint8_t)(seq >> 24);
    nonce[5] = (uint8_t)(seq >> 16);
    nonce[6] = (uint8_t)(seq >> 8);
    nonce[7] = (uint8_t)(seq);
}

int ct_encrypt(uint8_t peer_id,
               const uint8_t *plaintext, size_t plaintext_len,
               uint8_t *output, size_t output_max)
{
    if (peer_id >= CT_MAX_NODES || !g_peers[peer_id].active)
        return -1;

    size_t total = CT_HEADER_SIZE + plaintext_len + CT_TAG_SIZE;
    if (output_max < total)
        return -1;

    ct_peer_t *peer = &g_peers[peer_id];
    uint32_t seq = ++peer->tx_seq;

    /* Build header (AAD) */
    ct_header_t *hdr = (ct_header_t *)output;
    hdr->sender_id = g_my_id;
    hdr->receiver_id = peer_id;
    hdr->sequence_num = seq;
    build_nonce(g_my_id, seq, hdr->nonce);

    /* Pointers into output buffer */
    uint8_t *ciphertext = output + CT_HEADER_SIZE;
    uint8_t *tag = output + CT_HEADER_SIZE + plaintext_len;

    /* AEAD encrypt: ciphertext + tag from plaintext + AAD */
    Hacl_AEAD_Chacha20Poly1305_encrypt(
        ciphertext,             /* output ciphertext */
        tag,                    /* output tag (16 bytes) */
        (uint8_t *)plaintext,   /* input plaintext */
        (uint32_t)plaintext_len,
        (uint8_t *)hdr,         /* AAD = header */
        (uint32_t)CT_HEADER_SIZE,
        peer->key,              /* key */
        hdr->nonce              /* nonce */
    );

#ifdef CT_DEBUG
    printf("[CRYPTO] ct_encrypt: peer=%u seq=%u pt_len=%zu ct_total=%zu\n",
           peer_id, seq, plaintext_len, total);
#endif

    return (int)total;
}

int ct_decrypt(const uint8_t *input, size_t input_len,
               uint8_t *plaintext, size_t plaintext_max,
               uint8_t *sender_id_out)
{
    if (input_len < CT_OVERHEAD)
        return -1;

    const ct_header_t *hdr = (const ct_header_t *)input;

    /* Check receiver */
    if (hdr->receiver_id != g_my_id) {
        printf("[CRYPTO] REJECT: receiver_id=%u but my_id=%u\n",
               hdr->receiver_id, g_my_id);
        g_ct_rej_rcvr++;
        return -1;
    }

    uint8_t sender = hdr->sender_id;
    if (sender >= CT_MAX_NODES || !g_peers[sender].active) {
        printf("[CRYPTO] REJECT: sender=%u invalid (max=%d, active=%d)\n",
               sender, CT_MAX_NODES,
               (sender < CT_MAX_NODES) ? g_peers[sender].active : -1);
        g_ct_rej_sender++;
        return -1;
    }

    ct_peer_t *peer = &g_peers[sender];

    /* Anti-replay: reject if sequence <= highest seen */
    if (hdr->sequence_num <= peer->rx_seq_max) {
        printf("[CRYPTO] REJECT: replay seq=%u <= max=%u from sender=%u\n",
               hdr->sequence_num, peer->rx_seq_max, sender);
        g_ct_rej_replay++;
        return -1;
    }

    size_t ciphertext_len = input_len - CT_HEADER_SIZE - CT_TAG_SIZE;
    if (plaintext_max < ciphertext_len)
        return -1;

    const uint8_t *ciphertext = input + CT_HEADER_SIZE;
    const uint8_t *tag = input + CT_HEADER_SIZE + ciphertext_len;

    /* Rebuild nonce from header for verification */
    uint8_t nonce[CT_NONCE_SIZE];
    build_nonce(sender, hdr->sequence_num, nonce);

    /* Verify nonce matches header (defense-in-depth) */
    if (memcmp(nonce, hdr->nonce, CT_NONCE_SIZE) != 0)
        return -1;

    /* AEAD decrypt: returns 0 on success (auth OK) */
    uint32_t rc = Hacl_AEAD_Chacha20Poly1305_decrypt(
        plaintext,              /* output plaintext */
        (uint8_t *)ciphertext,  /* input ciphertext */
        (uint32_t)ciphertext_len,
        (uint8_t *)hdr,         /* AAD = header */
        (uint32_t)CT_HEADER_SIZE,
        peer->key,              /* key */
        nonce,                  /* nonce */
        (uint8_t *)tag          /* tag */
    );

    if (rc != 0) {
        /* Authentication failed — tampered or wrong key */
        printf("[CRYPTO] AEAD auth failure from node %u seq %u\n",
               sender, hdr->sequence_num);
        g_ct_rej_auth++;
        return -1;
    }

    /* Update anti-replay window */
    peer->rx_seq_max = hdr->sequence_num;

    if (sender_id_out)
        *sender_id_out = sender;

#ifdef CT_DEBUG
    printf("[CRYPTO] ct_decrypt: sender=%u seq=%u pt_len=%zu auth=OK\n",
           sender, hdr->sequence_num, ciphertext_len);
#endif

    return (int)ciphertext_len;
}

/* FPT-155: CryptoTransport — per-packet AEAD for inter-kernel DTU messages.
 *
 * Every outbound UDP packet is encrypted with ChaCha20-Poly1305.
 * Every inbound packet is decrypted and authenticated; failures are dropped.
 * Static pre-shared keys for now (replaced by admission-derived keys in FPT-157).
 */

#ifndef CRYPTO_TRANSPORT_H
#define CRYPTO_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>

#define CT_KEY_SIZE     32   /* ChaCha20-Poly1305 key */
#define CT_NONCE_SIZE   12   /* ChaCha20-Poly1305 nonce */
#define CT_TAG_SIZE     16   /* Poly1305 MAC tag */
#define CT_MAX_NODES    8

/* Packet header — sent in cleartext, authenticated as AAD by AEAD.
 * Total: 18 bytes. */
typedef struct {
    uint8_t  sender_id;
    uint8_t  receiver_id;
    uint32_t sequence_num;
    uint8_t  nonce[CT_NONCE_SIZE];
} __attribute__((packed)) ct_header_t;

#define CT_HEADER_SIZE  sizeof(ct_header_t)  /* 18 */
#define CT_OVERHEAD     (CT_HEADER_SIZE + CT_TAG_SIZE)  /* 34 */

/* Initialize CryptoTransport with static PSKs.
 * my_node_id: this node's KERNEL_ID (0, 1, or 2).
 * Returns 0 on success. */
int ct_init(uint8_t my_node_id);

/* Encrypt plaintext for a specific peer.
 *
 * Constructs: [ct_header_t (AAD)] [ciphertext] [16-byte tag]
 *
 * Returns total output length on success, -1 on error.
 * output_max must be >= plaintext_len + CT_OVERHEAD. */
int ct_encrypt(uint8_t peer_id,
               const uint8_t *plaintext, size_t plaintext_len,
               uint8_t *output, size_t output_max);

/* Decrypt and authenticate a received packet.
 *
 * Input format: [ct_header_t] [ciphertext] [16-byte tag]
 *
 * Returns plaintext length on success.
 * Returns -1 on: auth failure, replay, bad header, wrong receiver. */
int ct_decrypt(const uint8_t *input, size_t input_len,
               uint8_t *plaintext, size_t plaintext_max,
               uint8_t *sender_id_out);

/* FPT-179 Stage-5: per-reason ct_decrypt reject counters (read-only).
 * Lets DTUBridge print aggregate drop reasons without poking internals. */
uint32_t ct_get_rej_rcvr(void);
uint32_t ct_get_rej_sender(void);
uint32_t ct_get_rej_replay(void);
uint32_t ct_get_rej_auth(void);

/* FPT-179: clear a peer's replay window after externally-detected peer
 * reboot (HELLO epoch change). Sets rx_seq_max=0; tx_seq unchanged. */
void ct_reset_peer(uint8_t peer_id);

#endif /* CRYPTO_TRANSPORT_H */

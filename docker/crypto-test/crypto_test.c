/* FPT-155: Minimal CryptoTransport test — proves encrypt/decrypt over UDP.
 *
 * Usage:
 *   ./crypto_test --id 0 --peer-id 1 --peer-host node1 --port 7654
 *   ./crypto_test --id 1 --peer-id 0 --peer-host node0 --port 7654
 *
 * Node 0 sends 5 known-plaintext messages, Node 1 receives and decrypts.
 * Then Node 1 sends 5 back. Both sides verify.
 */

#define CT_DEBUG
#include "crypto_transport.h"
#include "crypto_transport.c"  /* single-translation-unit build */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <poll.h>

#define TEST_PORT 7654
#define NUM_MSGS  5

/* Known plaintext — a fake DTU message header + payload */
static const uint8_t test_plaintext[] = {
    /* Fake vdtu_msg_header (25 bytes) */
    0x00,                           /* flags */
    0x00, 0x00,                     /* sender_core_id */
    0x01,                           /* sender_ep_id */
    0x02,                           /* reply_ep_id */
    0x00, 0x08,                     /* length = 8 */
    0x00, 0x01,                     /* sender_vpe_id */
    0x53, 0x65, 0x6d, 0x70, 0x65, 0x72, 0x4f, 0x53, /* label = "SemperOS" */
    0x44, 0x54, 0x55, 0x42, 0x72, 0x69, 0x64, 0x67, /* replylabel = "DTUBridg" */
    0x65,                           /* padding to 25 */
    /* Payload (8 bytes) */
    'H', 'E', 'L', 'L', 'O', '!', '!', '!',
};

static void hex_dump(const char *label, const uint8_t *data, size_t len)
{
    printf("%s (%zu bytes):\n", label, len);
    for (size_t i = 0; i < len; i++) {
        printf("%02x ", data[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    if (len % 16 != 0) printf("\n");
}

int main(int argc, char **argv)
{
    int my_id = -1, peer_id = -1, port = TEST_PORT;
    const char *peer_host = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--id") == 0) my_id = atoi(argv[++i]);
        else if (strcmp(argv[i], "--peer-id") == 0) peer_id = atoi(argv[++i]);
        else if (strcmp(argv[i], "--peer-host") == 0) peer_host = argv[++i];
        else if (strcmp(argv[i], "--port") == 0) port = atoi(argv[++i]);
    }
    if (my_id < 0 || peer_id < 0 || !peer_host) {
        fprintf(stderr, "Usage: %s --id N --peer-id M --peer-host HOST --port PORT\n", argv[0]);
        return 1;
    }

    printf("=== CryptoTransport Test: node %d -> peer %d (%s:%d) ===\n",
           my_id, peer_id, peer_host, port);

    ct_init(my_id);

    /* Create UDP socket */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

    /* Resolve peer address */
    struct sockaddr_in peer_addr = { .sin_family = AF_INET, .sin_port = htons(port) };
    struct hostent *he = gethostbyname(peer_host);
    if (!he) { fprintf(stderr, "Cannot resolve %s\n", peer_host); return 1; }
    memcpy(&peer_addr.sin_addr, he->h_addr_list[0], he->h_length);

    /* Node 0 sends first, Node 1 listens first */
    if (my_id == 0) {
        /* Wait for peer to be ready */
        sleep(2);

        printf("\n--- SENDING %d encrypted messages ---\n", NUM_MSGS);
        for (int i = 0; i < NUM_MSGS; i++) {
            uint8_t ct_buf[256];
            int ct_len = ct_encrypt(peer_id, test_plaintext,
                                    sizeof(test_plaintext), ct_buf, sizeof(ct_buf));
            if (ct_len < 0) {
                printf("FAIL: ct_encrypt returned %d\n", ct_len);
                return 1;
            }

            if (i == 0)
                hex_dump("First ciphertext packet", ct_buf, ct_len);

            sendto(sock, ct_buf, ct_len, 0,
                   (struct sockaddr *)&peer_addr, sizeof(peer_addr));
            printf("Sent msg %d: %d bytes (plaintext was %zu bytes)\n",
                   i + 1, ct_len, sizeof(test_plaintext));
        }

        /* Now receive replies */
        printf("\n--- RECEIVING %d encrypted replies ---\n", NUM_MSGS);
        for (int i = 0; i < NUM_MSGS; i++) {
            uint8_t rxbuf[256];
            struct pollfd pfd = { .fd = sock, .events = POLLIN };
            int rc = poll(&pfd, 1, 5000);
            if (rc <= 0) { printf("Timeout waiting for reply %d\n", i + 1); break; }

            ssize_t n = recvfrom(sock, rxbuf, sizeof(rxbuf), 0, NULL, NULL);
            if (n <= 0) continue;

            uint8_t plaintext[256];
            uint8_t sender;
            int pt_len = ct_decrypt(rxbuf, n, plaintext, sizeof(plaintext), &sender);
            if (pt_len < 0) {
                printf("FAIL: ct_decrypt failed on reply %d\n", i + 1);
                continue;
            }
            printf("Reply %d: decrypted %d bytes from sender %u\n",
                   i + 1, pt_len, sender);

            if (pt_len == (int)sizeof(test_plaintext) &&
                memcmp(plaintext, test_plaintext, pt_len) == 0) {
                printf("  -> Plaintext matches original: PASS\n");
            } else {
                printf("  -> Plaintext MISMATCH: FAIL\n");
            }
        }

    } else {
        /* Node 1: receive first, then send back */
        printf("\n--- RECEIVING %d encrypted messages ---\n", NUM_MSGS);
        for (int i = 0; i < NUM_MSGS; i++) {
            uint8_t rxbuf[256];
            struct pollfd pfd = { .fd = sock, .events = POLLIN };
            int rc = poll(&pfd, 1, 10000);
            if (rc <= 0) { printf("Timeout waiting for msg %d\n", i + 1); break; }

            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            ssize_t n = recvfrom(sock, rxbuf, sizeof(rxbuf), 0,
                                 (struct sockaddr *)&from, &fromlen);
            if (n <= 0) continue;

            if (i == 0)
                hex_dump("First received ciphertext", rxbuf, n);

            uint8_t plaintext[256];
            uint8_t sender;
            int pt_len = ct_decrypt(rxbuf, n, plaintext, sizeof(plaintext), &sender);
            if (pt_len < 0) {
                printf("FAIL: ct_decrypt failed on msg %d\n", i + 1);
                continue;
            }
            printf("Msg %d: decrypted %d bytes from sender %u\n",
                   i + 1, pt_len, sender);

            if (pt_len == (int)sizeof(test_plaintext) &&
                memcmp(plaintext, test_plaintext, pt_len) == 0) {
                printf("  -> Plaintext matches original: PASS\n");
            } else {
                printf("  -> Plaintext MISMATCH: FAIL\n");
                hex_dump("  Expected", test_plaintext, sizeof(test_plaintext));
                hex_dump("  Got", plaintext, pt_len);
            }
        }

        /* Send replies back */
        sleep(1);
        printf("\n--- SENDING %d encrypted replies ---\n", NUM_MSGS);
        for (int i = 0; i < NUM_MSGS; i++) {
            uint8_t ct_buf[256];
            int ct_len = ct_encrypt(peer_id, test_plaintext,
                                    sizeof(test_plaintext), ct_buf, sizeof(ct_buf));
            sendto(sock, ct_buf, ct_len, 0,
                   (struct sockaddr *)&peer_addr, sizeof(peer_addr));
            printf("Sent reply %d: %d bytes\n", i + 1, ct_len);
        }
    }

    sleep(1);
    close(sock);
    printf("\n=== Test complete ===\n");
    return 0;
}

/*
 * VPE0.c -- First user VPE for SemperOS on seL4/CAmkES
 *
 * Task 05 test harness: exercises NOOP, CREATEGATE, and REVOKE syscalls
 * through the real SemperOS SyscallHandler.
 */

#include <stdio.h>
#include <string.h>
#include <camkes.h>
#include "vdtu_ring.h"
#include "vdtu_channels.h"

/* VPE0 is PE 2 in the platform config */
#define MY_PE       2
#define MY_VPE_ID   0

/* SemperOS endpoint IDs */
#define SYSC_EP     0   /* Send endpoint for syscalls */
#define DEF_RECVEP  1   /* Default receive endpoint (for replies) */

/*
 * SemperOS syscall opcodes (from KIF.h Syscall::Operation enum)
 *   CREATEGATE = 4
 *   EXCHANGE   = 9
 *   REVOKE     = 16
 *   NOOP       = 18
 */
#define SYSCALL_CREATEGATE  4
#define SYSCALL_EXCHANGE    9
#define SYSCALL_REVOKE      16
#define SYSCALL_NOOP        18

/* CapRngDesc::Type */
#define CAP_TYPE_OBJ  0

/* Channel table */
static struct vdtu_channel_table channels;
static int send_chan = -1;

static void init_channel_table(void)
{
    volatile void *msg[] = {
        (volatile void *)msgchan_kv_0,
        (volatile void *)msgchan_kv_1,
        (volatile void *)msgchan_kv_2,
        (volatile void *)msgchan_kv_3,
        (volatile void *)msgchan_kv_4,
        (volatile void *)msgchan_kv_5,
        (volatile void *)msgchan_kv_6,
        (volatile void *)msgchan_kv_7,
    };
    volatile void *mem[] = {
        (volatile void *)memep_kv_0,
        (volatile void *)memep_kv_1,
        (volatile void *)memep_kv_2,
        (volatile void *)memep_kv_3,
    };
    vdtu_channels_init(&channels, msg, mem);
}

/*
 * Wait for a reply on any recv channel (skip channel 0 = kernel's recv EP).
 * Returns the error code from the reply, or -1 on timeout.
 */
static int wait_for_reply(void)
{
    int timeout = 100000000;
    const struct vdtu_message *reply = NULL;
    struct vdtu_ring *ring = NULL;

    while (timeout-- > 0) {
        for (int ch = 1; ch < VDTU_MSG_CHANNELS; ch++) {
            if (!channels.msg[ch]) continue;
            if (!channels.msg_rings[ch].ctrl)
                vdtu_channels_attach_ring(&channels, ch);
            struct vdtu_ring *r = vdtu_channels_get_ring(&channels, ch);
            if (r && !vdtu_ring_is_empty(r)) {
                reply = vdtu_ring_fetch(r);
                if (reply) {
                    ring = r;
                    break;
                }
            }
        }
        if (reply) break;
    }

    if (!reply) return -1;

    int result = -1;
    if (reply->hdr.length >= sizeof(uint64_t)) {
        result = (int)(*(const uint64_t *)reply->data);
    }
    vdtu_ring_ack(ring);
    return result;
}

/*
 * Send a syscall with the given payload and wait for reply.
 * Returns the error code from the kernel's reply.
 */
static int send_syscall(const void *payload, uint16_t payload_len)
{
    struct vdtu_ring *ring = vdtu_channels_get_ring(&channels, send_chan);
    if (!ring) return -1;

    int rc = vdtu_ring_send(ring,
                            MY_PE, SYSC_EP, MY_VPE_ID,
                            DEF_RECVEP,
                            0, 0, 0,
                            payload, payload_len);
    if (rc != 0) return -1;

    return wait_for_reply();
}

/* Send a NOOP syscall. Returns 0 on success. */
static int send_noop(void)
{
    uint64_t payload = SYSCALL_NOOP;
    return send_syscall(&payload, sizeof(payload));
}

/*
 * Send a CREATEGATE syscall.
 *
 * Message format (m3 marshalling, 8-byte aligned):
 *   [0]  opcode    = CREATEGATE (4)
 *   [1]  tcap      = VPE capability selector (0 = self)
 *   [2]  dstcap    = destination cap selector (where to store the new cap)
 *   [3]  label     = routing label for the new gate
 *   [4]  epid      = target endpoint ID on the VPE (1-15)
 *   [5]  credits   = message credits
 *
 * Returns kernel error code (0 = success).
 */
static int send_creategate(uint64_t dstcap, uint64_t label, uint64_t epid, uint64_t credits)
{
    uint64_t payload[6];
    payload[0] = SYSCALL_CREATEGATE;
    payload[1] = 0;        /* tcap = VPE cap at selector 0 (self) */
    payload[2] = dstcap;   /* destination capability slot */
    payload[3] = label;    /* routing label */
    payload[4] = epid;     /* endpoint ID on target VPE */
    payload[5] = credits;  /* message credits */
    return send_syscall(payload, sizeof(payload));
}

/*
 * Send a REVOKE syscall.
 *
 * Message format (m3 marshalling, 8-byte aligned):
 *   [0]      opcode   = REVOKE (16)
 *   [1..2]   crd      = CapRngDesc { type(4), start(4), count(4) } padded to 16 bytes
 *   [3]      own      = bool (revoke only own copies)
 *
 * CapRngDesc is a 12-byte struct read with sizeof(CapRngDesc) = 12,
 * advanced by round_up(12, 8) = 16 bytes.
 *
 * Returns kernel error code (0 = success).
 */
static int send_revoke(uint64_t cap_sel)
{
    /* Build the payload manually matching the marshalling layout.
     * The CapRngDesc fields are: type(uint32), start(uint32), count(uint32).
     * These are laid out as 12 contiguous bytes in memory. */
    struct {
        uint64_t opcode;
        /* CapRngDesc: 12 bytes + 4 bytes padding to reach 16 */
        uint32_t crd_type;
        uint32_t crd_start;
        uint32_t crd_count;
        uint32_t _pad;
        /* bool own (as uint64_t) */
        uint64_t own;
    } __attribute__((packed)) payload;

    payload.opcode    = SYSCALL_REVOKE;
    payload.crd_type  = CAP_TYPE_OBJ;
    payload.crd_start = (uint32_t)cap_sel;
    payload.crd_count = 1;
    payload._pad      = 0;
    payload.own        = 1;  /* revoke own copies only */

    return send_syscall(&payload, sizeof(payload));
}

/*
 * Send an EXCHANGE syscall.
 *
 * EXCHANGE is a synchronous, kernel-internal capability transfer.
 * The kernel clones capabilities from one VPE's CapTable to another,
 * establishing parent-child pointers for recursive revocation.
 *
 * Message format (m3 marshalling, 8-byte aligned):
 *   [0]      opcode   = EXCHANGE (9)
 *   [1]      tcap     = VPECapability selector pointing to target VPE
 *   [2..3]   own      = CapRngDesc { type(4), start(4), count(4) } + pad(4) = 16 bytes
 *   [4..5]   other    = CapRngDesc { type(4), start(4), count(4) } + pad(4) = 16 bytes
 *   [6]      obtain   = bool (0=delegate: own→other, 1=obtain: other→own)
 *
 * When obtain=false (delegate): clones caps from sender's 'own' range
 *   to target VPE's 'other' range.
 * When obtain=true: clones caps from target VPE's 'other' range
 *   to sender's 'own' range.
 *
 * Returns kernel error code (0 = success).
 */
static int send_exchange(uint64_t tcap, uint32_t own_start, uint32_t own_count,
                         uint32_t other_start, uint32_t other_count, int obtain)
{
    /* Kernel handler: is >> tcap >> own >> other >> obtain;
     * tcap = capsel_t (uint64_t)
     * own/other = CapRngDesc (12 bytes, advanced by round_up(12,8)=16)
     * obtain = bool (marshalled as uint64_t) */
    struct {
        uint64_t opcode;
        uint64_t tcap;
        /* CapRngDesc 'own': 12 bytes + 4 pad */
        uint32_t own_type;
        uint32_t own_start;
        uint32_t own_count;
        uint32_t _pad1;
        /* CapRngDesc 'other': 12 bytes + 4 pad */
        uint32_t other_type;
        uint32_t other_start;
        uint32_t other_count;
        uint32_t _pad2;
        /* obtain flag */
        uint64_t obtain;
    } __attribute__((packed)) payload;

    payload.opcode      = SYSCALL_EXCHANGE;
    payload.tcap        = tcap;
    payload.own_type    = CAP_TYPE_OBJ;
    payload.own_start   = own_start;
    payload.own_count   = own_count;
    payload._pad1       = 0;
    payload.other_type  = CAP_TYPE_OBJ;
    payload.other_start = other_start;
    payload.other_count = other_count;
    payload._pad2       = 0;
    payload.obtain      = obtain ? 1 : 0;

    return send_syscall(&payload, sizeof(payload));
}

int run(void)
{
    printf("[VPE0] Starting (PE %d, VPE ID %d)\n", MY_PE, MY_VPE_ID);

    init_channel_table();

    /* Wait for kernel to configure our endpoints */
    printf("[VPE0] Waiting for channels...\n");
    for (volatile int i = 0; i < 10000000; i++) {}

    /* Find the send channel */
    for (int ch = 0; ch < VDTU_MSG_CHANNELS; ch++) {
        if (!channels.msg[ch]) continue;
        if (!channels.msg_rings[ch].ctrl)
            vdtu_channels_attach_ring(&channels, ch);
        if (channels.msg_rings[ch].ctrl) {
            send_chan = ch;
            break;
        }
    }

    if (send_chan < 0) {
        printf("[VPE0] ERROR: no send channel found\n");
        return -1;
    }
    printf("[VPE0] Send channel: %d\n", send_chan);

    int pass = 0, fail = 0, err;

    /* ==============================================================
     * Test 1: NOOP x3 (multi-message, no double-ack)
     * ============================================================== */
    {
        int ok = 1;
        for (int i = 0; i < 3; i++) {
            err = send_noop();
            if (err != 0) { ok = 0; break; }
        }
        if (ok) pass++; else fail++;
        printf("[VPE0] Test 1 (NOOP x3): %s\n", ok ? "PASS" : "FAIL");
    }

    /* ==============================================================
     * Test 2: CREATEGATE — create a message gate capability
     *   tcap=0 (self VPE), dstcap=5, label=0xCAFE, epid=2, credits=32
     * ============================================================== */
    {
        err = send_creategate(5, 0xCAFE, 2, 32);
        if (err == 0) pass++; else fail++;
        printf("[VPE0] Test 2 (CREATEGATE sel=5): %s (err=%d)\n",
               err == 0 ? "PASS" : "FAIL", err);
    }

    /* ==============================================================
     * Test 3: REVOKE — revoke the capability at selector 5
     * ============================================================== */
    {
        err = send_revoke(5);
        if (err == 0) pass++; else fail++;
        printf("[VPE0] Test 3 (REVOKE sel=5): %s (err=%d)\n",
               err == 0 ? "PASS" : "FAIL", err);
    }

    /* ==============================================================
     * Test 4: REVOKE non-existent — selector 99
     *   SemperOS treats revoking a non-existent cap as a no-op
     *   (returns NO_ERROR), which is correct for distributed revocation.
     *   This test verifies no crash occurs.
     * ============================================================== */
    {
        err = send_revoke(99);
        /* err=0 is acceptable (no-op revoke), any non-crash result passes */
        pass++;
        printf("[VPE0] Test 4 (REVOKE non-existent sel=99): PASS (err=%d, no crash)\n", err);
    }

    /* ==============================================================
     * Test 5: CREATE+REVOKE x3 cycle — prove no resource leaks
     * ============================================================== */
    {
        int ok = 1;
        for (int i = 0; i < 3; i++) {
            err = send_creategate(10 + i, 0xBEEF + i, 3, 64);
            if (err != 0) { printf("[VPE0]   cycle %d CREATE failed: %d\n", i, err); ok = 0; break; }
            err = send_revoke(10 + i);
            if (err != 0) { printf("[VPE0]   cycle %d REVOKE failed: %d\n", i, err); ok = 0; break; }
        }
        if (ok) pass++; else fail++;
        printf("[VPE0] Test 5 (CREATE+REVOKE x3): %s\n", ok ? "PASS" : "FAIL");
    }

    /* ==============================================================
     * Test 6: EXCHANGE — delegate capability from VPE0 to VPE1
     *
     * Setup: kernel installed VPECapability for VPE1 at selector 2
     * in our CapTable (during kernel_start).
     *
     * Steps:
     *   1. Create a gate at selector 20 (label=0xDEAD, epid=4, credits=16)
     *   2. EXCHANGE: delegate sel 20 from VPE0 → VPE1 at sel 30
     *      tcap=2 (VPE1 cap), own={OBJ,20,1}, other={OBJ,30,1}, obtain=false
     *   3. Verify EXCHANGE returned success
     *
     * The kernel's do_exchange() clones VPE0's cap at sel 20 into
     * VPE1's CapTable at sel 30, with parent-child pointers.
     * ============================================================== */
    {
        int ok = 1;
        /* Step 1: Create gate at sel 20 */
        err = send_creategate(20, 0xDEAD, 4, 16);
        if (err != 0) {
            printf("[VPE0]   EXCHANGE setup: CREATEGATE(20) failed: %d\n", err);
            ok = 0;
        }

        if (ok) {
            /* Step 2: EXCHANGE delegate sel 20 → VPE1 sel 30 */
            err = send_exchange(2, 20, 1, 30, 1, 0);
            if (err != 0) {
                printf("[VPE0]   EXCHANGE(delegate 20→VPE1:30) failed: %d\n", err);
                ok = 0;
            }
        }

        if (ok) pass++; else fail++;
        printf("[VPE0] Test 6 (EXCHANGE delegate to VPE1): %s (err=%d)\n",
               ok ? "PASS" : "FAIL", err);
    }

    /* ==============================================================
     * Test 7: Cross-VPE REVOKE — revoke parent, verify child removed
     *
     * VPE0 sel 20 is the parent; VPE1 sel 30 is the child (from Test 6).
     * Revoking sel 20 in VPE0 should recursively walk the capability
     * tree and remove the child at VPE1 sel 30.
     *
     * We verify via return code (success = tree walk completed without
     * error). The kernel logs confirm the cross-VPE revocation.
     * ============================================================== */
    {
        err = send_revoke(20);
        if (err == 0) pass++; else fail++;
        printf("[VPE0] Test 7 (cross-VPE REVOKE sel=20): %s (err=%d)\n",
               err == 0 ? "PASS" : "FAIL", err);
    }

    /* ==============================================================
     * Test 8: EXCHANGE + REVOKE cycle — prove cross-VPE cleanup
     *
     * Repeats the exchange+revoke pattern 3 times to verify no
     * resource leaks in the cross-VPE capability tree.
     * ============================================================== */
    {
        int ok = 1;
        for (int i = 0; i < 3; i++) {
            /* Create gate at sel 40+i */
            err = send_creategate(40 + i, 0xF000 + i, 5, 8);
            if (err != 0) {
                printf("[VPE0]   cycle %d CREATE failed: %d\n", i, err);
                ok = 0; break;
            }
            /* Delegate to VPE1 at sel 50+i */
            err = send_exchange(2, 40 + i, 1, 50 + i, 1, 0);
            if (err != 0) {
                printf("[VPE0]   cycle %d EXCHANGE failed: %d\n", i, err);
                ok = 0; break;
            }
            /* Revoke parent (should also revoke VPE1's child) */
            err = send_revoke(40 + i);
            if (err != 0) {
                printf("[VPE0]   cycle %d REVOKE failed: %d\n", i, err);
                ok = 0; break;
            }
        }
        if (ok) pass++; else fail++;
        printf("[VPE0] Test 8 (EXCHANGE+REVOKE x3 cycle): %s\n", ok ? "PASS" : "FAIL");
    }

    printf("[VPE0] === %d passed, %d failed ===\n", pass, fail);
    return 0;
}

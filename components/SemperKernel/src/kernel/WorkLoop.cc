/*
 * Copyright (C) 2019, Matthias Hille <matthias.hille@tu-dresden.de>, 
 * Nils Asmussen <nils@os.inf.tu-dresden.de>
 * Economic rights: Technische Universitaet Dresden (Germany)
 *
 * This file is part of SemperOS.
 *
 * SemperOS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * SemperOS is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License version 2 for more details.
 */

extern "C" {
#include <stdio.h>
}
#include <base/Common.h>
#include <base/tracing/Tracing.h>
#include <base/log/Kernel.h>
#include <base/WorkLoop.h>

#include "KernelcallHandler.h"
#include "SyscallHandler.h"
#include "WorkLoop.h"
#include "pes/PEManager.h"
#include "thread/ThreadManager.h"

#if defined(__sel4__)
extern "C" void net_poll(void);

#if !defined(SEMPEROS_NO_NETWORK)
/*
 * Dispatch a raw DTU message received from the network to the
 * KernelcallHandler (Task 08). Called from net_poll() in camkes_entry.c
 * when an inbound message is not a PING/PONG.
 *
 * The vdtu_message and m3::DTU::Message have identical packed header
 * layouts (25 bytes), so we cast the raw bytes directly.
 */
extern "C" void dispatch_net_krnlc(const void *raw_msg, uint16_t len) {
    if(len < 25) return;  /* minimum DTU header size */
    const m3::DTU::Message *msg =
        reinterpret_cast<const m3::DTU::Message *>(raw_msg);
    kernel::KernelcallHandler &krnlch = kernel::KernelcallHandler::get();
    kernel::GateIStream is(krnlch.rcvgate(0), msg);
    /* Disable auto-ack: the inbound ring is acked separately by
     * net_poll() via vdtu_ring_ack(). Letting GateIStream's destructor
     * call mark_read() would ack the wrong (KRNLC) ring. */
    is.claim();  /* sets _ack = false */
    /* Debug: print opcode for network-delivered KRNLC messages */
    {
        const unsigned char *d = msg->data;
        uint64_t op = 0;
        if (len >= 25 + 8) memcpy(&op, d, 8);
        printf("[dispatch_net_krnlc] len=%u op=%lu label=0x%lx\n",
               len, (unsigned long)op, (unsigned long)msg->label);
    }
    krnlch.handle_message(is, nullptr);
}
#endif /* !SEMPEROS_NO_NETWORK */
#endif

#if defined(__host__)
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/stat.h>

static int sigchilds = 0;

static void sigchild(int) {
    sigchilds++;
    signal(SIGCHLD, sigchild);
}

static void check_childs() {
    for(; sigchilds > 0; sigchilds--) {
        int status;
        pid_t pid = wait(&status);
        if(WIFEXITED(status)) {
            KLOG(VPES, "Child " << pid << " exited with status " << WEXITSTATUS(status));
        }
        else if(WIFSIGNALED(status)) {
            KLOG(VPES, "Child " << pid << " was killed by signal " << WTERMSIG(status));
            if(WCOREDUMP(status))
                KLOG(VPES, "Child " << pid << " core dumped");
        }
    }
}
#endif

namespace kernel {

void WorkLoop::run() {
#if defined(__host__)
    signal(SIGCHLD, sigchild);
#endif
    EVENT_TRACER_KWorkLoop_run();

    m3::DTU &dtu = m3::DTU::get();
    KernelcallHandler &krnlch = KernelcallHandler::get();
    SyscallHandler &sysch = SyscallHandler::get();
    m3::ThreadManager &tmng = m3::ThreadManager::get();
    int krnlep[DTU::KRNLC_GATES];
    for(int i = 0; i < DTU::KRNLC_GATES; i++) {
        krnlep[i] = krnlch.epid(i);
    }
    int sysep[DTU::SYSC_GATES];
    for(int i = 0; i < DTU::SYSC_GATES; i++)
        sysep[i] = sysch.epid(i);
    int srvep = sysch.srvepid();
    const m3::DTU::Message *msg;
    /* Debug: print EP→channel mapping once */
    static int ep_map_printed = 0;
    if (!ep_map_printed) {
        printf("[WorkLoop] SYSC EPs: ");
        for (int i = 0; i < DTU::SYSC_GATES; i++)
            printf("sysep[%d]=%d ", i, sysep[i]);
        printf("\n[WorkLoop] KRNLC EPs: ");
        for (int i = 0; i < DTU::KRNLC_GATES; i++)
            printf("krnlep[%d]=%d ", i, krnlep[i]);
        printf("\nsrvep=%d\n", srvep);
        ep_map_printed = 1;
    }

    while(has_items()) {
        m3::DTU::get().wait();

#if defined(__sel4__)
        /* Process network messages FIRST — dispatch_net_krnlc delivers
         * cross-node KRNLC responses (createSessResp, PONG, connect).
         * Must run before SYSC processing to avoid circular dependency:
         * sendTo blocks on msgsInflight → needs msg_received from PONG →
         * needs dispatch_net_krnlc → needs net_poll. */
        net_poll();
#endif

        for(int i = 0; i < DTU::KRNLC_GATES; i++) {
            msg = dtu.fetch_msg(krnlep[i]);
            if(msg) {
#if defined(__sel4__)
                dtu.mark_read(krnlep[i], 0); /* early ack — handler may block */
#endif
                GateIStream is(krnlch.rcvgate(i), msg);
#if defined(__sel4__)
                is.claim(); /* prevent double-ack from destructor */
#endif
                krnlch.handle_message(is, nullptr);
            }
        }

        for(int i = 0; i < DTU::SYSC_GATES; i++) {
            msg = dtu.fetch_msg(sysep[i]);
            if(msg) {
#if defined(__sel4__)
                /* Ack the ring slot IMMEDIATELY before any handler call.
                 * With cooperative threading, handlers can block in
                 * wait_for(). Without early ack, another worker thread
                 * running the WorkLoop would fetch+process the same
                 * unacked message, causing duplicate syscall handling. */
                dtu.mark_read(sysep[i], 0);

                /* On sel4, VPE service replies arrive through the SYSC
                 * channel (VPE1 can't write to the kernel's service recv
                 * EP ring because SPSC is unidirectional). Detect replies
                 * by VDTU_FLAG_REPLY and dispatch to the service callback
                 * instead of the syscall handler. */
                if((msg->flags & 0x01 /* VDTU_FLAG_REPLY */) && msg->label) {
                    RecvGate *gate = reinterpret_cast<RecvGate*>(msg->label);
                    GateIStream is(*gate, msg);
                    is.claim();
                    gate->notify_all(is);
                    continue;
                }
#endif
                RecvGate *rgate = reinterpret_cast<RecvGate*>(msg->label);
#if defined(__sel4__)
                /* On sel4, VPE sends may not have the correct label
                 * (vDTU doesn't auto-fill from EP config like gem5 HW).
                 * Look up the VPE from senderCoreId via PEManager. */
                if(!msg->label) {
                    int sender_core = msg->senderCoreId;
                    if(PEManager::get().exists(sender_core)) {
                        rgate = &PEManager::get().vpe(sender_core).syscall_gate();
                    }
                }
#endif
                GateIStream is(*rgate, msg);
                is.claim(); /* prevent double-ack from destructor */
                sysch.handle_message(is, nullptr);
                EVENT_TRACE_FLUSH_LIGHT();
            }
        }

        msg = dtu.fetch_msg(srvep);
        if(msg) {
            RecvGate *gate = reinterpret_cast<RecvGate*>(msg->label);
            if(gate) {
                GateIStream is(*gate, msg);
                gate->notify_all(is);
            } else {
                printf("[WorkLoop] WARNING: srvep msg with null label, dropping\n");
            }
        }

        tmng.yield();
#if defined(__sel4__)
        net_poll();
#endif
#if defined(__host__)
        check_childs();
#endif
    }
}

}

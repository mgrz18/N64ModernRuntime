#include <thread>

#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

struct QueuedMessage {
    PTR(OSMesgQueue) mq;
    OSMesg mesg;
    bool jam;
};

static moodycamel::BlockingConcurrentQueue<QueuedMessage> external_messages {};

void enqueue_external_message(PTR(OSMesgQueue) mq, OSMesg msg, bool jam) {
    external_messages.enqueue({mq, msg, jam});
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block);

void dequeue_external_messages(RDRAM_ARG1) {
    QueuedMessage to_send;
    while (external_messages.try_dequeue(to_send)) {
        do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false);
    }
}

void ultramodern::wait_for_external_message(RDRAM_ARG1) {
    QueuedMessage to_send;
    external_messages.wait_dequeue(to_send);
    do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false);
}

void ultramodern::wait_for_external_message_timed(RDRAM_ARG1, u32 millis) {
    QueuedMessage to_send;
    if (external_messages.wait_dequeue_timed(to_send, std::chrono::milliseconds{millis})) {
        do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false);
    }
}

extern "C" void osCreateMesgQueue(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg, s32 count) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    mq->blocked_on_recv = NULLPTR;
    mq->blocked_on_send = NULLPTR;
    mq->msgCount = count;
    mq->msg = msg;
    mq->validCount = 0;
    mq->first = 0;
}

s32 MQ_GET_COUNT(OSMesgQueue *mq) {
    return mq->validCount;
}

s32 MQ_IS_EMPTY(OSMesgQueue *mq) {
    return mq->validCount == 0;
}

s32 MQ_IS_FULL(OSMesgQueue* mq) {
    return MQ_GET_COUNT(mq) >= mq->msgCount;
}

bool do_send(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, bool jam, bool block) {
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    uint32_t dbg_m = (uint32_t)(uintptr_t)msg;
    // Diagnostic: log all sends of OSScMsg*-like pointers (0x80XXXXXX) to any queue,
    // to identify which queue is the game's clientQ (gfxFrameMsgQ) and whether RETRACE
    // forwarding stops after the first GFX task.
    // Reject bogus msgs universally. A valid OSMesg is either:
    //   - a small event id (< 0x1000), used by intQ for VI=0x29A / SP=0x29B / DP=0x29C
    //   - NULL, or
    //   - an OSScMsg*/generic pointer in the K0 RDRAM range [0x80000000..0x80800000)
    // Framebuffer/VRAM addresses like 0x7FC00000 or 0x7FE30000 come from corrupted
    // replyMsg fields in OSTask/OSScTask and would flood clientQ with garbage if let through.
    // Previously this check was gated to the hardcoded clientQ address 0x80141C90; that
    // address can differ across runs, so we now apply the filter to every queue.
    {
        uint32_t mv = (uint32_t)(uintptr_t)msg;
        bool msg_valid = (mv < 0x1000u) || ((mv >= 0x80000000u) && (mv < 0x80800000u));
        if (!msg_valid) {
            static int reject_log = 0;
            if (++reject_log <= 10) {
                fprintf(stderr, "[do_send] rejected bogus msg=0x%08X to mq=0x%08X (out of range)\n",
                    mv, (uint32_t)mq_);
            }
            return true;  // pretend success, silently drop
        }
    }

    if (!block) {
        // If non-blocking, fail if the queue is full.
        if (MQ_IS_FULL(mq)) {
            // Two queues are critical to the scheduler ↔ game ↔ scheduler event loop:
            //  - intQ (0x80141D70): scheduler intQ - VI(0x29A) / SP(0x29B) / DP(0x29C) events
            //  - clientQ (0x80141C90): bossMainloop client queue - RETRACE(type=1) / DONE(type=2)
            // When full, we evict messages that are harmless to drop (retrace ticks) and
            // preserve critical completion events. Without this, a full queue quietly drops
            // SP/DP/DONE and the scheduler/game deadlocks.
            bool is_int_q = (uint32_t)mq_ == 0x80141D70;
            bool is_client_q = (uint32_t)mq_ == 0x80141C90;
            if (is_int_q || is_client_q) {
                bool evicted = false;
                for (int i = 0; i < mq->validCount; i++) {
                    s32 idx = (mq->first + i) % mq->msgCount;
                    uint32_t slot_val = (uint32_t)(uintptr_t)TO_PTR(OSMesg, mq->msg)[idx];
                    bool is_retrace = false;
                    if (is_int_q) {
                        // intQ stores small event msg ids as OSMesg ints. Retrace = 0x29A.
                        is_retrace = (slot_val == 0x29A);
                    } else {
                        // clientQ stores OSScMsg*. RETRACE has gen.type=1 at offset 0
                        // (2-byte short, N64 big-endian). Check the pointed-at bytes.
                        if (slot_val != 0 && slot_val < 0x80800000) {
                            uint32_t phys = slot_val & 0x3FFFFFF;
                            // byte 0 and 1 of the struct at phys, via XOR-3 for N64 semantics
                            uint8_t b0 = rdram[phys ^ 3];
                            uint8_t b1 = rdram[(phys + 1) ^ 3];
                            uint32_t type_short = ((uint32_t)b0 << 8) | (uint32_t)b1;
                            is_retrace = (type_short == 1);
                        }
                    }
                    if (is_retrace) {
                        // Remove this slot, shift subsequent entries down.
                        for (int j = i; j < mq->validCount - 1; j++) {
                            s32 a = (mq->first + j) % mq->msgCount;
                            s32 b = (mq->first + j + 1) % mq->msgCount;
                            TO_PTR(OSMesg, mq->msg)[a] = TO_PTR(OSMesg, mq->msg)[b];
                        }
                        mq->validCount--;
                        evicted = true;
                        break;
                    }
                }
                if (!evicted) {
                    // No retrace to evict — as a last resort, drop the oldest to let the
                    // new msg through. Risk: this may drop a DONE. But the alternative
                    // (failing the send) also loses the msg.
                    mq->first = (mq->first + 1) % mq->msgCount;
                    mq->validCount--;
                }
                // fall through to add below
            } else {
                return false;
            }
        }
    }
    else {
        // Otherwise, yield this thread until the queue has room.
        while (MQ_IS_FULL(mq)) {
            debug_printf("[Message Queue] Thread %d is blocked on send\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_send), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
    }
    
    if (jam) {
        // Jams insert at the head of the message queue's buffer.
        mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[mq->first] = msg;
        mq->validCount++;
    }
    else {
        // Sends insert at the tail of the message queue's buffer.
        s32 last = (mq->first + mq->validCount) % mq->msgCount;
        TO_PTR(OSMesg, mq->msg)[last] = msg;
        mq->validCount++;
    }

    // If any threads were blocked on receiving from this message queue, pop the first one and schedule it.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv);
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        PTR(OSThread) woken = ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue);
        ultramodern::schedule_running_thread(PASS_RDRAM woken);
    }

    return true;
}

bool do_recv(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, bool block) {
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    if (!block) {
        // If non-blocking, fail if the queue is empty
        if (MQ_IS_EMPTY(mq)) {
            return false;
        }
    } else {
        // Before blocking, drain any pending external messages. This avoids a deadlock
        // where: scheduler thread is blocked on intQ, bossMainloop is blocked on
        // gfxFrameMsgQ, and all externals (VI retraces, etc.) sit in external_messages
        // with no game thread left to drain them. Without this, the only drain path is
        // pause_self's 1ms poll in an idle thread, which may not be scheduled reliably.
        QueuedMessage to_send;
        while (external_messages.try_dequeue(to_send)) {
            do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false);
        }

        // Now yield in a loop until our queue has a message.
        while (MQ_IS_EMPTY(mq)) {
            debug_printf("[Message Queue] Thread %d is blocked on receive\n", TO_PTR(OSThread, ultramodern::this_thread())->id);
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
    }

    // Drop msgs that are clearly in the N64 VRAM/TMEM/DMEM range (0x7F000000..0x7FFFFFFF).
    // These are garbage from buggy rspGfxTaskStart callers that pass a framebuffer/DMEM
    // pointer as the replyMsg argument. When dispatched into a client queue as an
    // OSScMsg*, bossMainloop reads random bytes as the msg type and gets stuck in a loop.
    // We filter them out at recv time regardless of which queue is being read from.
    while (mq->validCount > 0) {
        uint32_t slot_val = (uint32_t)(uintptr_t)TO_PTR(OSMesg, mq->msg)[mq->first];
        // Integer small values (<0x1000) are legal event IDs (e.g. 0x29A VI).
        // K0 RDRAM pointers in [0x80000000..0x80800000) are valid OSScMsg*.
        // Everything else is garbage we silently drop.
        bool ok = (slot_val < 0x1000) || ((slot_val >= 0x80000000u) && (slot_val < 0x80800000u));
        if (ok) break;
        mq->first = (mq->first + 1) % mq->msgCount;
        mq->validCount--;
        static int drop_log = 0;
        if (++drop_log <= 20) {
            fprintf(stderr, "[do_recv] dropped bogus msg=0x%08X from mq=0x%08X\n", slot_val, (uint32_t)mq_);
        }
    }
    if (mq->validCount == 0) {
        if (!block) return false;
        while (MQ_IS_EMPTY(mq)) {
            ultramodern::thread_queue_insert(PASS_RDRAM GET_MEMBER(OSMesgQueue, mq_, blocked_on_recv), ultramodern::this_thread());
            ultramodern::run_next_thread_and_wait(PASS_RDRAM1);
        }
    }

    if (msg_ != NULLPTR) {
        *TO_PTR(OSMesg, msg_) = TO_PTR(OSMesg, mq->msg)[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;

    // If any threads were blocked on sending to this message queue, pop the first one and schedule it.
    PTR(PTR(OSThread)) blocked_queue = GET_MEMBER(OSMesgQueue, mq_, blocked_on_send);
    if (!ultramodern::thread_queue_empty(PASS_RDRAM blocked_queue)) {
        ultramodern::schedule_running_thread(PASS_RDRAM ultramodern::thread_queue_pop(PASS_RDRAM blocked_queue));
    }

    return true;
}

// Process at most one external message to prevent scheduling livelock.
// Processing all queued VI retraces at once causes the scheduler to preempt
// the current thread repeatedly, preventing progress during level loading.
void dequeue_one_external_message(RDRAM_ARG1) {
    QueuedMessage to_send;
    if (external_messages.try_dequeue(to_send)) {
        do_send(PASS_RDRAM to_send.mq, to_send.mesg, to_send.jam, false);
    }
}

extern "C" s32 osSendMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = false;

    // Don't directly send to the message queue if this isn't a game thread to avoid contention.
    if (!ultramodern::is_game_thread()) {
        enqueue_external_message(mq_, msg, jam);
        return 0;
    }

    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to send the message.
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);

    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osJamMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);
    bool jam = true;

    // Don't directly send to the message queue if this isn't a game thread to avoid contention.
    if (!ultramodern::is_game_thread()) {
        enqueue_external_message(mq_, msg, jam);
        return 0;
    }

    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to send the message.
    bool sent = do_send(PASS_RDRAM mq_, msg, jam, flags == OS_MESG_BLOCK);

    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return sent ? 0 : -1;
}

extern "C" s32 osRecvMesg(RDRAM_ARG PTR(OSMesgQueue) mq_, PTR(OSMesg) msg_, s32 flags) {
    OSMesgQueue *mq = TO_PTR(OSMesgQueue, mq_);

    assert(ultramodern::is_game_thread() && "RecvMesg not allowed outside of game threads.");

    // Handle any messages that have been received from an external thread.
    dequeue_external_messages(PASS_RDRAM1);

    // Try to receive a message.
    bool received = do_recv(PASS_RDRAM mq_, msg_, flags == OS_MESG_BLOCK);

    // Check the queue to see if this thread should swap execution to another.
    ultramodern::check_running_queue(PASS_RDRAM1);

    return received ? 0 : -1;
}

#include <thread>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <variant>
#include <unordered_map>
#include <utility>
#include <mutex>
#include <queue>
#include <cstring>
#include <csignal>
#include <csetjmp>

#include "blockingconcurrentqueue.h"

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

#include "ultramodern/rsp.hpp"
#include "ultramodern/renderer_context.hpp"

// Bridge to the host app's UI: true once the launcher RmlUi content has been hidden
// by draw_hook. Used to gate enable_instant_present so PresentEarly doesn't freeze
// the launcher on screen.
extern "C" int recompui_is_launcher_fully_hidden(void);

static ultramodern::events::callbacks_t events_callbacks{};

void ultramodern::events::set_callbacks(const ultramodern::events::callbacks_t& callbacks) {
    events_callbacks = callbacks;
}

struct SpTaskAction {
    OSTask task;
};

struct SwapBuffersAction {
    uint32_t origin;
};

struct UpdateConfigAction {
};

using Action = std::variant<SpTaskAction, SwapBuffersAction, UpdateConfigAction>;

static struct {
    struct {
        std::thread thread;
        PTR(OSMesgQueue) mq = NULLPTR;
        PTR(void) current_buffer = NULLPTR;
        PTR(void) next_buffer = NULLPTR;
        OSMesg msg = (OSMesg)0;
        int retrace_count = 1;
    } vi;
    struct {
        std::thread gfx_thread;
        std::thread task_thread;
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } sp;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } dp;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } ai;
    struct {
        PTR(OSMesgQueue) mq = NULLPTR;
        OSMesg msg = (OSMesg)0;
    } si;
    // The same message queue may be used for multiple events, so share a mutex for all of them
    std::mutex message_mutex;
    uint8_t* rdram;
    moodycamel::BlockingConcurrentQueue<Action> action_queue{};
    moodycamel::BlockingConcurrentQueue<OSTask*> sp_task_queue{};
    moodycamel::ConcurrentQueue<OSThread*> deleted_threads{};
} events_context{};

extern "C" void osSetEventMesg(RDRAM_ARG OSEvent event_id, PTR(OSMesgQueue) mq_, OSMesg msg) {
    OSMesgQueue* mq = TO_PTR(OSMesgQueue, mq_);
    std::lock_guard lock{ events_context.message_mutex };

    switch (event_id) {
        case OS_EVENT_SP:
            events_context.sp.msg = msg;
            events_context.sp.mq = mq_;
            break;
        case OS_EVENT_DP:
            events_context.dp.msg = msg;
            events_context.dp.mq = mq_;
            break;
        case OS_EVENT_AI:
            events_context.ai.msg = msg;
            events_context.ai.mq = mq_;
            break;
        case OS_EVENT_SI:
            events_context.si.msg = msg;
            events_context.si.mq = mq_;
    }
}

extern "C" void osViSetEvent(RDRAM_ARG PTR(OSMesgQueue) mq_, OSMesg msg, u32 retrace_count) {
    std::lock_guard lock{ events_context.message_mutex };
    events_context.vi.mq = mq_;
    events_context.vi.msg = msg;
    events_context.vi.retrace_count = retrace_count;
}

uint64_t total_vis = 0;


extern std::atomic_bool exited;
extern moodycamel::LightweightSemaphore graphics_shutdown_ready;

void set_dummy_vi();

#ifdef __APPLE__
extern "C" void ensure_thread_autorelease_pool();
#endif

void vi_thread_func() {
#ifdef __APPLE__
    ensure_thread_autorelease_pool();
#endif
    ultramodern::set_native_thread_name("VI Thread");
    // This thread should be prioritized over every other thread in the application, as it's what allows
    // the game to generate new audio and gfx lists.
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Critical);
    using namespace std::chrono_literals;

    int remaining_retraces = events_context.vi.retrace_count;

    while (!exited) {
        // Determine the next VI time (more accurate than adding 16ms each VI interrupt)
        auto next = ultramodern::get_start() + (total_vis * 1000000us) / (60 * ultramodern::get_speed_multiplier());
        //if (next > std::chrono::high_resolution_clock::now()) {
        //    printf("Sleeping for %" PRIu64 " us to get from %" PRIu64 " us to %" PRIu64 " us \n",
        //        (next - std::chrono::high_resolution_clock::now()) / 1us,
        //        (std::chrono::high_resolution_clock::now() - events_context.start) / 1us,
        //        (next - events_context.start) / 1us);
        //} else {
        //    printf("No need to sleep\n");
        //}
        // Detect if there's more than a second to wait and wait a fixed amount instead for the next VI if so, as that usually means the system clock went back in time.
        if (std::chrono::floor<std::chrono::seconds>(next - std::chrono::high_resolution_clock::now()) > 1s) {
            // printf("Skipping the next VI wait\n");
            next = std::chrono::high_resolution_clock::now();
        }
        ultramodern::sleep_until(next);
        // Calculate how many VIs have passed
        uint64_t new_total_vis = (ultramodern::time_since_start() * (60 * ultramodern::get_speed_multiplier()) / 1000ms) + 1;
        if (new_total_vis > total_vis + 1) {
            //printf("Skipped % " PRId64 " frames in VI interupt thread!\n", new_total_vis - total_vis - 1);
        }
        total_vis = new_total_vis;

        remaining_retraces--;

        {
            std::lock_guard lock{ events_context.message_mutex };
            uint8_t* rdram = events_context.rdram;
            if (remaining_retraces == 0) {
                remaining_retraces = events_context.vi.retrace_count;

                if (ultramodern::is_game_started()) {
                    if (events_context.vi.mq != NULLPTR) {
                        osSendMesg(PASS_RDRAM events_context.vi.mq, events_context.vi.msg, OS_MESG_NOBLOCK);
                    }
                }
                else {
                    set_dummy_vi();
                    static bool swap = false;
                    uint32_t vi_origin = 0x400 + 0x280; // Skip initial RDRAM contents and add the usual origin offset
                    // Offset by one FB every other frame so RT64 continues drawing
                    if (swap) {
                        vi_origin += 0x25800;
                    }
                    osViSwapBuffer(rdram, vi_origin);
                    swap = !swap;
                }
            }
            if (events_context.ai.mq != NULLPTR) {
                if (osSendMesg(PASS_RDRAM events_context.ai.mq, events_context.ai.msg, OS_MESG_NOBLOCK) == -1) {
                    //printf("Game skipped a AI frame!\n");
                }
            }
        }

        if (events_callbacks.vi_callback != nullptr) {
            events_callbacks.vi_callback();
        }
    }
}

void sp_complete() {
    uint8_t* rdram = events_context.rdram;
    std::lock_guard lock{ events_context.message_mutex };
    osSendMesg(PASS_RDRAM events_context.sp.mq, events_context.sp.msg, OS_MESG_NOBLOCK);
}

void dp_complete() {
    uint8_t* rdram = events_context.rdram;
    std::lock_guard lock{ events_context.message_mutex };
    static int dp_count = 0;
    dp_count++;
    if (dp_count <= 3 || dp_count % 60 == 0) {
        fprintf(stderr, "[dp_complete #%d]\n", dp_count);
    }
    osSendMesg(PASS_RDRAM events_context.dp.mq, events_context.dp.msg, OS_MESG_NOBLOCK);
}

void task_thread_func(uint8_t* rdram, moodycamel::LightweightSemaphore* thread_ready) {
#ifdef __APPLE__
    ensure_thread_autorelease_pool();
#endif
    ultramodern::set_native_thread_name("SP Task Thread");
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Normal);

    // Notify the caller thread that this thread is ready.
    thread_ready->signal();

    while (true) {
        // Wait until an RSP task has been sent
        OSTask* task;
        events_context.sp_task_queue.wait_dequeue(task);

        if (task == nullptr) {
            return;
        }

        {
            static int task_count = 0;
            task_count++;
            if (task_count <= 5) {
// fprintf(stderr, "[DEBUG] RSP task #%d: type=%" PRIu32 "\n", task_count, task->t.type);
            }
        }
        if (!ultramodern::rsp::run_task(PASS_RDRAM task)) {
            static int rsp_fail_count = 0;
            rsp_fail_count++;
            if (rsp_fail_count <= 10) {
                fprintf(stderr, "Failed to execute task type: %" PRIu32 " (warning %d, continuing)\n", task->t.type, rsp_fail_count);
            }
            // Don't exit - signal completion anyway so the game doesn't deadlock
        }

        // Tell the game that the RSP has completed
        sp_complete();
    }
}

std::atomic_uint32_t display_refresh_rate = 60;
std::atomic<float> resolution_scale = 1.0f;

uint32_t ultramodern::get_target_framerate(uint32_t original) {
    auto& config = ultramodern::renderer::get_graphics_config();

    switch (config.rr_option) {
        case ultramodern::renderer::RefreshRate::Original:
        default:
            return original;
        case ultramodern::renderer::RefreshRate::Manual:
            return config.rr_manual_value;
        case ultramodern::renderer::RefreshRate::Display:
            return display_refresh_rate.load();
    }
}

uint32_t ultramodern::get_display_refresh_rate() {
    return display_refresh_rate.load();
}

float ultramodern::get_resolution_scale() {
    return resolution_scale.load();
}

void ultramodern::trigger_config_action() {
    events_context.action_queue.enqueue(UpdateConfigAction{});
}

std::atomic<ultramodern::renderer::SetupResult> renderer_setup_result = ultramodern::renderer::SetupResult::Success;
std::atomic<ultramodern::renderer::GraphicsApi> renderer_chosen_api = ultramodern::renderer::GraphicsApi::Auto;

void gfx_thread_func(uint8_t* rdram, moodycamel::LightweightSemaphore* thread_ready, ultramodern::renderer::WindowHandle window_handle) {
#ifdef __APPLE__
    ensure_thread_autorelease_pool();
#endif
    bool enabled_instant_present = false;
    using namespace std::chrono_literals;

    ultramodern::set_native_thread_name("Gfx Thread");
    ultramodern::set_native_thread_priority(ultramodern::ThreadPriority::Normal);

    auto old_config = ultramodern::renderer::get_graphics_config();

    auto renderer_context = ultramodern::renderer::create_render_context(rdram, window_handle, ultramodern::renderer::get_graphics_config().developer_mode);

    renderer_chosen_api.store(renderer_context->get_chosen_api());
    if (!renderer_context->valid()) {
        renderer_setup_result.store(renderer_context->get_setup_result());
        // Notify the caller thread that this thread is ready.
        thread_ready->signal();
        return;
    }

    if (events_callbacks.gfx_init_callback != nullptr) {
        events_callbacks.gfx_init_callback();
    }

    ultramodern::rsp::init();

    // Notify the caller thread that this thread is ready.
    thread_ready->signal();

    while (!exited) {
        // Try to pull an action from the queue
        Action action;
        if (events_context.action_queue.wait_dequeue_timed(action, 1ms)) {
            // Determine the action type and act on it
            if (const auto* task_action = std::get_if<SpTaskAction>(&action)) {
                // Turn on instant present if the game has been started and it hasn't been turned on yet.
                // DISABLED: PresentEarly mode makes updateScreen return early without advancing the
                // present queue, relying on DL workload completion to push presents. When GE's DLs
                // fail (unknown opcodes → SAFETY ABORT), no present fires and the pipeline stalls.
                // Keeping the default updateScreen-driven presentation keeps draw_hook firing.
                // Enable instant_present ONLY after the launcher RmlUi content has been
                // hidden by draw_hook. PresentEarly mode stops draw_hook from firing,
                // so enabling it before the hide propagates would freeze the launcher
                // on screen. The ui_renderer's draw_hook sets the bridge flag once it
                // has applied swap_document(Menu::None).
                if (ultramodern::is_game_started() && !enabled_instant_present &&
                    recompui_is_launcher_fully_hidden()) {
                    // GE_NO_INSTANT_PRESENT=1: keep updateScreen-driven presentation.
                    // Useful while GE DL pipeline is broken — allows frames to present even
                    // when RT64 DL workloads don't complete cleanly. Cost: input latency.
                    if (getenv("GE_NO_INSTANT_PRESENT") == nullptr) {
                        renderer_context->enable_instant_present();
                        fprintf(stderr, "[events] enable_instant_present activated (launcher was hidden)\n");
                    } else {
                        fprintf(stderr, "[events] enable_instant_present SKIPPED (GE_NO_INSTANT_PRESENT=1)\n");
                    }
                    enabled_instant_present = true;
                }
                // IMPORTANT: send_dl MUST run before sp_complete. GoldenEye reuses its
                // display list buffer as soon as it sees SP done; sending sp_complete early
                // lets the game's scheduler overwrite the DL before RT64 reads it, leading
                // to garbage commands. Process DL first, then signal SP and DP.
                ultramodern::measure_input_latency();

                auto renderer_start = std::chrono::high_resolution_clock::now();
                static int sdl_count = 0;
                sdl_count++;
                if (sdl_count <= 5 || sdl_count % 60 == 0) {
                    fprintf(stderr, "[gfx_thread] send_dl #%d start\n", sdl_count);
                }
                renderer_context->send_dl(&task_action->task);
                if (sdl_count <= 5 || sdl_count % 60 == 0) {
                    fprintf(stderr, "[gfx_thread] send_dl #%d done\n", sdl_count);
                }
                auto renderer_end = std::chrono::high_resolution_clock::now();
                sp_complete();
                dp_complete();
                // printf("Renderer ProcessDList time: %d us\n", static_cast<u32>(std::chrono::duration_cast<std::chrono::microseconds>(renderer_end - renderer_start).count()));
            }
            else if (const auto* swap_action = std::get_if<SwapBuffersAction>(&action)) {
                events_context.vi.current_buffer = events_context.vi.next_buffer;
                renderer_context->update_screen(swap_action->origin);
                display_refresh_rate = renderer_context->get_display_framerate();
                resolution_scale = renderer_context->get_resolution_scale();
            }
            else if (const auto* config_action = std::get_if<UpdateConfigAction>(&action)) {
                auto new_config = ultramodern::renderer::get_graphics_config();
                if (renderer_context->update_config(old_config, new_config)) {
                    old_config = new_config;
                }
            }
        }
    }

    graphics_shutdown_ready.wait();
    renderer_context->shutdown();
}

extern unsigned int VI_STATUS_REG;
extern unsigned int VI_ORIGIN_REG;
extern unsigned int VI_WIDTH_REG;
extern unsigned int VI_INTR_REG;
extern unsigned int VI_V_CURRENT_LINE_REG;
extern unsigned int VI_TIMING_REG;
extern unsigned int VI_V_SYNC_REG;
extern unsigned int VI_H_SYNC_REG;
extern unsigned int VI_LEAP_REG;
extern unsigned int VI_H_START_REG;
extern unsigned int VI_V_START_REG;
extern unsigned int VI_V_BURST_REG;
extern unsigned int VI_X_SCALE_REG;
extern unsigned int VI_Y_SCALE_REG;

#define VI_STATE_BLACK 0x20
#define VI_STATE_REPEATLINE 0x40

uint32_t hstart = 0;
uint32_t vi_origin_offset = 320 * sizeof(uint16_t);
static uint16_t vi_state = 0;

void set_dummy_vi() {
    VI_STATUS_REG = 0x311E;
    VI_WIDTH_REG = 0x140;
    VI_V_SYNC_REG = 0x20D;
    VI_H_SYNC_REG = 0xC15;
    VI_LEAP_REG = 0x0C150C15;
    hstart = 0x006C02EC;
    VI_X_SCALE_REG = 0x200;
    VI_V_CURRENT_LINE_REG = 0x0;
    vi_origin_offset = 0x280;
    VI_Y_SCALE_REG = 0x400;
    VI_V_START_REG = 0x2501FF;
    VI_V_BURST_REG = 0xE0204;
    VI_INTR_REG = 0x2;
}

extern "C" void osViSwapBuffer(RDRAM_ARG PTR(void) frameBufPtr) {
    VI_H_START_REG = hstart;
    if (vi_state & VI_STATE_BLACK) {
        VI_H_START_REG = 0;
    }

    if (vi_state & VI_STATE_REPEATLINE) {
        VI_Y_SCALE_REG = 0;
        VI_ORIGIN_REG = osVirtualToPhysical(frameBufPtr);
    }

    events_context.vi.next_buffer = frameBufPtr;
    events_context.action_queue.enqueue(SwapBuffersAction{ osVirtualToPhysical(frameBufPtr) + vi_origin_offset });
}

extern "C" void osViSetMode(RDRAM_ARG PTR(OSViMode) mode_) {
    OSViMode* mode = TO_PTR(OSViMode, mode_);
    VI_STATUS_REG = mode->comRegs.ctrl;
    VI_WIDTH_REG = mode->comRegs.width;
    // burst
    VI_V_SYNC_REG = mode->comRegs.vSync;
    VI_H_SYNC_REG = mode->comRegs.hSync;
    VI_LEAP_REG = mode->comRegs.leap;
    hstart = mode->comRegs.hStart;
    VI_X_SCALE_REG = mode->comRegs.xScale;
    VI_V_CURRENT_LINE_REG = mode->comRegs.vCurrent;

    // TODO swap these every VI to account for fields changing
    vi_origin_offset = mode->fldRegs[0].origin;
    VI_Y_SCALE_REG = mode->fldRegs[0].yScale;
    VI_V_START_REG = mode->fldRegs[0].vStart;
    VI_V_BURST_REG = mode->fldRegs[0].vBurst;
    VI_INTR_REG = mode->fldRegs[0].vIntr;
}

#define VI_CTRL_TYPE_16             0x00002
#define VI_CTRL_TYPE_32             0x00003
#define VI_CTRL_GAMMA_DITHER_ON     0x00004
#define VI_CTRL_GAMMA_ON            0x00008
#define VI_CTRL_DIVOT_ON            0x00010
#define VI_CTRL_SERRATE_ON          0x00040
#define VI_CTRL_ANTIALIAS_MASK      0x00300
#define VI_CTRL_ANTIALIAS_MODE_1    0x00100
#define VI_CTRL_ANTIALIAS_MODE_2    0x00200
#define VI_CTRL_ANTIALIAS_MODE_3    0x00300
#define VI_CTRL_PIXEL_ADV_MASK      0x01000
#define VI_CTRL_PIXEL_ADV_1         0x01000
#define VI_CTRL_PIXEL_ADV_2         0x02000
#define VI_CTRL_PIXEL_ADV_3         0x03000
#define VI_CTRL_DITHER_FILTER_ON    0x10000

#define OS_VI_GAMMA_ON          0x0001
#define OS_VI_GAMMA_OFF         0x0002
#define OS_VI_GAMMA_DITHER_ON   0x0004
#define OS_VI_GAMMA_DITHER_OFF  0x0008
#define OS_VI_DIVOT_ON          0x0010
#define OS_VI_DIVOT_OFF         0x0020
#define OS_VI_DITHER_FILTER_ON  0x0040
#define OS_VI_DITHER_FILTER_OFF 0x0080

extern "C" void osViSetSpecialFeatures(uint32_t func) {
    if ((func & OS_VI_GAMMA_ON) != 0) {
        VI_STATUS_REG |= VI_CTRL_GAMMA_ON;
    }

    if ((func & OS_VI_GAMMA_OFF) != 0) {
        VI_STATUS_REG &= ~VI_CTRL_GAMMA_ON;
    }

    if ((func & OS_VI_GAMMA_DITHER_ON) != 0) {
        VI_STATUS_REG |= VI_CTRL_GAMMA_DITHER_ON;
    }

    if ((func & OS_VI_GAMMA_DITHER_OFF) != 0) {
        VI_STATUS_REG &= ~VI_CTRL_GAMMA_DITHER_ON;
    }

    if ((func & OS_VI_DIVOT_ON) != 0) {
        VI_STATUS_REG |= VI_CTRL_DIVOT_ON;
    }

    if ((func & OS_VI_DIVOT_OFF) != 0) {
        VI_STATUS_REG &= ~VI_CTRL_DIVOT_ON;
    }

    if ((func & OS_VI_DITHER_FILTER_ON) != 0) {
        VI_STATUS_REG |= VI_CTRL_DITHER_FILTER_ON;
        VI_STATUS_REG &= ~VI_CTRL_ANTIALIAS_MASK;
    }

    if ((func & OS_VI_DITHER_FILTER_OFF) != 0) {
        VI_STATUS_REG &= ~VI_CTRL_DITHER_FILTER_ON;
        //VI_STATUS_REG |= __osViNext->modep->comRegs.ctrl & VI_CTRL_ANTIALIAS_MASK;
    }
}

extern "C" void osViBlack(uint8_t active) {
    if (active) {
        vi_state |= VI_STATE_BLACK;
    } else {
        vi_state &= ~VI_STATE_BLACK;
    }
}

extern "C" void osViRepeatLine(uint8_t active) {
    if (active) {
        vi_state |= VI_STATE_REPEATLINE;
    } else {
        vi_state &= ~VI_STATE_REPEATLINE;
    }
}

extern "C" void osViSetXScale(float scale) {
    if (scale != 1.0f) {
        assert(false);
    }
}

extern "C" void osViSetYScale(float scale) {
    if (scale != 1.0f) {
        assert(false);
    }
}

extern "C" PTR(void) osViGetNextFramebuffer() {
    return events_context.vi.next_buffer;
}

extern "C" PTR(void) osViGetCurrentFramebuffer() {
    return events_context.vi.current_buffer;
}

// Global shadow state, read by RSP::setSegment to remap segment bases into shadow.
// When deep_shadow is active and a segment base falls within [src, src+size), the
// setSegment call is redirected to the shadow equivalent so runtime address
// resolution stays in shadow space.
struct GE_ShadowInfo {
    uint32_t src = 0;    // original address range start
    uint32_t size = 0;   // range size
    uint32_t dest = 0;   // shadow base address
    bool active = false;
};
GE_ShadowInfo g_ge_shadow = {};

void ultramodern::submit_rsp_task(RDRAM_ARG PTR(OSTask) task_) {
    OSTask* task = TO_PTR(OSTask, task_);

    static int submit_count = 0;
    submit_count++;
    // Only log GFX submissions (type=1) to keep noise down
    if (task->t.type == 1 && submit_count <= 10) {
        fprintf(stderr, "[submit_rsp_task GFX] #%d: task_=0x%08X data_ptr=0x%X data_size=%u\n",
            submit_count, (uint32_t)task_, (uint32_t)task->t.data_ptr, task->t.data_size);
    }

    // Send gfx tasks to the graphics action queue
    if (task->t.type == M_GFXTASK) {
        // Copy the DL bytes into a dedicated shadow buffer in a fixed region of RDRAM
        // that the game won't touch. This avoids the race where the game overwrites its
        // single DL buffer before RT64 reads it. We pick a high RDRAM offset well beyond
        // the game's heap (typically < 0x400000 for 4MB, we use 0x600000+).
        static constexpr uint32_t SHADOW_BASE = 0x00600000; // 6MB into RDRAM
        static constexpr uint32_t SHADOW_SLOT_SIZE = 0x00200000; // 2MB per slot (covers sub-DL tree)
        static constexpr int SHADOW_SLOTS = 1;  // Only 1 slot — uses up to end of 8MB RDRAM
        static int shadow_cursor = 0;
        OSTask task_copy = *task;
        uint32_t orig_ptr = task->t.data_ptr & 0x3FFFFFF;
        uint32_t size = task->t.data_size;
        // Drop bogus tasks: data_ptr==0 means dynGetMasterDisplayList returned 0
        // (buffers not initialized) and the game then built commands to RAM[0]
        // overwriting the OS exception region. Nothing good comes of processing this.
        // Also drop any DL pointer below 0x1000 which cannot be a legitimate heap address.
        // Drop tasks whose data_ptr is outside valid RDRAM [0x1000, 0x00800000).
        // Includes VRAM/DMEM-ish addresses like 0x7FC00000 (masks to 0x03FC0000 > 8MB)
        // which come from corrupt OSTask structures and cause OOB memcpy + garbage DL walking.
        if (orig_ptr < 0x1000 || orig_ptr >= 0x00800000) {
            static int subst_log = 0;
            if (++subst_log <= 10) {
                fprintf(stderr, "[submit_rsp_task] SUBST bogus GFX task: data_ptr=0x%08X size=%u — substituting minimal DL\n",
                    (uint32_t)task->t.data_ptr, size);
            }
            // Do NOT short-circuit with sp_complete/dp_complete directly. That bypasses the
            // game-side scheduler's __scTaskComplete path (which stamps gen.type=2 DONE into
            // a stored OSScMsg and forwards it to clientQ). Empirical finding: short-circuit
            // caused the scheduler to enter a pathological state emitting 0x29B (SP done)
            // events forever without ever producing a DONE msg — bossMainloop then never
            // decrements pendingGfx and stalls.
            //
            // Instead, substitute a minimal valid DL (single G_ENDDL command) at a fixed
            // shadow offset so the normal pipeline runs end-to-end. RT64 walks 8 bytes,
            // terminates cleanly, then sp_complete+dp_complete fire from gfx_thread and the
            // scheduler's __scHandleRDP → __scTaskComplete chain delivers a proper DONE.
            static constexpr uint32_t NOOP_DL_ADDR = 0x007F8000; // within RDRAM, outside game heap
            // Write G_ENDDL. GE uses F3DGOLDEN (F3D-derived) where G_ENDDL=0xB8.
            // F3DEX2's 0xDF was not recognized by the walker (saw "OUT OF RDRAM" after
            // 4095 cmds). Use 0xB8 which is the F3D/F3DEX convention.
            // DL format: [opcode(1B) | pad(3B)] [w1(4B)]. BE in RDRAM.
            // With 32-bit host word-swap, writing a u32 at aligned offset directly stores
            // the BE value.
            uint8_t* dl = rdram + NOOP_DL_ADDR;
            *(uint32_t*)(dl + 0) = 0xB8000000u; // G_ENDDL (F3D)
            *(uint32_t*)(dl + 4) = 0x00000000u;
            task_copy.t.data_ptr = 0x80000000u | NOOP_DL_ADDR;
            task_copy.t.data_size = 8;
            events_context.action_queue.enqueue(SpTaskAction{ task_copy });
            return;
        }
        if (size == 0 || size > SHADOW_SLOT_SIZE) {
            // Size unknown or too big - fall back to original pointer (accept race risk)
            events_context.action_queue.enqueue(SpTaskAction{ task_copy });
        } else {
            uint32_t shadow_addr = SHADOW_BASE + (shadow_cursor % SHADOW_SLOTS) * SHADOW_SLOT_SIZE;
            shadow_cursor++;

            // Deep shadow for GE: the main DL references subroutine DLs via G_DL cmds whose
            // target addresses live in the SAME region but outside the `size` bytes we'd
            // normally copy. If we only shadow `size` bytes, the walker follows G_DL into
            // game-side memory that may have been overwritten — walker goes into garbage.
            //
            // Strategy: shadow a LARGER region covering [region_start, region_start + SHADOW_SLOT_SIZE)
            // that contains the main DL plus likely subroutine targets. Then walk the main DL
            // at shadow time and rewrite every G_DL's w1 (if the target falls in the copied
            // range) to point to the shadow equivalent. Addresses outside the range are left
            // alone (they reference external static assets that shouldn't be in danger).
            //
            // Only enabled with GE_DEEP_SHADOW=1 while under test.
            bool deep_shadow = getenv("GE_DEEP_SHADOW") != nullptr;
            uint32_t copy_src = orig_ptr;
            uint32_t copy_size = size;
            uint32_t shadow_dst_offset = 0;
            if (deep_shadow) {
                // Skip deep_shadow entirely if orig_ptr is outside valid RDRAM range.
                // Some game tasks use VRAM-ish addresses (0x7FC00000 masks to 0x03FC0000
                // which is past 8MB RDRAM) — would cause memcpy overflow.
                if (orig_ptr >= 0x00800000) {
                    static int skip_log = 0;
                    if (++skip_log <= 5) {
                        fprintf(stderr, "[deep_shadow] SKIP: orig_ptr 0x%08X outside RDRAM, using plain shadow\n", orig_ptr);
                    }
                    deep_shadow = false;
                } else {
                    uint32_t region_slack_before = 0x10000; // 64KB before
                    uint32_t region_start = (orig_ptr > region_slack_before) ? (orig_ptr - region_slack_before) : 0;
                    region_start &= ~0x7u;
                    uint32_t region_max_size = SHADOW_SLOT_SIZE;
                    if (region_start >= 0x00800000) {
                        // Shouldn't happen now but belt+suspenders
                        region_max_size = 0;
                    } else if (region_start + region_max_size > 0x00800000) {
                        region_max_size = 0x00800000 - region_start;
                    }
                    copy_src = region_start;
                    copy_size = region_max_size;
                    shadow_dst_offset = orig_ptr - region_start;
                }
            }

            memcpy(rdram + shadow_addr, rdram + copy_src, copy_size);
            uint32_t data_ptr_shadow = shadow_addr + shadow_dst_offset;
            task_copy.t.data_ptr = 0x80000000 | data_ptr_shadow;

            // Publish shadow info so RSP::setSegment can remap segment bases.
            g_ge_shadow.src = copy_src;
            g_ge_shadow.size = copy_size;
            g_ge_shadow.dest = shadow_addr;
            g_ge_shadow.active = true;

            // Rewrite G_DL (opcode 0x06) target addresses in the DL tree if deep_shadow.
            // The DL is 8 bytes per command: byte 0 = opcode, bytes 4-7 = w1 (BE in RDRAM).
            // With 32-bit word swap on host, reading a u32 at offset X (4-aligned) returns
            // the BE value natively. So to rewrite w1 we: read u32 at shadow + cmd_off + 4,
            // check if it's in [copy_src, copy_src + copy_size), and rewrite to shadow+delta.
            if (deep_shadow) {
                // Bounds check: ensure shadow_addr + copy_size doesn't exceed RDRAM.
                if (shadow_addr + copy_size > 0x00800000) {
                    fprintf(stderr, "[deep_shadow] ABORT: shadow_addr 0x%08X + copy_size 0x%X overflows RDRAM\n", shadow_addr, copy_size);
                    return;
                }
                // 2026-05-04: Extend rewriter to also snapshot G_MTX (0x01),
                // G_VTX (0x04), and G_MOVEMEM (0x03) targets that fall OUTSIDE the
                // main shadow region. The data referenced by these commands is
                // commonly in the game heap (e.g. matrices at ~0x00264500 while
                // DL is at ~0x00610000) and gets overwritten between submit and
                // walk, producing garbage. We carve out a "data shadow" at the
                // tail of the shadow region for these snapshots.
                uint32_t rewrites = 0;
                uint32_t data_rewrites = 0;
                // Use the last 256KB of the shadow region as the data shadow.
                // Main DL fits in the first SHADOW_SLOT_SIZE - 256KB.
                const uint32_t DATA_SHADOW_OFFSET = (copy_size > 0x40000) ? (copy_size - 0x40000) : 0;
                uint32_t data_cursor = 0;  // grows from DATA_SHADOW_OFFSET
                const uint32_t DATA_SHADOW_LIMIT = 0x40000;  // 256KB cap
                for (uint32_t o = 0; o + 8 <= copy_size && o < DATA_SHADOW_OFFSET; o += 8) {
                    uint8_t* cmd_ptr = rdram + shadow_addr + o;
                    uint32_t w0 = *(uint32_t*)cmd_ptr;
                    uint8_t op = (w0 >> 24) & 0xFF;
                    uint32_t w1 = *(uint32_t*)(cmd_ptr + 4);
                    uint32_t tgt_phys = w1 & 0x00FFFFFF;
                    if (op == 0x06) {  // G_DL — rewrite to inside main shadow
                        if (tgt_phys >= copy_src && tgt_phys < copy_src + copy_size) {
                            uint32_t new_w1 = 0x80000000 | (shadow_addr + (tgt_phys - copy_src));
                            *(uint32_t*)(cmd_ptr + 4) = new_w1;
                            rewrites++;
                        }
                    } else if (op == 0x01 || op == 0x04 || op == 0x03) {  // G_MTX, G_VTX, G_MOVEMEM
                        // Skip if already inside main shadow (DL referenced).
                        if (tgt_phys >= copy_src && tgt_phys < copy_src + copy_size) continue;
                        // Skip out-of-RDRAM addresses.
                        if (tgt_phys >= 0x00800000) continue;
                        // Determine how many bytes to snapshot.
                        // G_MTX: always 64 bytes (per F3D spec).
                        // G_VTX: F3D format w0 = 0x01_NN_LLLL where LLLL is byte length.
                        //        F3DEX2 differs but GE is F3D-style per agent.
                        // G_MOVEMEM: w0 low halfword is length.
                        uint32_t snap_size = 64;  // default for G_MTX
                        if (op == 0x04) {
                            uint32_t len = w0 & 0xFFFF;
                            // Sanity: vertex commands shouldn't be huge. Cap at 32*16=512.
                            snap_size = (len > 0 && len <= 512) ? len : 256;
                        } else if (op == 0x03) {
                            uint32_t len = w0 & 0xFFFF;
                            // Cap at 4KB.
                            snap_size = (len > 0 && len <= 0x1000) ? len : 64;
                        }
                        // Round up to 8-byte alignment.
                        snap_size = (snap_size + 7) & ~7u;
                        if (data_cursor + snap_size > DATA_SHADOW_LIMIT) continue;  // ran out
                        if (tgt_phys + snap_size > 0x00800000) continue;  // OOB source
                        uint32_t dst_off = DATA_SHADOW_OFFSET + data_cursor;
                        // memcpy_s analog: ensure no overlap (data shadow inside main shadow,
                        // source is elsewhere — should be fine).
                        memcpy(rdram + shadow_addr + dst_off, rdram + tgt_phys, snap_size);
                        uint32_t new_w1 = 0x80000000 | (shadow_addr + dst_off);
                        *(uint32_t*)(cmd_ptr + 4) = new_w1;
                        data_cursor += snap_size;
                        data_rewrites++;
                    }
                }
                static int dslog = 0;
                if (++dslog <= 5) {
                    fprintf(stderr, "[deep_shadow #%d] src=[0x%08X..0x%08X) %uKB -> shadow=0x%08X, data_ptr=0x%08X, G_DL rewrites=%u, MTX/VTX/MMEM rewrites=%u (data %ukB used)\n",
                        dslog, copy_src, copy_src + copy_size, copy_size / 1024, shadow_addr, data_ptr_shadow, rewrites, data_rewrites, data_cursor / 1024);
                }
            } else {
                static int copy_log = 0;
                if (++copy_log <= 5) {
                    fprintf(stderr, "[submit_rsp_task] DL shadow copy: 0x%08X -> 0x%08X (%u bytes)\n",
                        orig_ptr, shadow_addr, size);
                }
            }
            events_context.action_queue.enqueue(SpTaskAction{ task_copy });
        }
    }
    // Set all other tasks as the RSP task
    else {
        events_context.sp_task_queue.enqueue(task);
    }
}

void ultramodern::send_si_message(RDRAM_ARG1) {
    osSendMesg(PASS_RDRAM events_context.si.mq, events_context.si.msg, OS_MESG_NOBLOCK);
}

void ultramodern::init_events(RDRAM_ARG ultramodern::renderer::WindowHandle window_handle) {
    moodycamel::LightweightSemaphore gfx_thread_ready;
    moodycamel::LightweightSemaphore task_thread_ready;
    events_context.rdram = rdram;
    events_context.sp.gfx_thread = std::thread{ gfx_thread_func, rdram, &gfx_thread_ready, window_handle };
    events_context.sp.task_thread = std::thread{ task_thread_func, rdram, &task_thread_ready };

    // Wait for the two sp threads to be ready before continuing to prevent the game from
    // running before we're able to handle RSP tasks.
    gfx_thread_ready.wait();
    task_thread_ready.wait();

    ultramodern::renderer::SetupResult setup_result = renderer_setup_result.load();
    if (renderer_setup_result != ultramodern::renderer::SetupResult::Success) {
        auto show_renderer_error = [](const std::string& msg) {
            std::string error_msg = "An error has been encountered on startup: " + msg;

            ultramodern::error_handling::message_box(error_msg.c_str());
        };

        const std::string driver_os_suffix = "\nPlease make sure your GPU drivers and your OS are up to date.";
        switch (renderer_setup_result) {
            case ultramodern::renderer::SetupResult::Success:
                break;
            case ultramodern::renderer::SetupResult::DynamicLibrariesNotFound:
                show_renderer_error("Failed to load dynamic libraries. Make sure the DLLs are next to the recomp executable.");
                break;
            case ultramodern::renderer::SetupResult::InvalidGraphicsAPI:
                show_renderer_error(ultramodern::renderer::get_graphics_api_name(renderer_chosen_api.load()) + " is not supported on this platform. Please select a different graphics API.");
                break;
            case ultramodern::renderer::SetupResult::GraphicsAPINotFound:
                show_renderer_error("Unable to initialize " + ultramodern::renderer::get_graphics_api_name(renderer_chosen_api.load()) + "." + driver_os_suffix);
                break;
            case ultramodern::renderer::SetupResult::GraphicsDeviceNotFound:
                show_renderer_error("Unable to find compatible graphics device." + driver_os_suffix);
                break;
        }
        throw std::runtime_error("Failed to initialize the renderer");
    }

// fprintf(stderr, "[DEBUG] Creating VI thread...\n"); fflush(stderr);
    events_context.vi.thread = std::thread{ vi_thread_func };
// fprintf(stderr, "[DEBUG] VI thread created!\n"); fflush(stderr);
}

void ultramodern::join_event_threads() {
    events_context.sp.gfx_thread.join();
    events_context.vi.thread.join();

    // Send a null RSP task to indicate that the RSP task thread should exit.
    events_context.sp_task_queue.enqueue(nullptr);
    events_context.sp.task_thread.join();
}

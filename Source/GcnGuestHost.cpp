/**
 * @file GcnGuestHost.cpp
 * @brief gcn_platform.h on top of the engine and the C library (see GcnGuestHost.h).
 */

#include "GcnGuestHost.h"

#include "Log.h"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif PLATFORM_DOLPHIN
#include <malloc.h>
#include <ogc/lwp.h>
#include <ogc/system.h>
#include <setjmp.h>
#include <sys/stat.h>
#elif PLATFORM_3DS
#include <pthread.h>
#include <sys/stat.h>
#include <3ds/types.h>
#include <3ds/svc.h>
#include <3ds/os.h>
#include <3ds/allocator/linear.h>
#else
#include <pthread.h>
#include <sys/stat.h>
#include <ucontext.h>
#endif

#if PLATFORM_DOLPHIN
#include <ogc/lwp_watchdog.h>

// The CPU's time base, read directly: the C library's steady_clock went backwards on the
// console (heartbeat times below zero, a mod's 5 s timer reading -0.6 s), and the game's
// own clock (OSGetTime) and its retrace periods come from here.
struct HostClock
{
    using rep = int64_t;
    using period = std::micro;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<HostClock>;
    static constexpr bool is_steady = true;
    static time_point now() { return time_point(duration((rep)ticks_to_microsecs(gettime()))); }
};
#elif PLATFORM_3DS
// The ARM11's system tick (as on the Wii, the console's own counter rather than the C
// library's clock).
struct HostClock
{
    using rep = int64_t;
    using period = std::micro;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<HostClock>;
    static constexpr bool is_steady = true;
    static time_point now()
    {
        return time_point(duration((rep)((double)svcGetSystemTick() * (1000000.0 / SYSCLOCK_ARM11))));
    }
};
#else
using HostClock = std::chrono::steady_clock;
#endif

extern "C" {
#include "Gcn/gcn_disc.h"
#include "Gcn/gcn_gpu.h"
#include "Gcn/gcnw_backend.h"
#include "Gcn/gcnw_module.h"
}

namespace
{
// ---- state (sLock guards everything the two threads share) -----------------------------
std::mutex sLock;
std::condition_variable sWake;
const GcnwModule* sModule = nullptr;
volatile GcnGuestHost::State sState = GcnGuestHost::State::Stopped;
bool sRun = false;          // the game may run (until its next retrace)
bool sFreeRun = false;      // runs on by itself, paced at 60 Hz (else one frame per StepFrame)
bool sHold = false;         // a script wants the game parked (until ReleaseHold)
bool sStopRequested = false;
HostClock::time_point sDeadline; // game thread: when the next frame is due
bool sThreadDone = false;
uint32_t sFrameCount = 0;
double sGameMs = 0.0, sEngineMs = 0.0; // heartbeat (consoles): time in game frames, between them

// The game's pictures, triple buffered: the game thread fills sFrames[sFrameWrite] when the game
// finished a new one (a display copy), then publishes it as sFrameReady; the main thread reads
// sFrameRead (the newest when it asked) in place until it asks again. The writer never touches
// the ready or the read buffer, so neither side copies under the lock.
struct FrameBuffer
{
    std::vector<uint8_t> px;
    int w = 0, h = 0, scale = 1;
};
FrameBuffer sFrames[3];
int sFrameWrite = 0, sFrameReady = -1, sFrameRead = -1;
uint32_t sFrameSerial = 0;
uint32_t sFrameCopies = 0xFFFFFFFFu; // gcn_gpu_display_copies() of the last picture taken
std::vector<std::string> sLogLines;

GcnPad sPads[4];
uint16_t sHeldButtons = 0;
int sHeldFrames = 0;
bool sRumble[4];

const uint32_t kAudioRing = 65536; // frames, power of two
int16_t sAudio[kAudioRing * 2];
uint32_t sAudioWrite = 0, sAudioRead = 0, sAudioRate = 32000;

GcnDisc* sDisc = nullptr;
std::string sSaveDir;
FILE* sCard = nullptr;
const uint32_t kCardBytes = 2u << 20;

#if PLATFORM_WINDOWS
HANDLE sThread = nullptr;
const size_t kGameStack = 16u << 20;
#elif PLATFORM_DOLPHIN
lwp_t sThread = LWP_THREAD_NULL;
bool sThreadValid = false;
void* sThreadStack = nullptr;
jmp_buf sThreadExit; // the game thread leaves through here (no thread exit call in libogc)
const size_t kGameStack = 1u << 20; // the guest's main thread runs on it; memory is short
#else
pthread_t sThread;
bool sThreadValid = false;
#if PLATFORM_3DS
const size_t kGameStack = 1u << 20; // the guest's main thread runs on it; memory is short
#else
const size_t kGameStack = 16u << 20;
#endif
#endif

void QueueLog(const char* text)
{
    std::lock_guard<std::mutex> guard(sLock);
    if (sLogLines.size() < 4096)
    {
        sLogLines.push_back(text);
    }
}

// Leaves the game thread from whatever coroutine runs (guest threads are coroutines of it).
[[noreturn]] void ExitGameThread()
{
    {
        std::lock_guard<std::mutex> guard(sLock);
        sThreadDone = true;
    }
    sWake.notify_all();
#if PLATFORM_WINDOWS
    ExitThread(0);
#elif PLATFORM_DOLPHIN
    longjmp(sThreadExit, 1);
#else
    pthread_exit(nullptr);
#endif
    for (;;)
    {
    }
}

uint32_t sRetracesSincePark = 0; // consoles: see gcnp_retrace

// Parks the game thread until the main thread lets it run or stops it.
void Park()
{
    std::unique_lock<std::mutex> lock(sLock);
    sRetracesSincePark = 0;
    sRun = false;
    sWake.notify_all();
    sWake.wait(lock, [] { return sRun || sStopRequested; });
    const bool stop = sStopRequested;
    lock.unlock();
    if (stop)
    {
        ExitGameThread();
    }
}

void GameMain()
{
#if PLATFORM_DOLPHIN
    // one GPU command FIFO for the game and the engine: the game may only issue commands
    // while the engine waits for it, so even the boot waits for the first StepFrame
    Park();
#endif
    gcnp_coro_current();
    gcnw_run();
    {
        std::lock_guard<std::mutex> guard(sLock);
        sState = GcnGuestHost::State::Exited;
        sLogLines.push_back("gcn: the game's main() returned");
    }
    for (;;)
    {
        Park();
    }
}

#if PLATFORM_WINDOWS
DWORD WINAPI ThreadMain(LPVOID)
{
    GameMain();
    return 0;
}
#elif PLATFORM_DOLPHIN
void* ThreadMain(void*)
{
    if (setjmp(sThreadExit) == 0)
    {
        GameMain();
    }
    return nullptr;
}
#else
void* ThreadMain(void*)
{
    GameMain();
    return nullptr;
}
#endif

FILE* CardFile(int chan)
{
    if (chan != 0 || sSaveDir.empty())
    {
        return nullptr;
    }
    if (sCard != nullptr)
    {
        return sCard;
    }
#if PLATFORM_WINDOWS
    CreateDirectoryA(sSaveDir.c_str(), nullptr);
#else
    mkdir(sSaveDir.c_str(), 0755);
#endif
    const std::string path = sSaveDir + "/card_a.raw";
    sCard = fopen(path.c_str(), "r+b");
    if (sCard == nullptr)
    {
        static const char zero[4096] = {};
        sCard = fopen(path.c_str(), "w+b");
        if (sCard == nullptr)
        {
            return nullptr;
        }
        for (uint32_t i = 0; i < kCardBytes; i += sizeof(zero))
        {
            fwrite(zero, 1, sizeof(zero), sCard);
        }
        fflush(sCard);
    }
    return sCard;
}

bool Seek(FILE* f, uint64_t offset)
{
#if PLATFORM_WINDOWS
    return _fseeki64(f, (long long)offset, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)offset, SEEK_SET) == 0;
#endif
}
}

// ---- gcn_platform.h --------------------------------------------------------------------------
extern "C" {

void gcnp_log(const char* text)
{
    QueueLog(text);
}

void gcnp_crashed(void)
{
    {
        std::lock_guard<std::mutex> guard(sLock);
        sState = GcnGuestHost::State::Crashed;
        sLogLines.push_back("gcn: the game stopped after an error (see above)");
    }
    for (;;)
    {
        Park();
    }
}

uint64_t gcnp_time_us(void)
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               HostClock::now().time_since_epoch())
        .count();
}

uint32_t gcnp_disc_read(void* dst, uint64_t offset, uint32_t size)
{
    return gcn_disc_read(sDisc, dst, offset, size);
}

uint64_t gcnp_disc_size(void)
{
    return gcn_disc_size(sDisc);
}

uint32_t gcnp_retrace(void)
{
    uint32_t periods = 1;
    int w = 0, h = 0;
    const uint32_t* pixels = gcn_gpu_frame(&w, &h);
    const uint32_t copies = gcn_gpu_display_copies();
    if (pixels != nullptr && w > 0 && h > 0 && copies != sFrameCopies)
    {
        // a new picture (the game finished one since the last retrace): into the write buffer,
        // which only this thread touches; rows are gcn_gpu_frame_stride() pixels apart in the GPU's
        FrameBuffer& fb = sFrames[sFrameWrite];
        const size_t stride = size_t(gcn_gpu_frame_stride());
        fb.px.resize(size_t(w) * size_t(h) * 4);
        if (stride == size_t(w))
        {
            memcpy(fb.px.data(), pixels, fb.px.size());
        }
        else
        {
            for (int y = 0; y < h; ++y)
            {
                memcpy(&fb.px[size_t(y) * size_t(w) * 4], pixels + size_t(y) * stride, size_t(w) * 4);
            }
        }
        fb.w = w;
        fb.h = h;
        fb.scale = gcn_gpu_frame_scale();
        sFrameCopies = copies;
        std::lock_guard<std::mutex> guard(sLock);
        sFrameReady = sFrameWrite;
        sFrameSerial++;
        for (int i = 0; i < 3; ++i)
        {
            if (i != sFrameReady && i != sFrameRead)
            {
                sFrameWrite = i;
                break;
            }
        }
    }
    {
        std::lock_guard<std::mutex> guard(sLock);
        sFrameCount++;
#if PLATFORM_DOLPHIN || PLATFORM_3DS
        {
            // heartbeat: frames and their pace (the console has no other window into it)
            static auto lastBeat = HostClock::now();
            static uint32_t lastFrames = 0;
            const auto now = HostClock::now();
            const double secs = std::chrono::duration<double>(now - lastBeat).count();
            if (secs >= 2.0 || sFrameCount == 1)
            {
                // unknown commands mean the parser lost its place in the stream (a vertex size
                // that differs from the hardware's), which also desynchronises the real GPU
                const GcnGpuStats* gpu = gcn_gpu_last_frame_stats();
                const uint32_t frames = sFrameCount - lastFrames;
                uint32_t feedUs = 0, feedCalls = 0;
                gcn_gpu_take_feed(&feedUs, &feedCalls);
                char line[256];
                snprintf(line, sizeof(line),
                         "gcn: frame %u, %.1f frames/s, %u prims, %u lists, %u KB of commands, %u unknown; "
                         "per frame: game %.1f ms (of it feeding the GPU %.1f ms, %u pipe writes), engine %.1f ms",
                         (unsigned)sFrameCount, sFrameCount == 1 ? 0.0 : frames / (secs > 0.0 ? secs : 1.0),
                         (unsigned)gpu->primitives, (unsigned)gpu->display_lists, (unsigned)(gpu->fifo_bytes >> 10),
                         (unsigned)gpu->unknown_commands,
                         frames ? sGameMs / frames : 0.0, frames ? feedUs / 1000.0 / frames : 0.0,
                         frames ? feedCalls / frames : 0u, frames ? sEngineMs / frames : 0.0);
                sGameMs = sEngineMs = 0.0;
                sLogLines.push_back(line);
                char calls[384];
                gcnw_take_import_profile(calls, sizeof(calls), frames);
                sLogLines.push_back(std::string("gcn: host calls per frame: ") + calls);
                lastBeat = now;
                lastFrames = sFrameCount;
            }
        }
#endif
        if (sHeldFrames > 0 && --sHeldFrames == 0)
        {
            sHeldButtons = 0;
        }
    }
    {
        // free running: wait until the frame is due (60 Hz; a game slower than that runs
        // as fast as it can), unless the main thread wants it parked meanwhile
        std::unique_lock<std::mutex> lock(sLock);
        if (sFreeRun && !sHold)
        {
            // a frame that took longer than 1/60 s missed retraces: the game is told, so
            // its clock and audio keep real time
            const auto now = HostClock::now();
            const auto period = std::chrono::microseconds(16667);
            if (now > sDeadline + period)
            {
                periods = (uint32_t)((now - sDeadline) / period) + 1;
                if (periods > 4) periods = 4;
            }
            sDeadline += period * periods;
            if (now > sDeadline + std::chrono::milliseconds(100) || now + std::chrono::seconds(1) < sDeadline)
            {
                sDeadline = now; // far behind (or the clock jumped): do not try to catch up
            }
            sWake.wait_until(lock, sDeadline, [] { return sStopRequested || sHold || !sFreeRun; });
        }
        if (sFreeRun && !sHold && !sStopRequested)
        {
            return periods; // on to the next frame
        }
        if (!sFreeRun)
        {
            // stepped by the engine (Wii, or paused): the game is told the 1/60 s periods
            // that really passed, counted against a running total so the remainders carry
            // over (rounded per frame, they ran the game's clock - and cutscenes against
            // their streamed audio - up to 15% off); ahead of real time it waits for the
            // next one, as on the console
            static HostClock::time_point due;
            const auto period = std::chrono::microseconds(16667);
            auto now = HostClock::now();
            if (due.time_since_epoch().count() == 0)
            {
                due = now;
            }
            if (now < due)
            {
                sWake.wait_until(lock, due, [] { return sStopRequested; });
                now = HostClock::now();
            }
            periods = 1 + (uint32_t)((now - due) / period);
            if (periods > 4)
            {
                periods = 4;
                due = now + period; // far behind (loading): carry on from now
            }
            else
            {
                due += period * periods;
            }
        }
#if PLATFORM_DOLPHIN
        // the game's picture stays in the GPU's frame buffer for the engine to draw over and
        // show (gcn_gpu.c drops its copies to the external frame buffer): the engine's turn
        // comes when a picture is finished (gcnp_frame_done), not at a retrace in the middle
        // of one; only after a while without pictures (loading) at a retrace
        if (!sFreeRun && !sStopRequested && sRetracesSincePark + 1 < 8)
        {
            ++sRetracesSincePark;
            return periods;
        }
#endif
    }
    Park();
    return periods;
}

#if PLATFORM_DOLPHIN
void gcnp_frame_done(void)
{
    Park();
}
#endif

int gcnp_pad(int port, GcnPad* pad)
{
    if (port < 0 || port > 3)
    {
        return 0;
    }
    std::lock_guard<std::mutex> guard(sLock);
    *pad = sPads[port];
    if (port == 0)
    {
        pad->buttons |= sHeldButtons;
        if (sHeldButtons)
        {
            pad->connected = 1;
        }
    }
    return pad->connected;
}

void gcnp_rumble(int port, int on)
{
    if (port >= 0 && port < 4)
    {
        sRumble[port] = on != 0;
    }
}

uint32_t gcnp_card_size(int chan)
{
    return CardFile(chan) != nullptr ? kCardBytes : 0;
}

uint32_t gcnp_card_io(int chan, void* buf, uint32_t offset, uint32_t size, int write)
{
    FILE* f = CardFile(chan);

    if (f == nullptr || offset > kCardBytes || size > kCardBytes - offset || !Seek(f, offset))
    {
        return 0;
    }
    const size_t n = write ? fwrite(buf, 1, size, f) : fread(buf, 1, size, f);
    if (write)
    {
        fflush(f);
    }
    return (uint32_t)n;
}

void gcnp_audio(const int16_t* samples, uint32_t frames, uint32_t rate)
{
    std::lock_guard<std::mutex> guard(sLock);
    sAudioRate = rate;
    for (uint32_t i = 0; i < frames; ++i)
    {
        if (sAudioWrite - sAudioRead >= kAudioRing)
        {
            sAudioRead++; // nobody plays it: drop the oldest
        }
        sAudio[(sAudioWrite % kAudioRing) * 2] = samples[i * 2];
        sAudio[(sAudioWrite % kAudioRing) * 2 + 1] = samples[i * 2 + 1];
        sAudioWrite++;
    }
}

// ---- coroutines -------------------------------------------------------------------------
#if PLATFORM_WINDOWS
struct GcnpCoro
{
    void* fiber;
    void (*fn)(void*);
    void* arg;
    bool owned;
};

static thread_local GcnpCoro* sCurrentCoro = nullptr;
static GcnpCoro sMainCoro;
static std::vector<GcnpCoro*> sCoros;

GcnpCoro* gcnp_coro_current(void)
{
    if (sCurrentCoro == nullptr)
    {
        sMainCoro.fiber = ConvertThreadToFiber(nullptr);
        sMainCoro.owned = false;
        sCurrentCoro = &sMainCoro;
    }
    return sCurrentCoro;
}

static void WINAPI FiberMain(void* p)
{
    GcnpCoro* c = (GcnpCoro*)p;
    c->fn(c->arg);
}

GcnpCoro* gcnp_coro_create(void (*fn)(void*), void* arg, size_t stack_bytes)
{
    GcnpCoro* c = (GcnpCoro*)calloc(1, sizeof(GcnpCoro));
    c->fn = fn;
    c->arg = arg;
    c->owned = true;
    c->fiber = CreateFiber(stack_bytes, FiberMain, c);
    sCoros.push_back(c);
    return c;
}

void gcnp_coro_switch(GcnpCoro* to)
{
    sCurrentCoro = to;
    SwitchToFiber(to->fiber);
}

void gcnp_coro_destroy(GcnpCoro* coro)
{
    for (size_t i = 0; i < sCoros.size(); ++i)
    {
        if (sCoros[i] == coro)
        {
            sCoros.erase(sCoros.begin() + i);
            break;
        }
    }
    DeleteFiber(coro->fiber);
    free(coro);
}

static void FreeCoroutines()
{
    for (GcnpCoro* c : sCoros)
    {
        DeleteFiber(c->fiber);
        free(c);
    }
    sCoros.clear();
}
#elif PLATFORM_DOLPHIN
// PowerPC EABI: a coroutine is its callee-saved state (r1, LR, CR, r14-r31, f14-f31),
// switched by gcn_ppc_swap. The layout is fixed: the assembly below uses these offsets.
struct GcnpCoro
{
    uint32_t r1;         // 0
    uint32_t lr;         // 4
    uint32_t cr;         // 8
    uint32_t gpr[18];    // 12: r14-r31
    uint32_t pad;        // 84
    double fpr[18];      // 88: f14-f31
    void* stack;
    void (*fn)(void*);
    void* arg;
};
static_assert(offsetof(GcnpCoro, fpr) == 88, "gcn_ppc_swap offsets");
}

extern "C" void gcn_ppc_swap(GcnpCoro* from, GcnpCoro* to);
extern "C" void gcn_ppc_start(void);
extern "C" void gcn_ppc_entry(GcnpCoro* c)
{
    c->fn(c->arg); // a guest thread never returns from it (it switches away for good)
    for (;;)
    {
    }
}

asm(R"(
    .text
    .align 2
    .globl gcn_ppc_swap
gcn_ppc_swap:
    mflr 0
    stw 1, 0(3)
    stw 0, 4(3)
    mfcr 0
    stw 0, 8(3)
    stmw 14, 12(3)
    stfd 14, 88(3)
    stfd 15, 96(3)
    stfd 16, 104(3)
    stfd 17, 112(3)
    stfd 18, 120(3)
    stfd 19, 128(3)
    stfd 20, 136(3)
    stfd 21, 144(3)
    stfd 22, 152(3)
    stfd 23, 160(3)
    stfd 24, 168(3)
    stfd 25, 176(3)
    stfd 26, 184(3)
    stfd 27, 192(3)
    stfd 28, 200(3)
    stfd 29, 208(3)
    stfd 30, 216(3)
    stfd 31, 224(3)
    lwz 1, 0(4)
    lwz 0, 4(4)
    mtlr 0
    lwz 0, 8(4)
    mtcrf 0xff, 0
    lmw 14, 12(4)
    lfd 14, 88(4)
    lfd 15, 96(4)
    lfd 16, 104(4)
    lfd 17, 112(4)
    lfd 18, 120(4)
    lfd 19, 128(4)
    lfd 20, 136(4)
    lfd 21, 144(4)
    lfd 22, 152(4)
    lfd 23, 160(4)
    lfd 24, 168(4)
    lfd 25, 176(4)
    lfd 26, 184(4)
    lfd 27, 192(4)
    lfd 28, 200(4)
    lfd 29, 208(4)
    lfd 30, 216(4)
    lfd 31, 224(4)
    blr

    .globl gcn_ppc_start
gcn_ppc_start:
    mr 3, 14
    bl gcn_ppc_entry
    b gcn_ppc_start
)");

namespace
{
GcnpCoro* sCurrentCoro = nullptr; // only the game thread switches
GcnpCoro sMainCoro;
std::vector<GcnpCoro*> sCoros;
}

GcnpCoro* gcnp_coro_current(void)
{
    if (sCurrentCoro == nullptr)
    {
        sCurrentCoro = &sMainCoro;
    }
    return sCurrentCoro;
}

GcnpCoro* gcnp_coro_create(void (*fn)(void*), void* arg, size_t stack_bytes)
{
    GcnpCoro* c = (GcnpCoro*)memalign(32, sizeof(GcnpCoro));
    if (c == nullptr) return nullptr;
    memset(c, 0, sizeof(*c));
    c->fn = fn;
    c->arg = arg;
    c->stack = memalign(32, stack_bytes);
    if (c->stack == nullptr)
    {
        free(c);
        return nullptr;
    }
    // the first switch "returns" into gcn_ppc_start with r14 = the coroutine, on a fresh
    // stack whose first frame has an empty back chain
    uint32_t top = ((uint32_t)(uintptr_t)c->stack + (uint32_t)stack_bytes - 64u) & ~15u;
    *(uint32_t*)(uintptr_t)top = 0;
    c->r1 = top;
    c->lr = (uint32_t)(uintptr_t)gcn_ppc_start;
    c->gpr[0] = (uint32_t)(uintptr_t)c;
    sCoros.push_back(c);
    return c;
}

void gcnp_coro_switch(GcnpCoro* to)
{
    GcnpCoro* from = gcnp_coro_current();
    if (from == to) return;
    sCurrentCoro = to;
    gcn_ppc_swap(from, to);
}

void gcnp_coro_destroy(GcnpCoro* coro)
{
    for (size_t i = 0; i < sCoros.size(); ++i)
    {
        if (sCoros[i] == coro)
        {
            sCoros.erase(sCoros.begin() + i);
            break;
        }
    }
    free(coro->stack);
    free(coro);
}

namespace
{
void FreeCoroutines()
{
    for (GcnpCoro* c : sCoros)
    {
        free(c->stack);
        free(c);
    }
    sCoros.clear();
    sCurrentCoro = nullptr;
}
}
extern "C" {
#elif PLATFORM_3DS
// ARM (AAPCS, hard float): a coroutine is its callee-saved state (sp, lr, r4-r11, d8-d15),
// switched by gcn_arm_swap. The layout is fixed: the assembly below uses these offsets.
struct GcnpCoro
{
    uint32_t sp;      // 0
    uint32_t lr;      // 4
    uint32_t r[8];    // 8: r4-r11
    double d[8];      // 40: d8-d15
    void* stack;
    void (*fn)(void*);
    void* arg;
};
static_assert(offsetof(GcnpCoro, d) == 40, "gcn_arm_swap offsets");
}

extern "C" void gcn_arm_swap(GcnpCoro* from, GcnpCoro* to);
extern "C" void gcn_arm_start(void);
extern "C" void gcn_arm_entry(GcnpCoro* c)
{
    c->fn(c->arg); // a guest thread never returns from it (it switches away for good)
    for (;;)
    {
    }
}

asm(R"(
    .text
    .arm
    .align 2
    .globl gcn_arm_swap
    .type gcn_arm_swap, %function
gcn_arm_swap:
    str sp, [r0, #0]
    str lr, [r0, #4]
    add r2, r0, #8
    stmia r2, {r4-r11}
    add r2, r0, #40
    vstmia r2, {d8-d15}
    ldr sp, [r1, #0]
    ldr lr, [r1, #4]
    add r2, r1, #8
    ldmia r2, {r4-r11}
    add r2, r1, #40
    vldmia r2, {d8-d15}
    bx lr

    .globl gcn_arm_start
    .type gcn_arm_start, %function
gcn_arm_start:
    mov r0, r4
    bl gcn_arm_entry
    b gcn_arm_start
)");

namespace
{
GcnpCoro* sCurrentCoro = nullptr; // only the game thread switches
GcnpCoro sMainCoro;
std::vector<GcnpCoro*> sCoros;
}

GcnpCoro* gcnp_coro_current(void)
{
    if (sCurrentCoro == nullptr)
    {
        sCurrentCoro = &sMainCoro;
    }
    return sCurrentCoro;
}

GcnpCoro* gcnp_coro_create(void (*fn)(void*), void* arg, size_t stack_bytes)
{
    GcnpCoro* c = (GcnpCoro*)calloc(1, sizeof(GcnpCoro));
    if (c == nullptr) return nullptr;
    c->fn = fn;
    c->arg = arg;
    c->stack = malloc(stack_bytes);
    if (c->stack == nullptr)
    {
        free(c);
        return nullptr;
    }
    // the first switch "returns" into gcn_arm_start with r4 = the coroutine, on a fresh
    // 8-byte aligned stack
    c->sp = ((uint32_t)(uintptr_t)c->stack + (uint32_t)stack_bytes - 16u) & ~7u;
    c->lr = (uint32_t)(uintptr_t)gcn_arm_start;
    c->r[0] = (uint32_t)(uintptr_t)c;
    sCoros.push_back(c);
    return c;
}

void gcnp_coro_switch(GcnpCoro* to)
{
    GcnpCoro* from = gcnp_coro_current();
    if (from == to) return;
    sCurrentCoro = to;
    gcn_arm_swap(from, to);
}

void gcnp_coro_destroy(GcnpCoro* coro)
{
    for (size_t i = 0; i < sCoros.size(); ++i)
    {
        if (sCoros[i] == coro)
        {
            sCoros.erase(sCoros.begin() + i);
            break;
        }
    }
    free(coro->stack);
    free(coro);
}

namespace
{
void FreeCoroutines()
{
    for (GcnpCoro* c : sCoros)
    {
        free(c->stack);
        free(c);
    }
    sCoros.clear();
    sCurrentCoro = nullptr;
}
}
extern "C" {
#else
struct GcnpCoro
{
    ucontext_t ctx;
    void* stack;
    void (*fn)(void*);
    void* arg;
};

static thread_local GcnpCoro* sCurrentCoro = nullptr;
static GcnpCoro sMainCoro;
static std::vector<GcnpCoro*> sCoros;

GcnpCoro* gcnp_coro_current(void)
{
    if (sCurrentCoro == nullptr)
    {
        sCurrentCoro = &sMainCoro;
    }
    return sCurrentCoro;
}

static void CoroMain(unsigned int hi, unsigned int lo)
{
    GcnpCoro* c = (GcnpCoro*)(((uintptr_t)hi << 32) | (uintptr_t)lo);
    c->fn(c->arg);
}

GcnpCoro* gcnp_coro_create(void (*fn)(void*), void* arg, size_t stack_bytes)
{
    GcnpCoro* c = (GcnpCoro*)calloc(1, sizeof(GcnpCoro));
    c->fn = fn;
    c->arg = arg;
    c->stack = malloc(stack_bytes);
    getcontext(&c->ctx);
    c->ctx.uc_stack.ss_sp = c->stack;
    c->ctx.uc_stack.ss_size = stack_bytes;
    c->ctx.uc_link = nullptr;
    const uintptr_t p = (uintptr_t)c;
    makecontext(&c->ctx, (void (*)())CoroMain, 2, (unsigned int)(p >> 32), (unsigned int)p);
    sCoros.push_back(c);
    return c;
}

void gcnp_coro_switch(GcnpCoro* to)
{
    GcnpCoro* from = gcnp_coro_current();
    sCurrentCoro = to;
    swapcontext(&from->ctx, &to->ctx);
}

void gcnp_coro_destroy(GcnpCoro* coro)
{
    for (size_t i = 0; i < sCoros.size(); ++i)
    {
        if (sCoros[i] == coro)
        {
            sCoros.erase(sCoros.begin() + i);
            break;
        }
    }
    free(coro->stack);
    free(coro);
}

static void FreeCoroutines()
{
    for (GcnpCoro* c : sCoros)
    {
        free(c->stack);
        free(c);
    }
    sCoros.clear();
}
#endif
}

// ---- GcnGuestHost --------------------------------------------------------------------------
bool GcnGuestHost::Start(const GcnwModule* module, const std::string& discPath, const std::string& saveDir)
{
    Stop();
#if PLATFORM_DOLPHIN
    {
        char line[128];
        snprintf(line, sizeof(line), "gcn: free memory before the game: MEM1 %u KB, MEM2 %u KB",
                 (unsigned)(SYS_GetArena1Size() >> 10), (unsigned)(SYS_GetArena2Size() >> 10));
        sLogLines.push_back(line);
    }
#elif PLATFORM_3DS
    {
        char line[128];
        snprintf(line, sizeof(line), "gcn: free memory before the game: application %u KB, linear %u KB",
                 (unsigned)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10), (unsigned)(linearSpaceFree() >> 10));
        sLogLines.push_back(line);
    }
#endif
    sDisc = gcn_disc_open(discPath.c_str());
    if (sDisc == nullptr)
    {
        LogError("GcnPlayer: cannot open the disc %s", discPath.c_str());
        return false;
    }
    sSaveDir = saveDir;
    if (!gcnw_instantiate(module))
    {
        LogError("GcnPlayer: cannot set up %s", module->title);
        Stop();
        return false;
    }
    sModule = module;
    memset(sPads, 0, sizeof(sPads));
    sPads[0].connected = 1;
    sHeldButtons = 0;
    sHeldFrames = 0;
    sAudioWrite = sAudioRead = 0;
    sFrameCount = 0;
    sFrameSerial = 0;
    sFrameWrite = 0;
    sFrameReady = -1;
    sFrameRead = -1;
    sFrameCopies = 0xFFFFFFFFu;
#if PLATFORM_DOLPHIN
    sRun = false; // waits for the first StepFrame (see GameMain)
#else
    sRun = true; // boots until its first frame
#endif
    sStopRequested = false;
    sThreadDone = false;
    sState = State::Running;
#if PLATFORM_WINDOWS
    sThread = CreateThread(nullptr, kGameStack, ThreadMain, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (sThread == nullptr)
#elif PLATFORM_DOLPHIN
    if (sThreadStack == nullptr)
    {
        sThreadStack = memalign(32, kGameStack);
    }
    sThreadValid = sThreadStack != nullptr &&
                   LWP_CreateThread(&sThread, ThreadMain, nullptr, sThreadStack, kGameStack, 64) == 0; // the main thread's priority
    if (!sThreadValid)
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kGameStack);
    sThreadValid = pthread_create(&sThread, &attr, ThreadMain, nullptr) == 0;
    pthread_attr_destroy(&attr);
    if (!sThreadValid)
#endif
    {
        LogError("GcnPlayer: cannot start the game thread");
        sState = State::Crashed;
        Stop();
        return false;
    }
    return true;
}

void GcnGuestHost::Stop()
{
    bool hadThread;
#if PLATFORM_WINDOWS
    hadThread = sThread != nullptr;
#elif PLATFORM_DOLPHIN
    hadThread = sThreadValid;
#else
    hadThread = sThreadValid;
#endif
    if (hadThread)
    {
        {
            std::unique_lock<std::mutex> lock(sLock);
            sStopRequested = true;
            sWake.notify_all();
            // the game leaves at its next retrace; one that hangs is cut off
            sWake.wait_for(lock, std::chrono::seconds(3), [] { return sThreadDone; });
        }
#if PLATFORM_WINDOWS
        if (WaitForSingleObject(sThread, 2000) != WAIT_OBJECT_0)
        {
            TerminateThread(sThread, 1);
        }
        CloseHandle(sThread);
        sThread = nullptr;
#elif PLATFORM_DOLPHIN
        if (sThreadDone)
        {
            LWP_JoinThread(sThread, nullptr);
        }
        // else: a game that hangs keeps its thread (libogc can't cancel one); it is parked
        sThreadValid = false;
#else
        if (sThreadDone)
        {
            pthread_join(sThread, nullptr);
        }
        else
        {
            pthread_cancel(sThread);
        }
        sThreadValid = false;
#endif
        FreeCoroutines();
    }
    if (sModule != nullptr)
    {
        gcnw_free();
        sModule = nullptr;
    }
    if (sDisc != nullptr)
    {
        gcn_disc_close(sDisc);
        sDisc = nullptr;
    }
    if (sCard != nullptr)
    {
        fclose(sCard);
        sCard = nullptr;
    }
    FlushLog();
    sState = State::Stopped;
}

GcnGuestHost::State GcnGuestHost::GetState()
{
    return sState;
}

const GcnwModule* GcnGuestHost::GetModule()
{
    return sModule;
}

void GcnGuestHost::SetFreeRun(bool on)
{
    std::lock_guard<std::mutex> guard(sLock);
    if (sFreeRun == on)
    {
        return;
    }
    sFreeRun = on;
    if (on)
    {
        sDeadline = HostClock::now();
        if (!sHold && !sRun)
        {
            sRun = true;
        }
    }
    sWake.notify_all();
}

bool GcnGuestHost::Hold(int timeoutMs)
{
    std::unique_lock<std::mutex> lock(sLock);
    if (sModule == nullptr || sThreadDone)
    {
        return false;
    }
    sHold = true;
    sWake.notify_all();
    return sWake.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] { return !sRun || sThreadDone; }) && !sThreadDone;
}

void GcnGuestHost::ReleaseHold()
{
    std::lock_guard<std::mutex> guard(sLock);
    if (!sHold)
    {
        return;
    }
    sHold = false;
    if (sFreeRun && !sRun)
    {
        sRun = true;
        sDeadline = HostClock::now();
        sWake.notify_all();
    }
}

bool GcnGuestHost::StepFrame(int timeoutMs)
{
    std::unique_lock<std::mutex> lock(sLock);
    if (sModule == nullptr || sThreadDone)
    {
        return false;
    }
    if (!sRun)
    {
        sRun = true;
        sWake.notify_all();
    }
#if PLATFORM_DOLPHIN
    // the engine must not draw while the game is inside a frame (they share the GPU's
    // command FIFO): wait for the end of the frame however long it takes, saying so
    {
        static auto lastEnd = HostClock::now();
        const auto start = HostClock::now();
        struct Timer
        {
            HostClock::time_point start;
            HostClock::time_point& lastEnd;
            ~Timer()
            {
                const auto end = HostClock::now();
                sEngineMs += std::chrono::duration<double, std::milli>(start - lastEnd).count();
                sGameMs += std::chrono::duration<double, std::milli>(end - start).count();
                lastEnd = end;
            }
        } timer{start, lastEnd};
        int waited = 0;
        while (!sWake.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] { return !sRun || sThreadDone; }))
        {
            waited += timeoutMs;
            char line[96];
            snprintf(line, sizeof(line), "gcn: frame %u still running after %d ms", (unsigned)sFrameCount, waited);
            sLogLines.push_back(line);
        }
        return true;
    }
#endif
    const bool done = sWake.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] { return !sRun || sThreadDone; });
    if (!done)
    {
        static int said = 0;
        if (said < 5)
        {
            ++said;
            char line[96];
            snprintf(line, sizeof(line), "gcn: no frame within %d ms (frame %u)", timeoutMs, (unsigned)sFrameCount);
            sLogLines.push_back(line);
        }
    }
    return done;
}

void GcnGuestHost::SetPad(int port, const GcnPad& pad)
{
    if (port >= 0 && port < 4)
    {
        std::lock_guard<std::mutex> guard(sLock);
        sPads[port] = pad;
    }
}

void GcnGuestHost::HoldButtons(uint16_t buttons, int frames)
{
    std::lock_guard<std::mutex> guard(sLock);
    sHeldButtons = buttons;
    sHeldFrames = frames;
}

bool GcnGuestHost::GetRumble(int port)
{
    return port >= 0 && port < 4 && sRumble[port];
}

bool GcnGuestHost::GetFrame(uint32_t& lastSerial, const uint8_t*& rgba, int& width, int& height, int* scale)
{
    std::lock_guard<std::mutex> guard(sLock);

    if (sFrameReady < 0 || sFrameSerial == lastSerial)
    {
        return false;
    }
    // the newest picture, read in place: the game thread leaves it alone until the next call
    sFrameRead = sFrameReady;
    const FrameBuffer& fb = sFrames[sFrameRead];
    lastSerial = sFrameSerial;
    rgba = fb.px.data();
    width = fb.w;
    height = fb.h;
    if (scale != nullptr)
    {
        *scale = fb.scale;
    }
    return true;
}

uint32_t GcnGuestHost::PeekAudio(const int16_t*& frames, uint32_t maxFrames, uint32_t& rate)
{
    std::lock_guard<std::mutex> guard(sLock);
    uint32_t available = sAudioWrite - sAudioRead;
    const uint32_t untilWrap = kAudioRing - (sAudioRead % kAudioRing);

    if (available > untilWrap) available = untilWrap;
    if (available > maxFrames) available = maxFrames;
    frames = &sAudio[(sAudioRead % kAudioRing) * 2];
    rate = sAudioRate;
    return available;
}

void GcnGuestHost::ConsumeAudio(uint32_t frames)
{
    std::lock_guard<std::mutex> guard(sLock);
    sAudioRead += frames;
}

void GcnGuestHost::FlushLog()
{
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> guard(sLock);
        lines.swap(sLogLines);
    }
    for (const std::string& line : lines)
    {
        if (line.find("CRASH") != std::string::npos || line.find("stopped after an error") != std::string::npos)
            LogError("%s", line.c_str());
        else
            LogDebug("%s", line.c_str());
    }
}

// ---- scripts ---------------------------------------------------------------------------------
namespace
{
// scripts wait at most this long for the game to finish its frame
const int kScriptWaitMs = 100;

bool InRam(uint32_t addr, uint32_t bytes)
{
    const uint32_t off = addr & 0x01FFFFFFu;
    return sModule != nullptr && ((addr >> 24) == 0x80 || (addr >> 24) == 0x81 || (addr >> 24) == 0xC0 ||
                                  (addr >> 24) == 0xC1 || (addr >> 24) == 0x00 || (addr >> 24) == 0x01) &&
           off + bytes <= 0x01800000u;
}

int SizeType(uint32_t size)
{
    switch (size)
    {
    case 1: return GCNW_VAR_U8;
    case 2: return GCNW_VAR_S16;
    default: return GCNW_VAR_S32;
    }
}
}

bool GcnGuestHost::Resolve(const std::string& name, uint32_t& addr, uint32_t& size, int& type, int& count, int& stride)
{
    if (name.empty())
    {
        return false;
    }
    char* end = nullptr;
    const unsigned long number = strtoul(name.c_str(), &end, 0);
    if (end != nullptr && *end == 0)
    {
        addr = (uint32_t)number;
        size = 4;
        type = GCNW_VAR_S32;
        count = 1;
        stride = 4;
        return true;
    }
    if (sModule == nullptr)
    {
        return false;
    }
    if (const GcnwBridgeVar* v = gcnw_bridge_find_var(name.c_str()))
    {
        addr = v->addr;
        type = v->type;
        count = v->count;
        stride = v->stride;
        size = (uint32_t)(v->stride * v->count);
        return true;
    }
    if (const GcnwSymbol* s = gcnw_find_symbol(name.c_str()))
    {
        addr = s->addr;
        size = s->size;
        type = SizeType(s->size);
        stride = type == GCNW_VAR_U8 ? 1 : type == GCNW_VAR_S16 ? 2 : 4;
        count = stride ? (int)(s->size / (uint32_t)stride) : 1;
        if (count < 1) count = 1;
        return true;
    }
    return false;
}

uint32_t GcnGuestHost::Read(uint32_t addr, int bytes)
{
    Hold(kScriptWaitMs);
    if (!InRam(addr, (uint32_t)bytes))
    {
        return 0;
    }
    const uint8_t* p = gcnw_memory() + (addr & 0x01FFFFFFu);
    uint32_t v = 0;
    for (int i = 0; i < bytes; ++i)
    {
        v = (v << 8) | p[i];
    }
    return v;
}

void GcnGuestHost::Write(uint32_t addr, uint32_t value, int bytes)
{
    Hold(kScriptWaitMs);
    if (!InRam(addr, (uint32_t)bytes))
    {
        return;
    }
    uint8_t* p = gcnw_memory() + (addr & 0x01FFFFFFu);
    for (int i = bytes - 1; i >= 0; --i)
    {
        p[i] = (uint8_t)value;
        value >>= 8;
    }
}

std::string GcnGuestHost::ReadString(uint32_t addr, uint32_t maxBytes)
{
    std::string text;
    Hold(kScriptWaitMs);
    for (uint32_t i = 0; i < maxBytes && InRam(addr + i, 1); ++i)
    {
        const char c = (char)gcnw_memory()[(addr + i) & 0x01FFFFFFu];
        if (c == 0) break;
        text += c;
    }
    return text;
}

std::vector<GcnGuestHost::BridgeVar> GcnGuestHost::BridgeVariables()
{
    std::vector<BridgeVar> vars;
    if (sModule == nullptr) return vars;
    for (int i = 0; i < gcnw_bridge_var_count(); ++i)
    {
        const GcnwBridgeVar* v = gcnw_bridge_var(i);
        vars.push_back({v->name, v->help, v->type, v->count});
    }
    return vars;
}

std::vector<GcnGuestHost::BridgeRequestInfo> GcnGuestHost::BridgeRequests()
{
    std::vector<BridgeRequestInfo> requests;
    if (sModule == nullptr) return requests;
    for (int i = 0; i < gcnw_bridge_request_count(); ++i)
    {
        const GcnwBridgeRequest* r = gcnw_bridge_request_info(i);
        requests.push_back({r->name, r->help});
    }
    return requests;
}

int GcnGuestHost::BridgeRequest(const std::string& name, const std::vector<int>& args)
{
    Hold(kScriptWaitMs);
    std::lock_guard<std::mutex> guard(sLock);
    // only while the game is parked: the queue belongs to the game thread otherwise
    if (sModule == nullptr || sRun || sState != State::Running)
    {
        return 0;
    }
    return gcnw_bridge_request(name.c_str(), args.data(), (int)args.size());
}

bool GcnGuestHost::BridgeResult(int id, int& result)
{
    Hold(kScriptWaitMs);
    std::lock_guard<std::mutex> guard(sLock);
    return sModule != nullptr && !sRun && gcnw_bridge_result(id, &result) != 0;
}

std::vector<GcnGuestHost::BridgeEvent> GcnGuestHost::BridgeEvents()
{
    std::vector<BridgeEvent> events;
    Hold(kScriptWaitMs);
    std::lock_guard<std::mutex> guard(sLock);
    GcnwBridgeEvent e;

    while (sModule != nullptr && !sRun && gcnw_bridge_next_event(&e))
    {
        events.push_back({e.name, std::vector<int>(e.args, e.args + e.nargs)});
    }
    return events;
}

uint32_t GcnGuestHost::FrameCount()
{
    return sFrameCount;
}

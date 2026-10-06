/*
 * Threads, message queues and mutexes of the Dolphin OS, scheduled cooperatively.
 *
 * Every guest thread runs on its own host context (gcn_host_ctx_*: a native stack plus
 * the guest stack the game gave OSCreateThread). The scheduler switches only where the
 * game blocks or yields, or makes a higher-priority thread runnable; interrupts the
 * console would raise (vertical retrace, finished disc reads) are delivered at those
 * points, and by gcn_idle() when nothing can run. Thread state lives in the game's
 * OSThread structures, as on the console, so code that looks at them sees the usual.
 */
#include <dolphin.h>
#include <dolphin/os.h>

#include "gcn_guest.h"
#include "gcn_sdk.h"

#define MAX_THREADS 64
#define CTX(t) ((t)->context.gpr[0])    /* host context + 1; 0 = not started */
#define STARTED(t) ((t)->context.gpr[2]) /* entry function, kept until the context starts */

static OSThread sMainThread;
static OSThread *sCurrent;
static OSThread *sThreads[MAX_THREADS];
static int sThreadCount;
static int sSchedulerOff;
static int sInEvents;

static void dequeue(OSThread *t);

static void add_thread(OSThread *t)
{
    int i;

    for (i = 0; i < sThreadCount; i++)
    {
        if (sThreads[i] == t) return;
    }
    if (sThreadCount < MAX_THREADS) sThreads[sThreadCount++] = t;
    else gcn_host_fatal("gcn: too many threads");
}

static void remove_thread(OSThread *t)
{
    int i;

    for (i = 0; i < sThreadCount; i++)
    {
        if (sThreads[i] == t)
        {
            sThreads[i] = sThreads[--sThreadCount];
            return;
        }
    }
}

void gcn_threads_init(void)
{
    sMainThread.state = OS_THREAD_STATE_RUNNING;
    sMainThread.priority = sMainThread.base = 16;
    CTX(&sMainThread) = 1; /* host context 0 */
    sCurrent = &sMainThread;
    add_thread(&sMainThread);
}

OSThread *OSGetCurrentThread(void)
{
    return sCurrent;
}

static int runnable(OSThread *t)
{
    return t->state == OS_THREAD_STATE_READY || t->state == OS_THREAD_STATE_RUNNING ? t->suspend <= 0 : 0;
}

/* Highest priority runnable thread; among equals the one after the current (round robin). */
static OSThread *pick(int yield)
{
    OSThread *best = 0;
    int i, start = 0;

    for (i = 0; i < sThreadCount; i++)
    {
        if (sThreads[i] == sCurrent) start = i + 1;
    }
    for (i = 0; i < sThreadCount; i++)
    {
        OSThread *t = sThreads[(start + i) % sThreadCount];

        if (!runnable(t)) continue;
        if (t == sCurrent && yield) continue;
        if (best == 0 || t->priority < best->priority) best = t;
    }
    if (yield && sCurrent && runnable(sCurrent) && (best == 0 || sCurrent->priority < best->priority))
    {
        best = sCurrent;
    }
    return best;
}

static void switch_to(OSThread *t)
{
    OSThread *prev = sCurrent;

    if (t == prev) return;
    if (prev->state == OS_THREAD_STATE_RUNNING) prev->state = OS_THREAD_STATE_READY;
    t->state = OS_THREAD_STATE_RUNNING;
    sCurrent = t;
    if (CTX(t) == 0)
    {
        int ctx = gcn_host_ctx_create((void *)STARTED(t), t->val, t->stackBase);
        CTX(t) = (u32)ctx + 1;
    }
    gcn_host_ctx_switch((int)CTX(t) - 1);
    /* back here when something switched to this thread again */
}

/* Run the best runnable thread; if none, wait for events until one is. */
static void schedule(int yield)
{
    OSThread *t;

    if (sSchedulerOff || sInEvents) return;
    for (;;)
    {
        t = pick(yield);
        if (t)
        {
            switch_to(t);
            return;
        }
        /* nothing can run: interrupts the GPU raised may wake someone; else wait */
        sInEvents = 1;
        gcn_dvd_poll();
        if (gcn_dsp_poll() + gcn_dispatch_interrupts())
        {
            sInEvents = 0;
            continue;
        }
        sInEvents = 0;
        gcn_idle();
    }
}

void gcn_reschedule(void)
{
    OSThread *t;

    if (sSchedulerOff || sInEvents) return;
    t = pick(0);
    if (t && t != sCurrent && (!runnable(sCurrent) || t->priority < sCurrent->priority)) switch_to(t);
}

/* Called when the current thread cannot continue. */
static void block(void)
{
    if (sInEvents)
    {
        /* an interrupt handler or callback waits: on the console it would hang; here let
         * the waiting thread go on later, as if the wait returned at once */
        static int said;
        if (!said)
        {
            said = 1;
            gcn_host_log("gcn: a callback tried to block; continuing");
        }
        if (sCurrent->state == OS_THREAD_STATE_WAITING)
        {
            dequeue(sCurrent);
            sCurrent->state = OS_THREAD_STATE_RUNNING;
        }
        return;
    }
    if (sSchedulerOff)
    {
        /* waiting with the scheduler off: let time pass until something wakes us */
        while (!runnable(sCurrent)) gcn_idle();
        return;
    }
    schedule(0);
}

void gcn_poll_events(void)
{
    if (sInEvents) return;
    sInEvents = 1;
    gcn_dvd_poll();
    gcn_dsp_poll();
    gcn_dispatch_interrupts();
    sInEvents = 0;
}

/* Nothing can run: wait for the next vertical retrace and deliver what happened. */
void gcn_idle(void)
{
    if (sInEvents) gcn_host_fatal("gcn: a callback blocked");
    sInEvents = 1;
    gcn_vi_retrace();
    gcn_dvd_poll();
    gcn_dsp_poll();
    gcn_dispatch_interrupts();
    sInEvents = 0;
}

/* The game spins on memory only an interrupt would change (the host notices repeated
 * volatile loads): deliver what is pending, else let a retrace happen. No thread switch
 * here - the spinning code is in the middle of a function; woken threads run at the next
 * scheduling point. */
void gcn_spin_wait(void)
{
    int n;

    if (sInEvents) return;
    sInEvents = 1;
    n = gcn_dvd_poll_count();
    n += gcn_dsp_poll();
    n += gcn_dispatch_interrupts();
    if (!n) gcn_vi_retrace();
    sInEvents = 0;
}

void *gcn_thread_main(void *(*fn)(void *), void *arg)
{
    void *ret = fn(arg);
    OSExitThread(ret);
    return 0;
}

int OSCreateThread(OSThread *thread, void *(*func)(void *), void *param, void *stack, u32 stackSize,
                   OSPriority priority, u16 attr)
{
    if (priority < OS_PRIORITY_MIN || priority > OS_PRIORITY_MAX) return FALSE;
    memset(thread, 0, sizeof(*thread));
    thread->state = OS_THREAD_STATE_READY;
    thread->attr = attr & 1;
    thread->base = thread->priority = priority;
    thread->suspend = 1;
    thread->val = param;
    thread->stackBase = (u8 *)stack;
    thread->stackEnd = (u32 *)((u8 *)stack - stackSize);
    *thread->stackEnd = OS_THREAD_STACK_MAGIC;
    STARTED(thread) = (u32)func;
    add_thread(thread);
    return TRUE;
}

s32 OSResumeThread(OSThread *thread)
{
    s32 prev = thread->suspend--;

    if (thread->suspend < 0) thread->suspend = 0;
    if (thread->suspend == 0) gcn_reschedule();
    return prev;
}

s32 OSSuspendThread(OSThread *thread)
{
    s32 prev = thread->suspend++;

    if (thread == sCurrent) block();
    return prev;
}

BOOL OSIsThreadSuspended(OSThread *thread) { return thread->suspend > 0; }
BOOL OSIsThreadTerminated(OSThread *thread) { return thread->state == OS_THREAD_STATE_MORIBUND || thread->state == 0; }

void OSInitThreadQueue(OSThreadQueue *queue)
{
    queue->head = queue->tail = 0;
}

static void enqueue(OSThreadQueue *q, OSThread *t)
{
    t->link.next = 0;
    t->link.prev = q->tail;
    if (q->tail) q->tail->link.next = t;
    else q->head = t;
    q->tail = t;
    t->queue = q;
}

static void dequeue(OSThread *t)
{
    OSThreadQueue *q = t->queue;

    if (!q) return;
    if (t->link.prev) t->link.prev->link.next = t->link.next;
    else q->head = t->link.next;
    if (t->link.next) t->link.next->link.prev = t->link.prev;
    else q->tail = t->link.prev;
    t->queue = 0;
}

void OSSleepThread(OSThreadQueue *queue)
{
    OSThread *t = sCurrent;

    t->state = OS_THREAD_STATE_WAITING;
    enqueue(queue, t);
    block();
}

void OSWakeupThread(OSThreadQueue *queue)
{
    while (queue->head)
    {
        OSThread *t = queue->head;
        dequeue(t);
        t->state = OS_THREAD_STATE_READY;
    }
    gcn_reschedule();
}

void OSYieldThread(void)
{
    schedule(1);
}

s32 OSDisableScheduler(void) { return sSchedulerOff++; }

s32 OSEnableScheduler(void)
{
    s32 prev = sSchedulerOff;
    if (sSchedulerOff > 0) sSchedulerOff--;
    return prev;
}

void OSExitThread(void *val)
{
    OSThread *t = sCurrent;

    t->val = val;
    t->state = OS_THREAD_STATE_MORIBUND;
    OSWakeupThread(&t->queueJoin);
    if (t->attr & OS_THREAD_ATTR_DETACH) remove_thread(t);
    schedule(0);
    gcn_host_fatal("gcn: an exited thread ran again");
}

void OSCancelThread(OSThread *thread)
{
    if (thread == sCurrent)
    {
        OSExitThread(0);
        return;
    }
    dequeue(thread);
    thread->state = OS_THREAD_STATE_MORIBUND;
    remove_thread(thread);
    if (CTX(thread))
    {
        gcn_host_ctx_destroy((int)CTX(thread) - 1);
        CTX(thread) = 0;
    }
    OSWakeupThread(&thread->queueJoin);
}

int OSJoinThread(OSThread *thread, void *val)
{
    while (thread->state != OS_THREAD_STATE_MORIBUND && thread->state != 0)
    {
        OSSleepThread(&thread->queueJoin);
    }
    if (val) *(void **)val = thread->val;
    remove_thread(thread);
    return TRUE;
}

void OSDetachThread(OSThread *thread)
{
    thread->attr |= OS_THREAD_ATTR_DETACH;
}

int OSSetThreadPriority(OSThread *thread, OSPriority priority)
{
    thread->priority = thread->base = priority;
    gcn_reschedule();
    return TRUE;
}

s32 OSGetThreadPriority(OSThread *thread) { return thread->base; }
s32 OSCheckActiveThreads(void) { return sThreadCount; }
void OSClearStack(u8 val) {}

/* ---- message queues ------------------------------------------------------------------- */
void OSInitMessageQueue(OSMessageQueue *mq, void *msgArray, s32 msgCount)
{
    OSInitThreadQueue(&mq->queueSend);
    OSInitThreadQueue(&mq->queueReceive);
    mq->msgArray = msgArray;
    mq->msgCount = msgCount;
    mq->firstIndex = 0;
    mq->usedCount = 0;
}

int OSSendMessage(OSMessageQueue *mq, void *msg, s32 flags)
{
    while (mq->usedCount >= mq->msgCount)
    {
        if (!(flags & OS_MESSAGE_BLOCK)) return FALSE;
        OSSleepThread(&mq->queueSend);
    }
    ((OSMessage *)mq->msgArray)[(mq->firstIndex + mq->usedCount) % mq->msgCount] = msg;
    mq->usedCount++;
    OSWakeupThread(&mq->queueReceive);
    return TRUE;
}

int OSJamMessage(OSMessageQueue *mq, void *msg, s32 flags)
{
    while (mq->usedCount >= mq->msgCount)
    {
        if (!(flags & OS_MESSAGE_BLOCK)) return FALSE;
        OSSleepThread(&mq->queueSend);
    }
    mq->firstIndex = (mq->firstIndex + mq->msgCount - 1) % mq->msgCount;
    ((OSMessage *)mq->msgArray)[mq->firstIndex] = msg;
    mq->usedCount++;
    OSWakeupThread(&mq->queueReceive);
    return TRUE;
}

int OSReceiveMessage(OSMessageQueue *mq, void *msg, s32 flags)
{
    while (mq->usedCount == 0)
    {
        if (!(flags & OS_MESSAGE_BLOCK)) return FALSE;
        OSSleepThread(&mq->queueReceive);
    }
    if (msg) *(OSMessage *)msg = ((OSMessage *)mq->msgArray)[mq->firstIndex];
    mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
    mq->usedCount--;
    OSWakeupThread(&mq->queueSend);
    return TRUE;
}

/* ---- mutexes ---------------------------------------------------------------------------- */
void OSInitMutex(OSMutex *mutex)
{
    OSInitThreadQueue(&mutex->queue);
    mutex->thread = 0;
    mutex->count = 0;
}

void OSLockMutex(OSMutex *mutex)
{
    for (;;)
    {
        if (mutex->thread == 0)
        {
            mutex->thread = sCurrent;
            mutex->count = 1;
            return;
        }
        if (mutex->thread == sCurrent)
        {
            mutex->count++;
            return;
        }
        OSSleepThread(&mutex->queue);
    }
}

void OSUnlockMutex(OSMutex *mutex)
{
    if (mutex->thread == sCurrent && --mutex->count == 0)
    {
        mutex->thread = 0;
        OSWakeupThread(&mutex->queue);
    }
}

BOOL OSTryLockMutex(OSMutex *mutex)
{
    if (mutex->thread == 0)
    {
        mutex->thread = sCurrent;
        mutex->count = 1;
        return TRUE;
    }
    if (mutex->thread == sCurrent)
    {
        mutex->count++;
        return TRUE;
    }
    return FALSE;
}

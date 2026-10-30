#define _DARWIN_C_SOURCE 1
#include "bw_posix_ke_event.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#define BW_KE_CAPACITY 4096u
#define BW_KE_MAX_WAIT 64u
#define BW_NT_EPOCH_100NS UINT64_C(116444736000000000)

typedef struct KeSlot {
    uint32_t va;
    uint64_t generation;
    unsigned waiters;
    uint8_t type;
} KeSlot;

typedef struct KeWaitRef {
    KeSlot *slot;
    uint64_t generation;
} KeWaitRef;

typedef struct KeWaiter {
    struct KeWaiter *next;
    const BwPosixGuestMem *mem;
    const uint32_t *objects;
    KeWaitRef *refs;
    size_t count;
    uint64_t deadline;
    int wait_all;
    int armed;
    int polling_alert;
    int done;
    BwKeResult result;
    uint32_t selected;
} KeWaiter;

typedef struct KeWaitCleanup {
    KeWaiter *waiter;
    KeWaitRef *refs;
    size_t registered;
    int linked;
    int locked;
} KeWaitCleanup;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_changed = PTHREAD_COND_INITIALIZER;
static KeSlot s_slots[BW_KE_CAPACITY];
static KeWaiter *s_waiters;

static void dispatch_waiters(uint32_t pulse_va, uint8_t pulse_type);

static int lock_manager(void)
{
    int previous;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous);
    pthread_mutex_lock(&s_lock);
    return previous;
}

static void unlock_manager(int previous)
{
    pthread_mutex_unlock(&s_lock);
    pthread_setcancelstate(previous, NULL);
}

static BwKeResult read_header(const BwPosixGuestMem *mem, uint32_t va,
                              BwKeHeader *header)
{
    uint8_t bytes[16];
    BwKeHeader h;

    if ((va & 3u) || !bw_posix_guest_read(mem, va, bytes, sizeof bytes))
        return BW_KE_INACCESSIBLE;
    h.type = bytes[0];
    h.size = bytes[2];
    memcpy(&h.state, bytes + 4, sizeof h.state);
    if (header)
        *header = h;
    /* Xbox event headers encode their size in DWORDs. */
    if (h.type > 1 || h.size != 4 || h.state > 1)
        return BW_KE_NOT_EVENT;
    return BW_KE_OK;
}

BwKeResult bw_posix_ke_probe(const BwPosixGuestMem *mem, uint32_t va,
                             BwKeHeader *header)
{
    return read_header(mem, va, header);
}

static KeSlot *find_slot(uint32_t va)
{
    KeSlot *free_slot = NULL;
    KeSlot *idle_slot = NULL;

    for (size_t i = 0; i < BW_KE_CAPACITY; i++) {
        KeSlot *slot = &s_slots[i];
        if (slot->va == va)
            return slot;
        if (!slot->va && !free_slot)
            free_slot = slot;
        if (slot->va && !slot->waiters && !idle_slot)
            idle_slot = slot;
    }
    /* All persistent state lives in guest memory; idle slots can be reused. */
    return free_slot ? free_slot : idle_slot;
}

static void register_slot(KeSlot *slot, uint32_t va, uint8_t type, int reinit)
{
    if (slot->va != va) {
        memset(slot, 0, sizeof *slot);
        slot->va = va;
        slot->type = type;
        slot->generation = 1;
    } else if (reinit || slot->type != type) {
        slot->type = type;
        slot->generation++;
    }
}

BwKeResult bw_posix_ke_initialize(const BwPosixGuestMem *mem, uint32_t va,
                                  uint32_t type, uint32_t state)
{
    uint8_t bytes[16] = {0};
    uint32_t list_head;
    KeSlot *slot;
    int cancel_state;

    if (type > 1 || state > 1 || va < 0x1000u ||
        (va & 3u) || va > UINT32_MAX - 15u)
        return BW_KE_INVALID;
    bytes[0] = (uint8_t)type;
    bytes[2] = 4;
    list_head = va + 8;
    memcpy(bytes + 4, &state, sizeof state);
    memcpy(bytes + 8, &list_head, sizeof list_head);
    memcpy(bytes + 12, &list_head, sizeof list_head);

    cancel_state = lock_manager();
    slot = find_slot(va);
    if (!slot) {
        unlock_manager(cancel_state);
        return BW_KE_EXHAUSTED;
    }
    if (!bw_posix_guest_write(mem, va, bytes, sizeof bytes)) {
        unlock_manager(cancel_state);
        return BW_KE_INACCESSIBLE;
    }
    register_slot(slot, va, (uint8_t)type, 1);
    dispatch_waiters(0, 0);
    pthread_cond_broadcast(&s_changed);
    unlock_manager(cancel_state);
    return BW_KE_OK;
}

void bw_posix_ke_invalidate(uint32_t va)
{
    int cancel_state;
    if (!va)
        return;
    cancel_state = lock_manager();
    for (size_t i = 0; i < BW_KE_CAPACITY; i++) {
        KeSlot *slot = &s_slots[i];
        if (slot->va != va)
            continue;
        if (slot->waiters) {
            slot->generation++;
            dispatch_waiters(0, 0);
            pthread_cond_broadcast(&s_changed);
        } else {
            memset(slot, 0, sizeof *slot);
        }
        break;
    }
    unlock_manager(cancel_state);
}

BwKeResult bw_posix_ke_modify(const BwPosixGuestMem *mem, uint32_t va,
                              BwKeOperation operation, uint32_t *previous)
{
    BwKeHeader header;
    BwKeResult result;
    KeSlot *slot;
    uint32_t next;
    int cancel_state;

    if (operation != BW_KE_SET && operation != BW_KE_RESET &&
        operation != BW_KE_PULSE)
        return BW_KE_INVALID;
    cancel_state = lock_manager();
    result = read_header(mem, va, &header);
    if (result != BW_KE_OK)
        goto done;
    slot = find_slot(va);
    if (!slot) {
        result = BW_KE_EXHAUSTED;
        goto done;
    }
    next = operation == BW_KE_SET ? 1u : 0u;
    if (!bw_posix_guest_write(mem, va + 4, &next, sizeof next)) {
        result = BW_KE_INACCESSIBLE;
        goto done;
    }
    register_slot(slot, va, header.type, 0);
    if (previous)
        *previous = header.state;
    dispatch_waiters(operation == BW_KE_PULSE ? va : 0, header.type);
    pthread_cond_broadcast(&s_changed);

done:
    unlock_manager(cancel_state);
    return result;
}

static uint64_t now_ns(clockid_t clock)
{
    struct timespec ts;
    if (clock_gettime(clock, &ts) != 0 || ts.tv_sec < 0)
        return 0;
    if ((uint64_t)ts.tv_sec > UINT64_MAX / UINT64_C(1000000000))
        return UINT64_MAX;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

uint64_t bw_posix_ke_monotonic_ns(void)
{
    return now_ns(CLOCK_MONOTONIC);
}

uint64_t bw_posix_ke_deadline_ns(const int64_t *timeout)
{
    uint64_t ticks, delta, now = now_ns(CLOCK_MONOTONIC);

    if (!timeout)
        return UINT64_MAX;
    if (*timeout < 0) {
        ticks = (uint64_t)(-(*timeout + 1)) + 1;
    } else {
        uint64_t realtime = now_ns(CLOCK_REALTIME) / 100u;
        if (realtime > UINT64_MAX - BW_NT_EPOCH_100NS)
            return now;
        realtime += BW_NT_EPOCH_100NS;
        if ((uint64_t)*timeout <= realtime)
            return now;
        ticks = (uint64_t)*timeout - realtime;
    }
    delta = ticks > UINT64_MAX / 100u ? UINT64_MAX : ticks * 100u;
    return delta > UINT64_MAX - now ? UINT64_MAX : now + delta;
}

static void try_waiter(KeWaiter *waiter, uint32_t pulse_va, int pulse_available,
                       int *pulse_consumed)
{
    BwKeHeader headers[BW_KE_MAX_WAIT];
    uint8_t pulsed[BW_KE_MAX_WAIT];
    size_t first = waiter->count;
    int all_ready = 1;

    *pulse_consumed = 0;
    for (size_t i = 0; i < waiter->count; i++) {
        KeSlot *slot = waiter->refs[i].slot;
        BwKeResult result = read_header(waiter->mem, waiter->objects[i],
                                        &headers[i]);
        if (result != BW_KE_OK || slot->generation != waiter->refs[i].generation ||
            slot->type != headers[i].type) {
            waiter->result = result == BW_KE_OK ? BW_KE_CHANGED : result;
            waiter->done = 1;
            return;
        }
        pulsed[i] = pulse_available && pulse_va == waiter->objects[i];
        if (headers[i].state || pulsed[i]) {
            if (first == waiter->count)
                first = i;
        } else {
            all_ready = 0;
        }
    }
    if (!((waiter->wait_all && all_ready) ||
          (!waiter->wait_all && first != waiter->count)))
        return;

    /* Preflight every synchronization-event write before consuming any.
     * Protection can still change concurrently, so failed commits roll back
     * best-effort and never report a successful wait. */
    if (waiter->wait_all) {
        for (size_t i = 0; i < waiter->count; i++) {
            if (headers[i].state && headers[i].type &&
                !bw_posix_guest_write(waiter->mem, waiter->objects[i] + 4,
                                      &headers[i].state, sizeof headers[i].state)) {
                waiter->result = BW_KE_INACCESSIBLE;
                waiter->done = 1;
                return;
            }
        }
    }
    for (size_t i = 0; i < waiter->count; i++) {
        uint32_t zero = 0;
        if (!waiter->wait_all && i != first)
            continue;
        if (headers[i].state && headers[i].type &&
            !bw_posix_guest_write(waiter->mem, waiter->objects[i] + 4,
                                  &zero, sizeof zero)) {
            if (waiter->wait_all) {
                uint32_t one = 1;
                for (size_t j = 0; j < i; j++)
                    if (headers[j].state && headers[j].type)
                        (void)bw_posix_guest_write(waiter->mem,
                                                   waiter->objects[j] + 4,
                                                   &one, sizeof one);
            }
            waiter->result = BW_KE_INACCESSIBLE;
            waiter->done = 1;
            return;
        }
        if (pulsed[i] && !headers[i].state && headers[i].type)
            *pulse_consumed = 1;
    }
    waiter->selected = waiter->wait_all ? 0u : (uint32_t)first;
    waiter->result = BW_KE_OK;
    waiter->done = 1;
}

static void dispatch_waiters(uint32_t pulse_va, uint8_t pulse_type)
{
    int pulse_available = pulse_va != 0;
    int changed = 0;
    uint64_t now = now_ns(CLOCK_MONOTONIC);

    /* Decide pulse eligibility now, not after another object becomes ready. */
    for (KeWaiter *waiter = s_waiters; waiter; waiter = waiter->next) {
        int consumed = 0;
        if (waiter->done || waiter->polling_alert)
            continue;
        if (waiter->armed && waiter->deadline != UINT64_MAX &&
            now >= waiter->deadline) {
            waiter->result = BW_KE_TIMEOUT;
            waiter->done = 1;
        } else {
            try_waiter(waiter, pulse_va, pulse_available, &consumed);
        }
        if (consumed && pulse_type)
            pulse_available = 0;
        changed |= waiter->done;
    }
    if (changed)
        pthread_cond_broadcast(&s_changed);
}

static void cleanup_waiter(void *context)
{
    KeWaitCleanup *cleanup = context;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    if (!cleanup->locked)
        pthread_mutex_lock(&s_lock);
    if (cleanup->linked) {
        KeWaiter **link = &s_waiters;
        while (*link && *link != cleanup->waiter)
            link = &(*link)->next;
        if (*link)
            *link = cleanup->waiter->next;
    }
    for (size_t i = 0; i < cleanup->registered; i++)
        cleanup->refs[i].slot->waiters--;
    dispatch_waiters(0, 0);
    pthread_mutex_unlock(&s_lock);
}

static void dispatch_waiters_protected(void)
{
    int cancel_state;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
    dispatch_waiters(0, 0);
    pthread_setcancelstate(cancel_state, NULL);
}

BwKeResult bw_posix_ke_wait(const BwPosixGuestMem *mem, const uint32_t *objects,
                            size_t count, int wait_all, const int64_t *timeout,
                            BwKeAlertPoll alert_poll, void *alert_context,
                            uint32_t *selected)
{
    KeWaitRef refs[BW_KE_MAX_WAIT];
    KeWaiter waiter;
    KeWaitCleanup cleanup = {&waiter, refs, 0, 0, 1};
    uint64_t deadline = bw_posix_ke_deadline_ns(timeout);
    BwKeResult result = BW_KE_OK;
    int previous_cancel_type, exit_cancel_state;

    if (!mem || !objects || !selected || !count || count > BW_KE_MAX_WAIT)
        return BW_KE_INVALID;
    for (size_t i = 0; i < count; i++)
        for (size_t j = 0; j < i; j++)
            if (objects[i] == objects[j])
                return BW_KE_INVALID;

    if (pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &previous_cancel_type) != 0)
        return BW_KE_INVALID;
    pthread_mutex_lock(&s_lock);
    pthread_cleanup_push(cleanup_waiter, &cleanup);
    for (size_t i = 0; i < count; i++) {
        KeSlot *slot;
        BwKeHeader header;
        result = read_header(mem, objects[i], &header);
        if (result != BW_KE_OK)
            goto out;
        slot = find_slot(objects[i]);
        if (!slot) {
            result = BW_KE_EXHAUSTED;
            goto out;
        }
        register_slot(slot, objects[i], header.type, 0);
        refs[i] = (KeWaitRef){slot, slot->generation};
        slot->waiters++;
        cleanup.registered++;
    }

    waiter = (KeWaiter){NULL, mem, objects, refs, count, deadline,
                        wait_all, 0, 0, 0, BW_KE_OK, 0};
    KeWaiter **link = &s_waiters;
    while (*link)
        link = &(*link)->next;
    *link = &waiter;
    cleanup.linked = 1;

    for (;;) {
        if (waiter.done) {
            result = waiter.result;
            if (result == BW_KE_OK)
                *selected = waiter.selected;
            break;
        }
        if (alert_poll) {
            int cancel_state;
            /* Alert callbacks may enter guest code and re-enter this manager. */
            waiter.polling_alert = 1;
            pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
            pthread_mutex_unlock(&s_lock);
            cleanup.locked = 0;
            pthread_setcancelstate(cancel_state, NULL);
            int alerted = alert_poll(alert_context);
            pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &cancel_state);
            pthread_mutex_lock(&s_lock);
            cleanup.locked = 1;
            pthread_setcancelstate(cancel_state, NULL);
            waiter.polling_alert = 0;
            if (alerted) {
                result = BW_KE_ALERTED;
                break;
            }
        }
        dispatch_waiters_protected();
        waiter.armed = 1;
        if (waiter.done) {
            result = waiter.result;
            if (result == BW_KE_OK)
                *selected = waiter.selected;
            break;
        }
        uint64_t now = now_ns(CLOCK_MONOTONIC);
        if (deadline != UINT64_MAX && now >= deadline) {
            result = BW_KE_TIMEOUT;
            break;
        }
        if (deadline == UINT64_MAX && !alert_poll) {
            if (pthread_cond_wait(&s_changed, &s_lock) != 0) {
                result = BW_KE_INVALID;
                break;
            }
        } else {
            uint64_t remaining = deadline == UINT64_MAX ? UINT64_MAX : deadline - now;
            if (alert_poll && remaining > UINT64_C(10000000))
                remaining = UINT64_C(10000000);
            struct timespec delay = {(time_t)(remaining / UINT64_C(1000000000)),
                                     (long)(remaining % UINT64_C(1000000000))};
            int rc = pthread_cond_timedwait_relative_np(&s_changed, &s_lock, &delay);
            if (rc && rc != ETIMEDOUT) {
                result = BW_KE_INVALID;
                break;
            }
        }
    }

out:
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &exit_cancel_state);
    pthread_cleanup_pop(1);
    pthread_setcanceltype(previous_cancel_type, NULL);
    pthread_setcancelstate(exit_cancel_state, NULL);
    return result;
}

#if defined(BW_KE_EVENT_TESTING)
unsigned bw_posix_ke_test_waiters(uint32_t va)
{
    unsigned waiters = 0;
    int cancel_state = lock_manager();
    for (KeWaiter *waiter = s_waiters; waiter; waiter = waiter->next)
        if (!waiter->done)
            for (size_t i = 0; i < waiter->count; i++)
                if (waiter->objects[i] == va)
                    waiters++;
    unlock_manager(cancel_state);
    return waiters;
}
#endif

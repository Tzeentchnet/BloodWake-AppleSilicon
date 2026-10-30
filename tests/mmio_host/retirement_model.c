#include "retirement_model.h"

#include <stdatomic.h>
#include <string.h>

static _Atomic uint64_t next_domain = 1;

int rm_init(RetirementModel *m)
{
    uint64_t id = atomic_load_explicit(&next_domain, memory_order_relaxed);

    while (id != UINT64_MAX &&
           !atomic_compare_exchange_weak_explicit(&next_domain, &id, id + 1,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {}
    if (id == UINT64_MAX)
        return 0;
    memset(m, 0, sizeof(*m));
    if (pthread_mutex_init(&m->lock, NULL) != 0)
        return 0;
    m->domain = id;
    return 1;
}

/* The caller must first prevent and join every thread that can enter any API. */
void rm_destroy(RetirementModel *m)
{
    pthread_mutex_destroy(&m->lock);
}

static RmSlot *lookup(RetirementModel *m, RmToken t)
{
    if (t.domain != m->domain || t.slot >= RM_CAPACITY ||
        m->slots[t.slot].state == RM_FREE ||
        m->slots[t.slot].generation != t.generation)
        return NULL;
    return &m->slots[t.slot];
}

static RmResult allocate(RetirementModel *m, RmWorkState state,
                         unsigned dpc, RmToken *out)
{
    for (unsigned i = 0; i < RM_CAPACITY; ++i) {
        RmSlot *s = &m->slots[i];
        if (s->state == RM_FREE && s->generation != UINT64_MAX) {
            ++s->generation;
            s->state = state;
            s->dpc = dpc;
            *out = (RmToken){m->domain, s->generation, i};
            return RM_OK;
        }
    }
    return RM_FULL;
}

static RmResult queue_dpc(RetirementModel *m, unsigned dpc, RmToken *out)
{
    if (!dpc)
        return RM_INVALID;
    for (unsigned i = 0; i < RM_CAPACITY; ++i)
        if (m->slots[i].state == RM_DPC_QUEUED && m->slots[i].dpc == dpc)
            return RM_DUPLICATE;
    return allocate(m, RM_DPC_QUEUED, dpc, out);
}

RmResult rm_admit_irq(RetirementModel *m, RmToken *out)
{
    RmResult r = RM_CLOSED;
    pthread_mutex_lock(&m->lock);
    if (m->phase == RM_OPEN) {
        r = RM_DUPLICATE;
        for (unsigned i = 0; i < RM_CAPACITY; ++i)
            if (m->slots[i].state == RM_IRQ_PENDING)
                goto done;
        r = allocate(m, RM_IRQ_PENDING, 0, out);
    }
done:
    pthread_mutex_unlock(&m->lock);
    return r;
}

RmResult rm_admit_dpc(RetirementModel *m, unsigned dpc, RmToken *out)
{
    RmResult r;
    pthread_mutex_lock(&m->lock);
    r = m->phase == RM_OPEN ? queue_dpc(m, dpc, out) : RM_CLOSED;
    pthread_mutex_unlock(&m->lock);
    return r;
}

RmResult rm_admit_access(RetirementModel *m, RmToken *out)
{
    RmResult r;
    pthread_mutex_lock(&m->lock);
    r = m->phase == RM_OPEN ? allocate(m, RM_ACCESS, 0, out) : RM_CLOSED;
    pthread_mutex_unlock(&m->lock);
    return r;
}

static RmResult transition(RetirementModel *m, RmToken t,
                           RmWorkState from, RmWorkState to)
{
    RmResult r = RM_INVALID;
    pthread_mutex_lock(&m->lock);
    RmSlot *s = lookup(m, t);
    if (s && s->state == from) {
        s->state = to;
        r = RM_OK;
    }
    pthread_mutex_unlock(&m->lock);
    return r;
}

RmResult rm_claim_irq(RetirementModel *m, RmToken t)
{
    return transition(m, t, RM_IRQ_PENDING, RM_IRQ_CLAIMED);
}

RmResult rm_start_irq(RetirementModel *m, RmToken t)
{
    return transition(m, t, RM_IRQ_CLAIMED, RM_IRQ_RUNNING);
}

RmResult rm_start_dpc(RetirementModel *m, RmToken t)
{
    return transition(m, t, RM_DPC_QUEUED, RM_DPC_RUNNING);
}

RmResult rm_spawn_dpc(RetirementModel *m, RmToken parent,
                      unsigned dpc, RmToken *out)
{
    RmResult r = RM_INVALID;
    pthread_mutex_lock(&m->lock);
    RmSlot *s = lookup(m, parent);
    if (s && (s->state == RM_IRQ_RUNNING || s->state == RM_DPC_RUNNING ||
              s->state == RM_ACCESS))
        r = queue_dpc(m, dpc, out);
    pthread_mutex_unlock(&m->lock);
    return r;
}

RmResult rm_cancel_queued(RetirementModel *m, RmToken t)
{
    RmResult r = RM_INVALID;
    pthread_mutex_lock(&m->lock);
    RmSlot *s = lookup(m, t);
    if (s && (s->state == RM_IRQ_PENDING || s->state == RM_DPC_QUEUED)) {
        s->state = RM_FREE;
        r = RM_OK;
    }
    pthread_mutex_unlock(&m->lock);
    return r;
}

RmResult rm_finish(RetirementModel *m, RmToken t)
{
    RmResult r = RM_INVALID;
    pthread_mutex_lock(&m->lock);
    RmSlot *s = lookup(m, t);
    if (s && (s->state == RM_IRQ_RUNNING || s->state == RM_DPC_RUNNING ||
              s->state == RM_ACCESS)) {
        s->state = RM_FREE;
        r = RM_OK;
    }
    pthread_mutex_unlock(&m->lock);
    return r;
}

void rm_close(RetirementModel *m)
{
    pthread_mutex_lock(&m->lock);
    if (m->phase == RM_OPEN)
        m->phase = RM_CLOSING;
    pthread_mutex_unlock(&m->lock);
}

RmResult rm_producers_joined(RetirementModel *m)
{
    RmResult r = RM_INVALID;
    pthread_mutex_lock(&m->lock);
    if (m->phase != RM_OPEN) {
        m->producers_joined = 1;
        r = RM_OK;
    }
    pthread_mutex_unlock(&m->lock);
    return r;
}

int rm_try_retire(RetirementModel *m)
{
    int ready = 0;
    pthread_mutex_lock(&m->lock);
    if (m->phase == RM_RETIRED) {
        ready = 1;
    } else if (m->phase == RM_CLOSING && m->producers_joined) {
        ready = 1;
        for (unsigned i = 0; i < RM_CAPACITY; ++i)
            if (m->slots[i].state != RM_FREE)
                ready = 0;
        if (ready)
            m->phase = RM_RETIRED;
    }
    pthread_mutex_unlock(&m->lock);
    return ready;
}

unsigned rm_count(RetirementModel *m, RmWorkState state)
{
    unsigned count = 0;
    pthread_mutex_lock(&m->lock);
    for (unsigned i = 0; i < RM_CAPACITY; ++i)
        count += m->slots[i].state == state;
    pthread_mutex_unlock(&m->lock);
    return count;
}

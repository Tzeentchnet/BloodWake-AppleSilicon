/* A bounded, test-only model. All modeled metadata and byte accesses share
 * one mutex; production guest pointers do not participate in this protocol. */
#include "physical_authority_model.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

enum { PHYSICAL_BYTES = PM_PAGE_SIZE * PM_PHYSICAL_PAGES,
       VIEW_BYTES = PM_PAGE_SIZE * PM_VIEW_PAGES };
enum { OWNER_FREE, OWNER_LIVE, OWNER_RETIRED };

typedef struct {
    unsigned committed, access, pins;
} Page;

typedef struct {
    unsigned physical_page, pins;
} Alias;

typedef struct {
    uint64_t generation;
    size_t offset, length;
    unsigned state, leases;
} Owner;

typedef struct {
    uint64_t serial, owner_generation;
    size_t offset, length, physical_offset;
    unsigned active, owner_index, view, access;
} Lease;

struct PmModel {
    pthread_mutex_t lock;
    uint64_t authority, next_owner, next_lease;
    unsigned closing, active_leases;
    Page pages[PM_PHYSICAL_PAGES];
    Alias aliases[PM_VIEWS][PM_VIEW_PAGES];
    Owner owners[PM_MAX_OWNERS];
    Lease leases[PM_MAX_LEASES];
    uint64_t byte_owner[PHYSICAL_BYTES];
    uint8_t bytes[PHYSICAL_BYTES];
};

typedef struct {
    unsigned owner_index;
    size_t physical_offset;
} Resolved;

static pthread_mutex_t authority_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t last_authority;

static void lock(pthread_mutex_t *mutex)
{
    if (pthread_mutex_lock(mutex)) abort();
}

static void unlock(pthread_mutex_t *mutex)
{
    if (pthread_mutex_unlock(mutex)) abort();
}

static int in_range(size_t offset, size_t length, size_t capacity)
{
    return length && offset < capacity && length <= capacity - offset;
}

static int owner_index(PmModel *m, PmOwner token)
{
    if (token.authority != m->authority || !token.generation) return -1;
    for (unsigned i = 0; i < PM_MAX_OWNERS; i++)
        if (m->owners[i].state == OWNER_LIVE &&
            m->owners[i].generation == token.generation)
            return (int)i;
    return -1;
}

static void collect(PmModel *m, unsigned index)
{
    Owner *owner = &m->owners[index];
    if (owner->state != OWNER_RETIRED || owner->leases) return;
    for (size_t i = owner->offset; i < owner->offset + owner->length; i++) {
        if (m->byte_owner[i] != owner->generation) abort();
        m->byte_owner[i] = 0;
    }
    memset(owner, 0, sizeof(*owner));
}

PmModel *pm_create(void)
{
    PmModel *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    if (pthread_mutex_init(&m->lock, NULL)) {
        free(m);
        return NULL;
    }
    lock(&authority_lock);
    if (last_authority != UINT64_MAX)
        m->authority = ++last_authority;
    unlock(&authority_lock);
    if (!m->authority) {
        if (pthread_mutex_destroy(&m->lock)) abort();
        free(m);
        return NULL;
    }
    for (unsigned view = 0; view < PM_VIEWS; view++)
        for (unsigned page = 0; page < PM_VIEW_PAGES; page++)
            m->aliases[view][page].physical_page = PM_NO_PAGE;
    return m;
}

PmResult pm_begin_shutdown(PmModel *m)
{
    if (!m) return PM_INVALID;
    lock(&m->lock);
    if (!m->closing) {
        m->closing = 1;
        for (unsigned i = 0; i < PM_MAX_OWNERS; i++) {
            if (m->owners[i].state == OWNER_LIVE)
                m->owners[i].state = OWNER_RETIRED;
            collect(m, i);
        }
    }
    unlock(&m->lock);
    return PM_OK;
}

PmResult pm_shutdown_ready(PmModel *m)
{
    PmResult result;
    if (!m) return PM_INVALID;
    lock(&m->lock);
    result = m->closing && !m->active_leases ? PM_OK : PM_BUSY;
    unlock(&m->lock);
    return result;
}

PmResult pm_destroy(PmModel *m)
{
    if (!m) return PM_INVALID;
    lock(&m->lock);
    if (!m->closing || m->active_leases) {
        unlock(&m->lock);
        return PM_BUSY;
    }
    for (unsigned i = 0; i < PM_MAX_OWNERS; i++)
        if (m->owners[i].state != OWNER_FREE) abort();
    unlock(&m->lock);
    if (pthread_mutex_destroy(&m->lock)) abort();
    free(m);
    return PM_OK;
}

PmResult pm_commit_page(PmModel *m, unsigned page)
{
    PmResult result = PM_OK;
    if (!m || page >= PM_PHYSICAL_PAGES) return PM_INVALID;
    lock(&m->lock);
    if (m->closing) result = PM_CLOSED;
    else if (m->pages[page].committed) result = PM_CONFLICT;
    else {
        if (m->pages[page].pins) abort();
        for (size_t i = page * PM_PAGE_SIZE; i < (page + 1) * PM_PAGE_SIZE; i++)
            if (m->byte_owner[i]) abort();
        memset(&m->bytes[page * PM_PAGE_SIZE], 0, PM_PAGE_SIZE);
        m->pages[page].committed = 1;
        m->pages[page].access = PM_READ | PM_WRITE;
    }
    unlock(&m->lock);
    return result;
}

PmResult pm_decommit_page(PmModel *m, unsigned page)
{
    PmResult result = PM_OK;
    if (!m || page >= PM_PHYSICAL_PAGES) return PM_INVALID;
    lock(&m->lock);
    if (m->closing) result = PM_CLOSED;
    else if (!m->pages[page].committed) result = PM_UNMAPPED;
    else if (m->pages[page].pins) result = PM_BUSY;
    else {
        for (size_t i = page * PM_PAGE_SIZE; i < (page + 1) * PM_PAGE_SIZE; i++)
            if (m->byte_owner[i]) {
                result = PM_BUSY;
                break;
            }
        if (result == PM_OK) {
            m->pages[page].committed = 0;
            m->pages[page].access = 0;
        }
    }
    unlock(&m->lock);
    return result;
}

PmResult pm_protect_page(PmModel *m, unsigned page, unsigned access)
{
    PmResult result = PM_OK;
    if (!m || page >= PM_PHYSICAL_PAGES || (access & ~(PM_READ | PM_WRITE)))
        return PM_INVALID;
    lock(&m->lock);
    if (m->closing) result = PM_CLOSED;
    else if (!m->pages[page].committed) result = PM_UNMAPPED;
    else if (m->pages[page].pins) result = PM_BUSY;
    else m->pages[page].access = access;
    unlock(&m->lock);
    return result;
}

PmResult pm_map_page(PmModel *m, unsigned view, unsigned virtual_page,
                     unsigned physical_page)
{
    PmResult result = PM_OK;
    Alias *alias;
    unsigned old;
    if (!m || view >= PM_VIEWS || virtual_page >= PM_VIEW_PAGES ||
        (physical_page != PM_NO_PAGE && physical_page >= PM_PHYSICAL_PAGES))
        return PM_INVALID;
    lock(&m->lock);
    alias = &m->aliases[view][virtual_page];
    old = alias->physical_page;
    if (m->closing) result = PM_CLOSED;
    else if (physical_page != PM_NO_PAGE && !m->pages[physical_page].committed)
        result = PM_UNMAPPED;
    else if (old == physical_page) result = PM_OK;
    else if (alias->pins ||
             (old != PM_NO_PAGE && m->pages[old].pins) ||
             (physical_page != PM_NO_PAGE && m->pages[physical_page].pins))
        result = PM_BUSY;
    else alias->physical_page = physical_page;
    unlock(&m->lock);
    return result;
}

PmResult pm_claim(PmModel *m, size_t physical_offset, size_t length,
                  PmOwner *out)
{
    unsigned slot;
    PmResult result = PM_OK;
    if (!m || !out || !in_range(physical_offset, length, PHYSICAL_BYTES))
        return PM_INVALID;
    lock(&m->lock);
    if (m->closing) {
        result = PM_CLOSED;
        goto out;
    }
    for (slot = 0; slot < PM_MAX_OWNERS; slot++)
        if (m->owners[slot].state == OWNER_FREE) break;
    if (slot == PM_MAX_OWNERS || m->next_owner == UINT64_MAX) {
        result = PM_CAPACITY;
        goto out;
    }
    for (size_t i = physical_offset; i < physical_offset + length; i++) {
        Page *page = &m->pages[i / PM_PAGE_SIZE];
        if (!page->committed) { result = PM_UNMAPPED; goto out; }
        if (!(page->access & PM_WRITE)) { result = PM_PROTECTION; goto out; }
        if (m->byte_owner[i]) { result = PM_CONFLICT; goto out; }
    }
    uint64_t generation = ++m->next_owner;
    m->owners[slot] = (Owner){ generation, physical_offset, length, OWNER_LIVE, 0 };
    for (size_t i = physical_offset; i < physical_offset + length; i++)
        m->byte_owner[i] = generation;
    memset(&m->bytes[physical_offset], 0, length);
    *out = (PmOwner){ m->authority, generation };
out:
    unlock(&m->lock);
    return result;
}

PmResult pm_retire(PmModel *m, PmOwner token)
{
    PmResult result = PM_OK;
    int index;
    if (!m) return PM_INVALID;
    lock(&m->lock);
    if (m->closing) result = PM_CLOSED;
    else if ((index = owner_index(m, token)) < 0) result = PM_STALE;
    else {
        m->owners[index].state = OWNER_RETIRED;
        collect(m, (unsigned)index);
    }
    unlock(&m->lock);
    return result;
}

static PmResult resolve(PmModel *m, const PmRequest *request, Resolved *out)
{
    size_t physical;
    int index;
    unsigned first_page;
    if (request->view >= PM_VIEWS ||
        !in_range(request->offset, request->length, VIEW_BYTES) ||
        !request->access || (request->access & ~(PM_READ | PM_WRITE)))
        return PM_INVALID;
    index = owner_index(m, request->owner);
    if (index < 0) return PM_STALE;
    first_page = m->aliases[request->view][request->offset / PM_PAGE_SIZE]
                     .physical_page;
    if (first_page == PM_NO_PAGE) return PM_UNMAPPED;
    physical = first_page * PM_PAGE_SIZE + request->offset % PM_PAGE_SIZE;
    if (!in_range(physical, request->length, PHYSICAL_BYTES)) return PM_NONLINEAR;
    for (size_t i = 0; i < request->length; i++) {
        size_t address = request->offset + i;
        unsigned page = m->aliases[request->view][address / PM_PAGE_SIZE].physical_page;
        size_t current;
        if (page == PM_NO_PAGE) return PM_UNMAPPED;
        current = page * PM_PAGE_SIZE + address % PM_PAGE_SIZE;
        if (current != physical + i) return PM_NONLINEAR;
        if (!m->pages[page].committed) return PM_UNMAPPED;
        if ((m->pages[page].access & request->access) != request->access)
            return PM_PROTECTION;
        if (m->byte_owner[current] != request->owner.generation)
            return PM_CONFLICT;
    }
    *out = (Resolved){ (unsigned)index, physical };
    return PM_OK;
}

PmResult pm_submit(PmModel *m, const PmRequest *requests, size_t count,
                   PmLease *out)
{
    PmRequest batch[PM_MAX_BATCH];
    Resolved resolved[PM_MAX_BATCH];
    unsigned slots[PM_MAX_BATCH];
    size_t found = 0;
    PmResult result = PM_OK;
    if (!m || !requests || !out || !count || count > PM_MAX_BATCH)
        return PM_INVALID;
    lock(&m->lock);
    if (m->closing) {
        result = PM_CLOSED;
        goto out;
    }
    memcpy(batch, requests, count * sizeof(*batch));
    for (size_t i = 0; i < count; i++) {
        result = resolve(m, &batch[i], &resolved[i]);
        if (result != PM_OK) goto out;
    }
    for (unsigned i = 0; i < PM_MAX_LEASES && found < count; i++)
        if (!m->leases[i].active) slots[found++] = i;
    if (found != count || count > UINT64_MAX - m->next_lease) {
        result = PM_CAPACITY;
        goto out;
    }
    for (size_t i = 0; i < count; i++) {
        const PmRequest *request = &batch[i];
        Lease *lease = &m->leases[slots[i]];
        size_t first = request->offset / PM_PAGE_SIZE;
        size_t last = (request->offset + request->length - 1) / PM_PAGE_SIZE;
        *lease = (Lease){ ++m->next_lease, request->owner.generation,
                          request->offset, request->length, resolved[i].physical_offset,
                          1, resolved[i].owner_index, request->view, request->access };
        for (size_t page = first; page <= last; page++) {
            Alias *alias = &m->aliases[request->view][page];
            alias->pins++;
            m->pages[alias->physical_page].pins++;
        }
        m->owners[resolved[i].owner_index].leases++;
        m->active_leases++;
        out[i] = (PmLease){ m->authority, lease->serial, slots[i] };
    }
out:
    unlock(&m->lock);
    return result;
}

static Lease *find_lease(PmModel *m, PmLease token)
{
    Lease *lease;
    if (token.authority != m->authority || !token.serial ||
        token.slot >= PM_MAX_LEASES) return NULL;
    lease = &m->leases[token.slot];
    return lease->active && lease->serial == token.serial ? lease : NULL;
}

PmResult pm_release(PmModel *m, PmLease token)
{
    Lease *lease;
    Owner *owner;
    unsigned index;
    if (!m) return PM_INVALID;
    lock(&m->lock);
    lease = find_lease(m, token);
    if (!lease) {
        unlock(&m->lock);
        return PM_STALE;
    }
    index = lease->owner_index;
    owner = &m->owners[index];
    if (owner->state == OWNER_FREE || owner->generation != lease->owner_generation ||
        !owner->leases || !m->active_leases) abort();
    size_t first = lease->offset / PM_PAGE_SIZE;
    size_t last = (lease->offset + lease->length - 1) / PM_PAGE_SIZE;
    for (size_t page = first; page <= last; page++) {
        Alias *alias = &m->aliases[lease->view][page];
        if (alias->physical_page == PM_NO_PAGE || !alias->pins ||
            !m->pages[alias->physical_page].pins) abort();
        alias->pins--;
        m->pages[alias->physical_page].pins--;
    }
    owner->leases--;
    m->active_leases--;
    memset(lease, 0, sizeof(*lease));
    collect(m, index);
    unlock(&m->lock);
    return PM_OK;
}

static PmResult guest_copy(PmModel *m, PmOwner owner, unsigned view,
                           size_t offset, void *buffer, size_t length, int write)
{
    PmRequest request = { owner, view, offset, length, write ? PM_WRITE : PM_READ };
    Resolved resolved;
    PmResult result;
    if (!m || !buffer) return PM_INVALID;
    lock(&m->lock);
    result = m->closing ? PM_CLOSED : resolve(m, &request, &resolved);
    if (result == PM_OK) {
        if (write) memcpy(&m->bytes[resolved.physical_offset], buffer, length);
        else memcpy(buffer, &m->bytes[resolved.physical_offset], length);
    }
    unlock(&m->lock);
    return result;
}

PmResult pm_guest_read(PmModel *m, PmOwner owner, unsigned view,
                       size_t offset, void *destination, size_t length)
{
    return guest_copy(m, owner, view, offset, destination, length, 0);
}

PmResult pm_guest_write(PmModel *m, PmOwner owner, unsigned view,
                        size_t offset, const void *source, size_t length)
{
    return guest_copy(m, owner, view, offset, (void *)source, length, 1);
}

static PmResult device_copy(PmModel *m, PmLease token, size_t offset,
                            void *buffer, size_t length, int write)
{
    PmResult result = PM_OK;
    Lease *lease;
    if (!m || !buffer || !length) return PM_INVALID;
    lock(&m->lock);
    lease = find_lease(m, token);
    if (!lease) result = PM_STALE;
    else if (!(lease->access & (write ? PM_WRITE : PM_READ))) result = PM_PROTECTION;
    else if (!in_range(offset, length, lease->length)) result = PM_INVALID;
    else if (write) memcpy(&m->bytes[lease->physical_offset + offset], buffer, length);
    else memcpy(buffer, &m->bytes[lease->physical_offset + offset], length);
    unlock(&m->lock);
    return result;
}

PmResult pm_device_read(PmModel *m, PmLease lease, size_t offset,
                        void *destination, size_t length)
{
    return device_copy(m, lease, offset, destination, length, 0);
}

PmResult pm_device_write(PmModel *m, PmLease lease, size_t offset,
                         const void *source, size_t length)
{
    return device_copy(m, lease, offset, (void *)source, length, 1);
}

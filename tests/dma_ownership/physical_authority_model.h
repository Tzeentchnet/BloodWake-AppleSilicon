/* Synthetic contract model only; this is not a guest-memory implementation. */
#ifndef BW_PHYSICAL_AUTHORITY_MODEL_H
#define BW_PHYSICAL_AUTHORITY_MODEL_H

#include <stddef.h>
#include <stdint.h>

enum {
    PM_PAGE_SIZE = 64,
    PM_PHYSICAL_PAGES = 8,
    PM_VIEW_PAGES = 8,
    PM_VIEWS = 3,
    PM_MAX_OWNERS = 32,
    PM_MAX_LEASES = 32,
    PM_MAX_BATCH = 8
};

#define PM_NO_PAGE UINT32_MAX

enum {
    PM_READ = 1,
    PM_WRITE = 2
};

typedef enum {
    PM_OK,
    PM_INVALID,
    PM_CLOSED,
    PM_STALE,
    PM_CONFLICT,
    PM_UNMAPPED,
    PM_PROTECTION,
    PM_NONLINEAR,
    PM_CAPACITY,
    PM_BUSY
} PmResult;

typedef struct PmModel PmModel;

typedef struct {
    uint64_t authority;
    uint64_t generation;
} PmOwner;

typedef struct {
    uint64_t authority;
    uint64_t serial;
    unsigned slot;
} PmLease;

typedef struct {
    PmOwner owner;
    unsigned view;
    size_t offset;
    size_t length;
    unsigned access;
} PmRequest;

PmModel *pm_create(void);
PmResult pm_begin_shutdown(PmModel *model);
/* This checks leases only, not threads or external callback queues. */
PmResult pm_shutdown_ready(PmModel *model);
/* Caller must have joined all threads that can enter any model API. */
PmResult pm_destroy(PmModel *model);

PmResult pm_commit_page(PmModel *model, unsigned page);
PmResult pm_decommit_page(PmModel *model, unsigned page);
PmResult pm_protect_page(PmModel *model, unsigned page, unsigned access);
PmResult pm_map_page(PmModel *model, unsigned view, unsigned virtual_page,
                     unsigned physical_page);

PmResult pm_claim(PmModel *model, size_t physical_offset, size_t length,
                  PmOwner *owner);
PmResult pm_retire(PmModel *model, PmOwner owner);
/* On failure no pins are taken and the output array is unchanged. */
PmResult pm_submit(PmModel *model, const PmRequest *requests, size_t count,
                   PmLease *leases);
PmResult pm_release(PmModel *model, PmLease lease);

PmResult pm_guest_read(PmModel *model, PmOwner owner, unsigned view,
                       size_t offset, void *destination, size_t length);
PmResult pm_guest_write(PmModel *model, PmOwner owner, unsigned view,
                        size_t offset, const void *source, size_t length);
PmResult pm_device_read(PmModel *model, PmLease lease, size_t offset,
                        void *destination, size_t length);
PmResult pm_device_write(PmModel *model, PmLease lease, size_t offset,
                         const void *source, size_t length);

#endif

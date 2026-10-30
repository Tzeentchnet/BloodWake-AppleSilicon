#ifndef BW_BACKEND_SYNC_H
#define BW_BACKEND_SYNC_H

#include <stdint.h>

#include "nv2a_backend.h"
#include "../toolkit_patches/kernel/bw_backend_ext.h"

/* The callback runs synchronously on the executor thread before either the
 * window snapshot or framebuffer dump. It must finish GPU work and copy the
 * selected surface into guest memory; return zero on failure to suppress both
 * consumers. The backend must report the error itself. As with the toolkit
 * registration API, change registrations only while the executor is idle. */
void bw_backend_register_with_present_sync(
    const Nv2aBackend *backend, int (*present_sync)(uint32_t color_va, uint32_t pitch));

/* Remove only registrations still owned by the caller. */
void bw_backend_unregister_if_current(const Nv2aBackend *backend);
void bw_backend_ext_unregister_if_current(const BwXBackend *backend);

typedef struct {
    Nv2aSurface surface;
    uint32_t format;
    uint32_t clip_x, clip_y;
} BwBackendSnapshotSurface;

/* Executor-thread only: return the surface selected for the pending snapshot,
 * including the state frozen when a rendered surface was selected. */
void bw_backend_snapshot_surface(BwBackendSnapshotSurface *snapshot);

#endif

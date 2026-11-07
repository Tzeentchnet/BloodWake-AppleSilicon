#ifndef BW_BACKEND_SYNC_H
#define BW_BACKEND_SYNC_H

#include <stdint.h>

#include "nv2a_backend.h"
#include "../toolkit_patches/kernel/bw_backend_ext.h"

/* The callbacks run on the executor thread. prepare_present selects and
 * validates the private generation used by the window; publish_snapshot
 * additionally makes that generation CPU-visible in guest memory for dumps
 * and other explicit snapshot consumers. Either callback may be NULL.
 * Return zero on failure and report the backend error before returning.
 * Change registrations only while the executor is idle. */
void bw_backend_register_with_present_ops(
    const Nv2aBackend *backend,
    int (*prepare_present)(uint32_t color_va, uint32_t pitch),
    int (*publish_snapshot)(uint32_t color_va, uint32_t pitch));

/* Register only for the current backend while the executor is idle. The
 * callbacks run serially on the executor thread. flip_stall receives the host
 * time immediately before presentation work; flip_release receives the host
 * time when the guest-facing flip hold releases, including immediate flips. */
void bw_backend_register_flip_timing(
    const Nv2aBackend *backend,
    void (*flip_stall)(uint64_t host_ns),
    void (*flip_release)(uint64_t host_ns));

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

#ifndef BW_GPU_CAPTURE_H
#define BW_GPU_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

/*
 * A deliberately synthetic, single-owner capture format. The producer must
 * supply every guest-memory change and synchronization event; this API does
 * not observe or pin live guest memory. See docs/GPU_CAPTURE.md.
 */
#define BW_GPU_CAPTURE_MAX_BYTES (16u * 1024u * 1024u)
#define BW_GPU_CAPTURE_MAX_EVENTS 262144u
#define BW_GPU_CAPTURE_MAX_MEMORY_EVENT (4u * 1024u * 1024u)

typedef enum {
    BW_GPU_CAPTURE_OK = 0,
    BW_GPU_CAPTURE_ARGUMENT,
    BW_GPU_CAPTURE_LIMIT,
    BW_GPU_CAPTURE_IO,
    BW_GPU_CAPTURE_FORMAT,
    BW_GPU_CAPTURE_UNSUPPORTED,
    BW_GPU_CAPTURE_MISMATCH,
    BW_GPU_CAPTURE_CALLBACK,
    BW_GPU_CAPTURE_THREAD
} BwGpuCaptureStatus;

typedef enum {
    BW_GPU_CAPTURE_METHOD = 1,
    BW_GPU_CAPTURE_MEMORY = 2,
    BW_GPU_CAPTURE_SEMAPHORE_TARGET = 3,
    BW_GPU_CAPTURE_BARRIER = 4,
    BW_GPU_CAPTURE_EXPECT_MEMORY = 5
} BwGpuCaptureEventType;

typedef enum {
    BW_GPU_CAPTURE_BARRIER_EXECUTOR = 1,
    BW_GPU_CAPTURE_BARRIER_BACKEND = 2
} BwGpuCaptureBarrier;

typedef struct {
    BwGpuCaptureEventType type;
    union {
        struct { uint32_t subchannel, method, parameter; } method;
        struct { uint32_t address, size; const uint8_t *bytes; } memory;
        struct { uint32_t address; } semaphore;
        struct { BwGpuCaptureBarrier kind; } barrier;
    } data;
} BwGpuCaptureEvent;

typedef struct BwGpuCapture BwGpuCapture;

/*
 * max_bytes includes the file header and footer. Limits can be made smaller
 * but cannot exceed the hard caps. All recording errors are sticky; a failed
 * or still-attached recorder cannot be written as a completed capture.
 */
BwGpuCapture *bw_gpu_capture_create(size_t max_bytes, uint32_t max_events);
/* Destroy refuses an attached recorder; detach it before releasing it. */
BwGpuCaptureStatus bw_gpu_capture_destroy(BwGpuCapture *capture);
BwGpuCaptureStatus bw_gpu_capture_status(const BwGpuCapture *capture);
void bw_gpu_capture_abort(BwGpuCapture *capture, BwGpuCaptureStatus reason);

BwGpuCaptureStatus bw_gpu_capture_method(BwGpuCapture *capture, uint32_t subchannel,
                                          uint32_t method, uint32_t parameter);
BwGpuCaptureStatus bw_gpu_capture_memory(BwGpuCapture *capture, uint32_t address,
                                          const void *bytes, uint32_t size);
BwGpuCaptureStatus bw_gpu_capture_semaphore_target(BwGpuCapture *capture, uint32_t address);
BwGpuCaptureStatus bw_gpu_capture_barrier(BwGpuCapture *capture, BwGpuCaptureBarrier kind);
BwGpuCaptureStatus bw_gpu_capture_expect_memory(BwGpuCapture *capture, uint32_t address,
                                                 const void *bytes, uint32_t size);

/* Exclusive, owner-only creation of an explicit .bwgpu path; never overwrites. */
BwGpuCaptureStatus bw_gpu_capture_write_file(BwGpuCapture *capture, const char *path);

/*
 * Replay first checks the entire file and then calls preflight for every
 * event, followed by preflight_end, before calling execute for any event.
 * Preflight must not perform guest/backend effects and must reject every
 * unsupported operation or missing input. Execute must propagate backend
 * errors and perform barriers and comparisons rather than ignoring them.
 * An execution failure is not rolled back; discard that replay context.
 * Event byte pointers are valid only until the callback returns.
 */
typedef struct {
    BwGpuCaptureStatus (*preflight)(void *context, const BwGpuCaptureEvent *event);
    BwGpuCaptureStatus (*preflight_end)(void *context);
    BwGpuCaptureStatus (*execute)(void *context, const BwGpuCaptureEvent *event);
} BwGpuCaptureReplayOps;

BwGpuCaptureStatus bw_gpu_capture_replay_file(const char *path,
                                               const BwGpuCaptureReplayOps *ops,
                                               void *context,
                                               uint32_t *failure_event);

#endif

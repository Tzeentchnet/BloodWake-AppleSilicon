#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif

#include "bw_gpu_capture.h"
#include "bw_gpu_capture_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define HEADER_BYTES 32u
#define FOOTER_BYTES 16u
#define EVENT_HEADER_BYTES 8u
#define FORMAT_VERSION 1u
#define SOURCE_SYNTHETIC_QUIESCENT 1u

static const uint8_t s_magic[8] = { 'B', 'W', 'G', 'P', 'U', 'C', '1', 0 };
static const uint8_t s_end_magic[8] = { 'B', 'W', 'E', 'N', 'D', '1', 0, 0 };

struct BwGpuCapture {
    uint8_t *body;
    size_t body_capacity, body_size;
    uint32_t max_events, events, methods;
    _Atomic(BwGpuCaptureStatus) status;
    int attached, written;
};

static void clear_private(void *ptr, size_t size)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (size--)
        *p++ = 0;
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put64(uint8_t *p, uint64_t value)
{
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

/* Detects accidental damage, not hostile modification or capture provenance. */
static uint64_t checksum(uint64_t hash, const uint8_t *data, size_t size)
{
    size_t i;
    for (i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int valid_path(const char *path)
{
    static const char suffix[] = ".bwgpu";
    size_t n;
    if (!path)
        return 0;
    n = strlen(path);
    return n > sizeof suffix - 1 &&
           strcmp(path + n - (sizeof suffix - 1), suffix) == 0;
}

static int valid_memory(uint32_t address, uint32_t size)
{
    return size && size <= BW_GPU_CAPTURE_MAX_MEMORY_EVENT &&
           (uint64_t)address + size <= UINT64_C(0x100000000);
}

static int valid_semaphore(uint32_t address)
{
    return !address || (!(address & 3u) && address <= UINT32_MAX - 3u);
}

static int valid_barrier(uint32_t kind)
{
    return kind == BW_GPU_CAPTURE_BARRIER_EXECUTOR ||
           kind == BW_GPU_CAPTURE_BARRIER_BACKEND;
}

BwGpuCapture *bw_gpu_capture_create(size_t max_bytes, uint32_t max_events)
{
    BwGpuCapture *capture;
    if (max_bytes < HEADER_BYTES + FOOTER_BYTES + EVENT_HEADER_BYTES ||
        max_bytes > BW_GPU_CAPTURE_MAX_BYTES || !max_events ||
        max_events > BW_GPU_CAPTURE_MAX_EVENTS)
        return NULL;
    capture = (BwGpuCapture *)calloc(1, sizeof *capture);
    if (!capture)
        return NULL;
    capture->body_capacity = max_bytes - HEADER_BYTES - FOOTER_BYTES;
    capture->body = (uint8_t *)malloc(capture->body_capacity);
    if (!capture->body) {
        free(capture);
        return NULL;
    }
    capture->max_events = max_events;
    atomic_init(&capture->status, BW_GPU_CAPTURE_OK);
    return capture;
}

BwGpuCaptureStatus bw_gpu_capture_destroy(BwGpuCapture *capture)
{
    if (!capture)
        return BW_GPU_CAPTURE_ARGUMENT;
    /* The fixture must first detach; otherwise its observer would dangle. */
    if (capture->attached)
        return BW_GPU_CAPTURE_ARGUMENT;
    clear_private(capture->body, capture->body_capacity);
    free(capture->body);
    clear_private(capture, sizeof *capture);
    free(capture);
    return BW_GPU_CAPTURE_OK;
}

BwGpuCaptureStatus bw_gpu_capture_status(const BwGpuCapture *capture)
{
    return capture ? capture->status : BW_GPU_CAPTURE_ARGUMENT;
}

void bw_gpu_capture_abort(BwGpuCapture *capture, BwGpuCaptureStatus reason)
{
    BwGpuCaptureStatus expected = BW_GPU_CAPTURE_OK;
    if (capture && reason != BW_GPU_CAPTURE_OK)
        (void)atomic_compare_exchange_strong_explicit(&capture->status, &expected, reason,
                                                       memory_order_relaxed,
                                                       memory_order_relaxed);
}

int bw_gpu_capture_set_attached(BwGpuCapture *capture, int attached)
{
    if (!capture || capture->written || (!!attached == !!capture->attached) ||
        (attached && (capture->status != BW_GPU_CAPTURE_OK || capture->events)))
        return 0;
    capture->attached = !!attached;
    return 1;
}

static uint8_t *append(BwGpuCapture *capture, uint32_t type, uint32_t payload)
{
    uint8_t *out;
    size_t need = (size_t)payload + EVENT_HEADER_BYTES;
    if (!capture || capture->status != BW_GPU_CAPTURE_OK)
        return NULL;
    if (capture->written) {
        bw_gpu_capture_abort(capture, BW_GPU_CAPTURE_ARGUMENT);
        return NULL;
    }
    if (capture->events >= capture->max_events ||
        need > capture->body_capacity - capture->body_size) {
        bw_gpu_capture_abort(capture, BW_GPU_CAPTURE_LIMIT);
        return NULL;
    }
    out = capture->body + capture->body_size;
    put32(out, type);
    put32(out + 4, payload);
    capture->body_size += need;
    capture->events++;
    return out + EVENT_HEADER_BYTES;
}

BwGpuCaptureStatus bw_gpu_capture_method(BwGpuCapture *capture, uint32_t subchannel,
                                          uint32_t method, uint32_t parameter)
{
    uint8_t *p;
    if (!capture)
        return BW_GPU_CAPTURE_ARGUMENT;
    if (subchannel >= 8 || method > 0x1FFCu || (method & 3u)) {
        bw_gpu_capture_abort(capture, BW_GPU_CAPTURE_ARGUMENT);
        return capture->status;
    }
    p = append(capture, BW_GPU_CAPTURE_METHOD, 12);
    if (p) {
        put32(p, subchannel);
        put32(p + 4, method);
        put32(p + 8, parameter);
        capture->methods++;
    }
    return capture->status;
}

static BwGpuCaptureStatus memory_event(BwGpuCapture *capture, uint32_t type,
                                        uint32_t address, const void *bytes, uint32_t size)
{
    uint8_t *p;
    if (!capture)
        return BW_GPU_CAPTURE_ARGUMENT;
    if (!bytes || !valid_memory(address, size)) {
        bw_gpu_capture_abort(capture, BW_GPU_CAPTURE_ARGUMENT);
        return capture->status;
    }
    p = append(capture, type, 8u + size);
    if (p) {
        put32(p, address);
        put32(p + 4, size);
        memcpy(p + 8, bytes, size);
    }
    return capture->status;
}

BwGpuCaptureStatus bw_gpu_capture_memory(BwGpuCapture *capture, uint32_t address,
                                          const void *bytes, uint32_t size)
{
    return memory_event(capture, BW_GPU_CAPTURE_MEMORY, address, bytes, size);
}

BwGpuCaptureStatus bw_gpu_capture_expect_memory(BwGpuCapture *capture, uint32_t address,
                                                 const void *bytes, uint32_t size)
{
    return memory_event(capture, BW_GPU_CAPTURE_EXPECT_MEMORY, address, bytes, size);
}

BwGpuCaptureStatus bw_gpu_capture_semaphore_target(BwGpuCapture *capture, uint32_t address)
{
    uint8_t *p;
    if (!capture)
        return BW_GPU_CAPTURE_ARGUMENT;
    if (!valid_semaphore(address)) {
        bw_gpu_capture_abort(capture, BW_GPU_CAPTURE_ARGUMENT);
        return capture->status;
    }
    p = append(capture, BW_GPU_CAPTURE_SEMAPHORE_TARGET, 4);
    if (p)
        put32(p, address);
    return capture->status;
}

BwGpuCaptureStatus bw_gpu_capture_barrier(BwGpuCapture *capture, BwGpuCaptureBarrier kind)
{
    uint8_t *p;
    if (!capture)
        return BW_GPU_CAPTURE_ARGUMENT;
    if (!valid_barrier((uint32_t)kind)) {
        bw_gpu_capture_abort(capture, BW_GPU_CAPTURE_ARGUMENT);
        return capture->status;
    }
    p = append(capture, BW_GPU_CAPTURE_BARRIER, 4);
    if (p)
        put32(p, (uint32_t)kind);
    return capture->status;
}

static int private_file(int fd, struct stat *st)
{
    return fstat(fd, st) == 0 && S_ISREG(st->st_mode) &&
           st->st_uid == geteuid() && !(st->st_mode & 077u) && st->st_nlink == 1;
}

static int write_all(int fd, const uint8_t *data, size_t size)
{
    while (size) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return 0;
        data += (size_t)n;
        size -= (size_t)n;
    }
    return 1;
}

BwGpuCaptureStatus bw_gpu_capture_write_file(BwGpuCapture *capture, const char *path)
{
    uint8_t header[HEADER_BYTES] = {0}, footer[FOOTER_BYTES] = {0};
    uint64_t hash;
    struct stat st;
    int fd, ok;
    if (!capture)
        return BW_GPU_CAPTURE_ARGUMENT;
    if (capture->status != BW_GPU_CAPTURE_OK)
        return capture->status;
    if (!valid_path(path) || capture->attached || capture->written || !capture->methods) {
        bw_gpu_capture_abort(capture, BW_GPU_CAPTURE_ARGUMENT);
        return capture->status;
    }
#if !defined(O_NOFOLLOW) || !defined(O_CLOEXEC)
    bw_gpu_capture_abort(capture, BW_GPU_CAPTURE_UNSUPPORTED);
    return capture->status;
#else
    memcpy(header, s_magic, sizeof s_magic);
    put32(header + 8, FORMAT_VERSION);
    put32(header + 12, SOURCE_SYNTHETIC_QUIESCENT);
    put32(header + 16, capture->events);
    put32(header + 20, (uint32_t)capture->body_size);
    hash = checksum(UINT64_C(14695981039346656037), header, sizeof header);
    hash = checksum(hash, capture->body, capture->body_size);
    memcpy(footer, s_end_magic, sizeof s_end_magic);
    put64(footer + 8, hash);

    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        bw_gpu_capture_abort(capture, BW_GPU_CAPTURE_IO);
        return capture->status;
    }
    ok = private_file(fd, &st) &&
         write_all(fd, header, sizeof header) &&
         write_all(fd, capture->body, capture->body_size) &&
         write_all(fd, footer, sizeof footer) && fsync(fd) == 0;
    if (!ok)
        (void)ftruncate(fd, 0);
    if (close(fd) != 0)
        ok = 0;
    if (!ok)
        bw_gpu_capture_abort(capture, BW_GPU_CAPTURE_IO);
    else
        capture->written = 1;
    return capture->status;
#endif
}

static int read_all(int fd, uint8_t *data, size_t size)
{
    while (size) {
        ssize_t n = read(fd, data, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return 0;
        data += (size_t)n;
        size -= (size_t)n;
    }
    return 1;
}

static int decode_event(const uint8_t *body, size_t left, size_t *used,
                        BwGpuCaptureEvent *event)
{
    uint32_t type, payload;
    const uint8_t *p;
    if (left < EVENT_HEADER_BYTES)
        return 0;
    type = get32(body);
    payload = get32(body + 4);
    if ((size_t)payload > left - EVENT_HEADER_BYTES)
        return 0;
    p = body + EVENT_HEADER_BYTES;
    memset(event, 0, sizeof *event);
    event->type = (BwGpuCaptureEventType)type;
    switch (type) {
    case BW_GPU_CAPTURE_METHOD:
        if (payload != 12 || get32(p) >= 8 || get32(p + 4) > 0x1FFCu ||
            (get32(p + 4) & 3u))
            return 0;
        event->data.method.subchannel = get32(p);
        event->data.method.method = get32(p + 4);
        event->data.method.parameter = get32(p + 8);
        break;
    case BW_GPU_CAPTURE_MEMORY:
    case BW_GPU_CAPTURE_EXPECT_MEMORY:
        if (payload < 8 || get32(p + 4) != payload - 8 ||
            !valid_memory(get32(p), get32(p + 4)))
            return 0;
        event->data.memory.address = get32(p);
        event->data.memory.size = get32(p + 4);
        event->data.memory.bytes = p + 8;
        break;
    case BW_GPU_CAPTURE_SEMAPHORE_TARGET:
        if (payload != 4 || !valid_semaphore(get32(p)))
            return 0;
        event->data.semaphore.address = get32(p);
        break;
    case BW_GPU_CAPTURE_BARRIER:
        if (payload != 4 || !valid_barrier(get32(p)))
            return 0;
        event->data.barrier.kind = (BwGpuCaptureBarrier)get32(p);
        break;
    default:
        return 0;
    }
    *used = EVENT_HEADER_BYTES + (size_t)payload;
    return 1;
}

static BwGpuCaptureStatus validate_structure(const uint8_t *file, size_t size)
{
    uint32_t count, i, methods = 0;
    size_t left, used;
    const uint8_t *p;
    uint64_t hash;
    BwGpuCaptureEvent event;
    if (size < HEADER_BYTES + FOOTER_BYTES ||
        memcmp(file, s_magic, sizeof s_magic) ||
        get32(file + 8) != FORMAT_VERSION ||
        get32(file + 12) != SOURCE_SYNTHETIC_QUIESCENT ||
        get32(file + 24) || get32(file + 28) ||
        memcmp(file + size - FOOTER_BYTES, s_end_magic, sizeof s_end_magic))
        return BW_GPU_CAPTURE_FORMAT;
    count = get32(file + 16);
    left = get32(file + 20);
    if (!count || count > BW_GPU_CAPTURE_MAX_EVENTS ||
        left != size - HEADER_BYTES - FOOTER_BYTES)
        return BW_GPU_CAPTURE_FORMAT;
    hash = checksum(UINT64_C(14695981039346656037), file, size - FOOTER_BYTES);
    if (hash != get64(file + size - 8))
        return BW_GPU_CAPTURE_FORMAT;
    p = file + HEADER_BYTES;
    for (i = 0; i < count; i++) {
        if (!decode_event(p, left, &used, &event))
            return BW_GPU_CAPTURE_FORMAT;
        if (event.type == BW_GPU_CAPTURE_METHOD)
            methods++;
        p += used;
        left -= used;
    }
    return left || !methods ? BW_GPU_CAPTURE_FORMAT : BW_GPU_CAPTURE_OK;
}

static BwGpuCaptureStatus visit(const uint8_t *file, void *context,
                                BwGpuCaptureStatus (*callback)(void *, const BwGpuCaptureEvent *),
                                uint32_t *failure_event)
{
    uint32_t count = get32(file + 16), i;
    size_t left = get32(file + 20), used;
    const uint8_t *p = file + HEADER_BYTES;
    BwGpuCaptureEvent event;
    for (i = 0; i < count; i++) {
        BwGpuCaptureStatus status;
        if (!decode_event(p, left, &used, &event))
            return BW_GPU_CAPTURE_FORMAT;
        status = callback(context, &event);
        if (status != BW_GPU_CAPTURE_OK) {
            if (failure_event)
                *failure_event = i;
            return status;
        }
        p += used;
        left -= used;
    }
    return BW_GPU_CAPTURE_OK;
}

BwGpuCaptureStatus bw_gpu_capture_replay_file(const char *path,
                                               const BwGpuCaptureReplayOps *ops,
                                               void *context,
                                               uint32_t *failure_event)
{
    struct stat st, after;
    uint8_t *file = NULL;
    size_t size = 0;
    BwGpuCaptureStatus status = BW_GPU_CAPTURE_IO;
    int fd;
    if (failure_event)
        *failure_event = UINT32_MAX;
    if (!valid_path(path) || !ops || !ops->preflight || !ops->preflight_end || !ops->execute)
        return BW_GPU_CAPTURE_ARGUMENT;
#if !defined(O_NOFOLLOW) || !defined(O_CLOEXEC)
    return BW_GPU_CAPTURE_UNSUPPORTED;
#else
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return BW_GPU_CAPTURE_IO;
    if (!private_file(fd, &st))
        goto out;
    if (st.st_size < (off_t)(HEADER_BYTES + FOOTER_BYTES)) {
        status = BW_GPU_CAPTURE_FORMAT;
        goto out;
    }
    if ((uint64_t)st.st_size > BW_GPU_CAPTURE_MAX_BYTES) {
        status = BW_GPU_CAPTURE_LIMIT;
        goto out;
    }
    size = (size_t)st.st_size;
    file = (uint8_t *)malloc(size);
    if (!file || !read_all(fd, file, size) || fstat(fd, &after) != 0 ||
        after.st_size != st.st_size)
        goto out;
    status = validate_structure(file, size);
    if (status != BW_GPU_CAPTURE_OK)
        goto out;
    status = visit(file, context, ops->preflight, failure_event);
    if (status != BW_GPU_CAPTURE_OK)
        goto out;
    status = ops->preflight_end(context);
    if (status != BW_GPU_CAPTURE_OK)
        goto out;
    status = visit(file, context, ops->execute, failure_event);
out:
    if (file) {
        clear_private(file, size);
        free(file);
    }
    (void)close(fd);
    return status;
#endif
}

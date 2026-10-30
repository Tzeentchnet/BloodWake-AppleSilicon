#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "bw_gpu_capture.h"
#include "bw_gpu_capture_fixture.h"
#include "fixture_executor.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 0; \
    } \
} while (0)

typedef struct {
    uint8_t memory[32];
    unsigned preflight_count, execute_count;
    uint32_t semaphore;
} SimpleReplay;

static BwGpuCaptureStatus simple_preflight(void *context, const BwGpuCaptureEvent *e)
{
    SimpleReplay *r = (SimpleReplay *)context;
    r->preflight_count++;
    switch (e->type) {
    case BW_GPU_CAPTURE_METHOD:
        return e->data.method.subchannel == 0 && e->data.method.method == 0x0208
             ? BW_GPU_CAPTURE_OK : BW_GPU_CAPTURE_UNSUPPORTED;
    case BW_GPU_CAPTURE_MEMORY:
    case BW_GPU_CAPTURE_EXPECT_MEMORY:
        return (uint64_t)e->data.memory.address + e->data.memory.size <= sizeof r->memory
             ? BW_GPU_CAPTURE_OK : BW_GPU_CAPTURE_UNSUPPORTED;
    case BW_GPU_CAPTURE_SEMAPHORE_TARGET:
        return e->data.semaphore.address == 8 ? BW_GPU_CAPTURE_OK : BW_GPU_CAPTURE_UNSUPPORTED;
    case BW_GPU_CAPTURE_BARRIER:
        return e->data.barrier.kind == BW_GPU_CAPTURE_BARRIER_EXECUTOR
             ? BW_GPU_CAPTURE_OK : BW_GPU_CAPTURE_UNSUPPORTED;
    default:
        return BW_GPU_CAPTURE_UNSUPPORTED;
    }
}

static BwGpuCaptureStatus simple_preflight_end(void *context)
{
    SimpleReplay *r = (SimpleReplay *)context;
    return r->preflight_count == 5 ? BW_GPU_CAPTURE_OK : BW_GPU_CAPTURE_UNSUPPORTED;
}

static BwGpuCaptureStatus simple_execute(void *context, const BwGpuCaptureEvent *e)
{
    SimpleReplay *r = (SimpleReplay *)context;
    static const BwGpuCaptureEventType sequence[] = {
        BW_GPU_CAPTURE_METHOD, BW_GPU_CAPTURE_MEMORY, BW_GPU_CAPTURE_SEMAPHORE_TARGET,
        BW_GPU_CAPTURE_BARRIER, BW_GPU_CAPTURE_EXPECT_MEMORY
    };
    if (r->execute_count >= sizeof sequence / sizeof sequence[0] ||
        e->type != sequence[r->execute_count++])
        return BW_GPU_CAPTURE_CALLBACK;
    switch (e->type) {
    case BW_GPU_CAPTURE_METHOD:
        return e->data.method.parameter == 8 ? BW_GPU_CAPTURE_OK : BW_GPU_CAPTURE_MISMATCH;
    case BW_GPU_CAPTURE_MEMORY:
        memcpy(r->memory + e->data.memory.address, e->data.memory.bytes, e->data.memory.size);
        return BW_GPU_CAPTURE_OK;
    case BW_GPU_CAPTURE_SEMAPHORE_TARGET:
        r->semaphore = e->data.semaphore.address;
        return BW_GPU_CAPTURE_OK;
    case BW_GPU_CAPTURE_BARRIER:
        return BW_GPU_CAPTURE_OK;
    case BW_GPU_CAPTURE_EXPECT_MEMORY:
        return memcmp(r->memory + e->data.memory.address, e->data.memory.bytes,
                      e->data.memory.size) ? BW_GPU_CAPTURE_MISMATCH : BW_GPU_CAPTURE_OK;
    default:
        return BW_GPU_CAPTURE_UNSUPPORTED;
    }
}

static const BwGpuCaptureReplayOps s_simple_ops = {
    simple_preflight, simple_preflight_end, simple_execute
};

static int sample_file(const char *path)
{
    const uint8_t bytes[4] = { 0x10, 0x20, 0x30, 0x40 };
    BwGpuCapture *capture = bw_gpu_capture_create(4096, 32);
    int ok;
    if (!capture)
        return 0;
    ok = bw_gpu_capture_method(capture, 0, 0x0208, 8) == BW_GPU_CAPTURE_OK &&
         bw_gpu_capture_memory(capture, 4, bytes, sizeof bytes) == BW_GPU_CAPTURE_OK &&
         bw_gpu_capture_semaphore_target(capture, 8) == BW_GPU_CAPTURE_OK &&
         bw_gpu_capture_barrier(capture, BW_GPU_CAPTURE_BARRIER_EXECUTOR) == BW_GPU_CAPTURE_OK &&
         bw_gpu_capture_expect_memory(capture, 4, bytes, sizeof bytes) == BW_GPU_CAPTURE_OK &&
         bw_gpu_capture_write_file(capture, path) == BW_GPU_CAPTURE_OK;
    bw_gpu_capture_destroy(capture);
    return ok;
}

static int run_child(const char *program, const char *action, const char *path, int audit)
{
    pid_t pid = fork();
    int status;
    if (pid < 0)
        return 0;
    if (!pid) {
        char *const args[] = { (char *)program, (char *)action, (char *)path,
                               (char *)(audit ? "1" : "0"), NULL };
        execv(program, args);
        _exit(127);
    }
    do {
        if (waitpid(pid, &status, 0) >= 0)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    } while (errno == EINTR);
    return 0;
}

static void audit_path(char *out, size_t size, const char *path, const char *suffix)
{
    snprintf(out, size, "%s.%s-audit.txt", path, suffix);
}

static int files_equal(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int ca, cb, equal = 1;
    if (!fa || !fb)
        equal = 0;
    else {
        do {
            ca = fgetc(fa);
            cb = fgetc(fb);
            if (ca != cb) {
                equal = 0;
                break;
            }
        } while (ca != EOF);
        if (ferror(fa) || ferror(fb))
            equal = 0;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return equal;
}

static int roundtrip(const char *program, const char *path, int audit)
{
    char first[1024], second[1024];
    int ok;
    audit_path(first, sizeof first, path, "capture");
    audit_path(second, sizeof second, path, "replay");
    ok = run_child(program, "produce", path, audit) &&
         run_child(program, "replay", path, audit);
    if (ok && audit)
        ok = files_equal(first, second);
    unlink(first);
    unlink(second);
    return ok;
}

static int read_file(const char *path, uint8_t **bytes, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long length;
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) || (length = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return 0;
    }
    *size = (size_t)length;
    *bytes = (uint8_t *)malloc(*size);
    if (!*bytes || fread(*bytes, 1, *size, f) != *size) {
        free(*bytes);
        *bytes = NULL;
        fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

static int replace_file(const char *path, const uint8_t *bytes, size_t size)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f)
        return 0;
    ok = fwrite(bytes, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void fix_checksum(uint8_t *bytes, size_t size)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t i;
    for (i = 0; i < size - 16; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    put32(bytes + size - 8, (uint32_t)hash);
    put32(bytes + size - 4, (uint32_t)(hash >> 32));
}

static size_t find_event(const uint8_t *bytes, size_t size, uint32_t type, uint32_t address)
{
    size_t pos = 32;
    while (pos + 8 <= size - 16) {
        uint32_t n = get32(bytes + pos + 4);
        if (n > size - 16 - pos - 8)
            return 0;
        if (get32(bytes + pos) == type &&
            (address == UINT32_MAX || (n >= 4 && get32(bytes + pos + 8) == address)))
            return pos;
        pos += 8u + n;
    }
    return 0;
}

static size_t remove_event(uint8_t *bytes, size_t size, size_t pos)
{
    size_t removed = 8u + get32(bytes + pos + 4);
    memmove(bytes + pos, bytes + pos + removed, size - pos - removed);
    size -= removed;
    put32(bytes + 16, get32(bytes + 16) - 1u);
    put32(bytes + 20, get32(bytes + 20) - (uint32_t)removed);
    fix_checksum(bytes, size);
    return size;
}

static int rejected_without_callbacks(const char *path, BwGpuCaptureStatus expected)
{
    SimpleReplay replay = {0};
    uint32_t failure = 0;
    BwGpuCaptureStatus status =
        bw_gpu_capture_replay_file(path, &s_simple_ops, &replay, &failure);
    return status == expected && !replay.preflight_count && !replay.execute_count &&
           failure == UINT32_MAX;
}

static int bounds_test(const char *path)
{
    const uint8_t data[4] = {1, 2, 3, 4};
    BwGpuCapture *capture;
    CHECK(!bw_gpu_capture_create(BW_GPU_CAPTURE_MAX_BYTES + 1u, 1));
    CHECK(!bw_gpu_capture_create(1024, BW_GPU_CAPTURE_MAX_EVENTS + 1u));
    CHECK(!bw_gpu_capture_create(1024, 0));
    capture = bw_gpu_capture_create(1024, 1);
    CHECK(capture);
    CHECK(bw_gpu_capture_method(capture, 0, 0x0208, 8) == BW_GPU_CAPTURE_OK);
    CHECK(bw_gpu_capture_method(capture, 0, 0x020C, 16) == BW_GPU_CAPTURE_LIMIT);
    CHECK(bw_gpu_capture_memory(capture, 4, data, 4) == BW_GPU_CAPTURE_LIMIT);
    CHECK(bw_gpu_capture_write_file(capture, path) == BW_GPU_CAPTURE_LIMIT);
    CHECK(access(path, F_OK) != 0);
    bw_gpu_capture_destroy(capture);

    capture = bw_gpu_capture_create(68, 3);
    CHECK(capture);
    CHECK(bw_gpu_capture_method(capture, 0, 0x0208, 8) == BW_GPU_CAPTURE_OK);
    CHECK(bw_gpu_capture_barrier(capture, BW_GPU_CAPTURE_BARRIER_EXECUTOR) ==
          BW_GPU_CAPTURE_LIMIT);
    CHECK(bw_gpu_capture_write_file(capture, path) == BW_GPU_CAPTURE_LIMIT);
    bw_gpu_capture_destroy(capture);

    capture = bw_gpu_capture_create(1024, 4);
    CHECK(capture);
    CHECK(bw_gpu_capture_memory(capture, UINT32_MAX - 1u, data, 4) ==
          BW_GPU_CAPTURE_ARGUMENT);
    CHECK(bw_gpu_capture_write_file(capture, path) == BW_GPU_CAPTURE_ARGUMENT);
    bw_gpu_capture_destroy(capture);

    capture = bw_gpu_capture_create(1024, 4);
    CHECK(capture);
    CHECK(bw_gpu_capture_semaphore_target(capture, 3) == BW_GPU_CAPTURE_ARGUMENT);
    bw_gpu_capture_destroy(capture);
    capture = bw_gpu_capture_create(1024, 4);
    CHECK(capture);
    CHECK(bw_gpu_capture_barrier(capture, (BwGpuCaptureBarrier)99) == BW_GPU_CAPTURE_ARGUMENT);
    bw_gpu_capture_destroy(capture);

    capture = bw_gpu_capture_create(1024, 4);
    CHECK(capture && bw_gpu_capture_method(capture, 0, 0x0208, 8) == BW_GPU_CAPTURE_OK);
    CHECK(!bw_gpu_capture_fixture_begin(capture));
    CHECK(bw_gpu_capture_destroy(capture) == BW_GPU_CAPTURE_OK);

    capture = bw_gpu_capture_create(1024, 4);
    CHECK(capture && bw_gpu_capture_fixture_begin(capture));
    CHECK(bw_gpu_capture_destroy(capture) == BW_GPU_CAPTURE_ARGUMENT);
    CHECK(bw_gpu_capture_write_file(capture, path) == BW_GPU_CAPTURE_ARGUMENT);
    CHECK(bw_gpu_capture_fixture_end() == BW_GPU_CAPTURE_ARGUMENT);
    CHECK(bw_gpu_capture_destroy(capture) == BW_GPU_CAPTURE_OK);
    CHECK(access(path, F_OK) != 0);
    return 1;
}

static int privacy_test(const char *path)
{
    char alias[1024], symlink_path[1024], fifo_path[1024], wrong_suffix[1024];
    struct stat st;
    SimpleReplay replay = {0};
    BwGpuCapture *capture;
    int fd;
    snprintf(alias, sizeof alias, "%s-hardlink.bwgpu", path);
    snprintf(symlink_path, sizeof symlink_path, "%s-symlink.bwgpu", path);
    snprintf(fifo_path, sizeof fifo_path, "%s-fifo.bwgpu", path);
    snprintf(wrong_suffix, sizeof wrong_suffix, "%s.txt", path);
    CHECK(sample_file(path));
    CHECK(stat(path, &st) == 0 && S_ISREG(st.st_mode) && !(st.st_mode & 077) &&
          st.st_uid == geteuid() && st.st_nlink == 1);
    CHECK(bw_gpu_capture_replay_file(path, &s_simple_ops, &replay, NULL) ==
          BW_GPU_CAPTURE_OK);
    CHECK(replay.preflight_count == 5 && replay.execute_count == 5 && replay.semaphore == 8);
    capture = bw_gpu_capture_create(1024, 4);
    CHECK(capture && bw_gpu_capture_method(capture, 0, 0x0208, 8) == BW_GPU_CAPTURE_OK);
    CHECK(bw_gpu_capture_write_file(capture, path) == BW_GPU_CAPTURE_IO);
    bw_gpu_capture_destroy(capture);
    CHECK(stat(path, &st) == 0 && st.st_size > 0);
    CHECK(symlink(path, symlink_path) == 0);
    CHECK(rejected_without_callbacks(symlink_path, BW_GPU_CAPTURE_IO));
    CHECK(link(path, alias) == 0);
    CHECK(rejected_without_callbacks(path, BW_GPU_CAPTURE_IO));
    CHECK(unlink(alias) == 0);
    CHECK(chmod(path, 0644) == 0);
    CHECK(rejected_without_callbacks(path, BW_GPU_CAPTURE_IO));
    CHECK(chmod(path, 0600) == 0);
    CHECK(mkfifo(fifo_path, 0600) == 0);
    CHECK(rejected_without_callbacks(fifo_path, BW_GPU_CAPTURE_IO));

    capture = bw_gpu_capture_create(1024, 4);
    CHECK(capture && bw_gpu_capture_method(capture, 0, 0x0208, 8) == BW_GPU_CAPTURE_OK);
    CHECK(bw_gpu_capture_write_file(capture, wrong_suffix) == BW_GPU_CAPTURE_ARGUMENT);
    bw_gpu_capture_destroy(capture);
    fd = open(symlink_path, O_RDONLY | O_NOFOLLOW);
    CHECK(fd < 0);
    CHECK(unlink(symlink_path) == 0);
    CHECK(unlink(fifo_path) == 0);
    return 1;
}

static int corrupt_test(const char *path)
{
    uint8_t *original, *work;
    size_t size, method, memory, barrier;
    int fd;
    CHECK(sample_file(path) && read_file(path, &original, &size));
    work = (uint8_t *)malloc(size + 1u);
    CHECK(work);
    method = find_event(original, size, BW_GPU_CAPTURE_METHOD, UINT32_MAX);
    memory = find_event(original, size, BW_GPU_CAPTURE_MEMORY, UINT32_MAX);
    barrier = find_event(original, size, BW_GPU_CAPTURE_BARRIER, UINT32_MAX);
    CHECK(method && memory && barrier);
    for (unsigned which = 0; which < 11; which++) {
        size_t length = size;
        memcpy(work, original, size);
        switch (which) {
        case 0: work[method + 16] ^= 1; break; /* bad checksum */
        case 1: length--; break;
        case 2: work[size] = 0; length++; break;
        case 3: put32(work + 12, 2); fix_checksum(work, size); break;
        case 4: put32(work + 24, 1); fix_checksum(work, size); break;
        case 5: put32(work + method, 99); fix_checksum(work, size); break;
        case 6: put32(work + method + 8, 8); fix_checksum(work, size); break;
        case 7: put32(work + method + 12, 3); fix_checksum(work, size); break;
        case 8: put32(work + memory + 8, UINT32_MAX); fix_checksum(work, size); break;
        case 9: put32(work + memory + 12, 99); fix_checksum(work, size); break;
        case 10: put32(work + barrier + 8, 99); fix_checksum(work, size); break;
        }
        CHECK(replace_file(path, work, length));
        CHECK(rejected_without_callbacks(path, BW_GPU_CAPTURE_FORMAT));
    }
    CHECK(replace_file(path, original, size));
    fd = open(path, O_WRONLY);
    CHECK(fd >= 0 && ftruncate(fd, (off_t)BW_GPU_CAPTURE_MAX_BYTES + 1) == 0);
    CHECK(close(fd) == 0);
    CHECK(rejected_without_callbacks(path, BW_GPU_CAPTURE_LIMIT));
    free(work);
    free(original);
    return 1;
}

static int unsupported_test(const char *path)
{
    BwGpuCapture *capture = bw_gpu_capture_create(1024, 8);
    uint8_t bytes[4] = {1, 2, 3, 4};
    CHECK(capture);
    CHECK(bw_gpu_capture_memory(capture, 0x10000, bytes, 4) == BW_GPU_CAPTURE_OK);
    CHECK(bw_gpu_capture_method(capture, 0, 0x1FFC, 1) == BW_GPU_CAPTURE_OK);
    CHECK(bw_gpu_capture_write_file(capture, path) == BW_GPU_CAPTURE_OK);
    bw_gpu_capture_destroy(capture);
    CHECK(fixture_replay_rejection(path, BW_GPU_CAPTURE_UNSUPPORTED));
    return 1;
}

static int missing_input_test(const char *program, const char *path)
{
    uint8_t *original, *work;
    size_t size, pos, smaller;
    CHECK(run_child(program, "produce", path, 0));
    CHECK(read_file(path, &original, &size));
    work = (uint8_t *)malloc(size);
    CHECK(work);
    for (unsigned which = 0; which < 3; which++) {
        memcpy(work, original, size);
        pos = which == 0 ? find_event(work, size, BW_GPU_CAPTURE_MEMORY, 0x20000u)
            : which == 1 ? find_event(work, size, BW_GPU_CAPTURE_BARRIER, UINT32_MAX)
                         : find_event(work, size, BW_GPU_CAPTURE_SEMAPHORE_TARGET, 0x30000u);
        CHECK(pos);
        smaller = remove_event(work, size, pos);
        CHECK(replace_file(path, work, smaller));
        CHECK(fixture_replay_rejection(path, BW_GPU_CAPTURE_UNSUPPORTED));
    }
    free(work);
    free(original);
    return 1;
}

static int mismatch_test(const char *program, const char *path)
{
    uint8_t *bytes;
    size_t size, pos;
    CHECK(run_child(program, "produce", path, 0));
    CHECK(read_file(path, &bytes, &size));
    pos = find_event(bytes, size, BW_GPU_CAPTURE_EXPECT_MEMORY, UINT32_MAX);
    CHECK(pos);
    bytes[pos + 16] ^= 1;
    fix_checksum(bytes, size);
    CHECK(replace_file(path, bytes, size));
    free(bytes);
    CHECK(run_child(program, "replay-mismatch", path, 0));
    return 1;
}

int main(int argc, char **argv)
{
    char path[512];
    int ok = 0;
    if (argc == 4) {
        int audit = !strcmp(argv[3], "1");
        if (!strcmp(argv[1], "produce"))
            return fixture_capture(argv[2], audit) ? 0 : 1;
        if (!strcmp(argv[1], "replay"))
            return fixture_replay(argv[2], audit, 0) ? 0 : 1;
        if (!strcmp(argv[1], "replay-mismatch"))
            return fixture_replay(argv[2], audit, 1) ? 0 : 1;
        return 2;
    }
    if (argc != 2)
        return 2;
    snprintf(path, sizeof path, "gpu-capture-%s-%ld.bwgpu", argv[1], (long)getpid());
    if (!strcmp(argv[1], "roundtrip"))
        ok = roundtrip(argv[0], path, 0);
    else if (!strcmp(argv[1], "audit-roundtrip"))
        ok = roundtrip(argv[0], path, 1);
    else if (!strcmp(argv[1], "bounds"))
        ok = bounds_test(path);
    else if (!strcmp(argv[1], "privacy"))
        ok = privacy_test(path);
    else if (!strcmp(argv[1], "corrupt"))
        ok = corrupt_test(path);
    else if (!strcmp(argv[1], "unsupported"))
        ok = unsupported_test(path);
    else if (!strcmp(argv[1], "missing-input"))
        ok = missing_input_test(argv[0], path);
    else if (!strcmp(argv[1], "mismatch"))
        ok = mismatch_test(argv[0], path);
    else if (!strcmp(argv[1], "concurrent-owner"))
        ok = fixture_thread_violation();
    else if (!strcmp(argv[1], "late-attach"))
        ok = fixture_late_attach();
    else if (!strcmp(argv[1], "bulk-disabled"))
        ok = fixture_bulk_disabled();
    else
        return 2;
    unlink(path);
    return ok ? 0 : 1;
}

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bw_gpu_audit.h"
#include "platform/win32_compat.h"

static ULONGLONG s_now = 1000;
static const char s_path[] = "metal-audit-test.txt";
static atomic_int s_start, s_init_failed;

ULONGLONG GetTickCount64(void) { return s_now; }

static void cleanup(void) { remove(s_path); }

static void *check_init(void *unused)
{
    (void)unused;
    while (!atomic_load(&s_start))
        ;
    if (!bw_audit_on())
        atomic_store(&s_init_failed, 1);
    return NULL;
}

static void *write_gamma(void *unused)
{
    uint8_t ramp[768];
    (void)unused;
    for (unsigned i = 0; i < 3000; i++) {
        memset(ramp, i & 1 ? 0 : 255, sizeof ramp);
        bw_audit_gamma(ramp);
    }
    return NULL;
}

int main(void)
{
    pthread_t thread, initializers[4];
    char text[8192];
    size_t n;
    FILE *f;
    remove(s_path);
    atexit(cleanup);
    setenv("BW_GPU_AUDIT", "1", 1);
    setenv("BW_GPU_AUDIT_EVERY", "1", 1);
    setenv("BW_GPU_AUDIT_FILE", s_path, 1);
    for (unsigned i = 0; i < 4; i++)
        if (pthread_create(&initializers[i], NULL, check_init, NULL))
            return 1;
    atomic_store(&s_start, 1);
    for (unsigned i = 0; i < 4; i++)
        pthread_join(initializers[i], NULL);
    if (atomic_load(&s_init_failed) || !bw_audit_on())
        return 1;
    bw_audit_method(0, 0x0208, 8);
    bw_audit_clear(0xF0, 16, 12, NULL);
    bw_audit_flip(0x10000, 8);
    if (pthread_create(&thread, NULL, write_gamma, NULL))
        return 1;
    for (unsigned i = 0; i < 3000; i++)
        bw_audit_flip(0x10000, 8);
    pthread_join(thread, NULL);
    s_now = 3000;
    bw_audit_flip(0x10000, 8);
    f = fopen(s_path, "rb");
    if (!f)
        return 1;
    n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = '\0';
    if (!strstr(text, "GPU audit") || !strstr(text, "0208")) {
        fprintf(stderr, "audit report is missing expected data\n");
        return 1;
    }
    return 0;
}

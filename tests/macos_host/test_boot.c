#define _DARWIN_C_SOURCE 1
#include "bw_macos_boot.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

ptrdiff_t g_xbox_mem_offset;

static int fail(const char *what)
{
    fprintf(stderr, "boot test failed: %s\n", what);
    return 1;
}

static int run_child(void)
{
    BwMacosLaunch received;
    int has_launch;
    const char *mode = getenv("BW_TEST_BOOT_MODE");

    if (mode && !strcmp(mode, "malformed")) {
        const char *s = getenv("BW_NATIVE_LAUNCH_OUT_FD");
        int fd = s ? atoi(s) : -1;
        return fd >= 0 && write(fd, "bad", 3) == 3 ? 0 : 1;
    }
    if (bw_macos_boot_receive(&received, &has_launch) != 0)
        return 2;
    if (mode && !strcmp(mode, "exit-error"))
        return 7;
    if (has_launch)
        return !bw_macos_launch_is_gameplay(&received) ||
               strcmp(received.path, "D:\\default.xbe") ||
               received.data[42] != 0x7a;

    uint8_t *guest = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANON, -1, 0);
    if (guest == MAP_FAILED)
        return 3;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest;
    memcpy(guest + 0x1000, "D:\\default.xbe", 15);
    memcpy(guest + 0x2000, "SPLASH", 6);
    guest[0x2008] = 1;
    guest[0x2000 + 42] = 0x7a;
    if (mode && !strcmp(mode, "bad-pointer")) {
        int rc;
        if (mprotect(guest + 0x4000, 0x4000, PROT_NONE) != 0)
            return 10;
        rc = bw_macos_record_relaunch(0x4000, 0x2000);
        munmap(guest, 65536);
        return rc == 0 ? 8 : 9;
    }
    int rc = bw_macos_record_relaunch(0x1000, 0x2000);
    munmap(guest, 65536);
    return rc == 0 ? 0 : 4;
}

static int test_format(void)
{
    BwMacosLaunch in = {0}, out, sentinel;
    uint8_t data[BW_MACOS_LAUNCH_FILE_MAX + 1], page[4096];
    FILE *f;
    size_t n;

    strcpy(in.path, "D:\\default.xbe");
    in.data_len = BW_MACOS_LAUNCH_DATA_BYTES;
    memcpy(in.data, "SPLASH", 6);
    in.data[8] = 1;
    in.data[42] = 0x7a;
    if (bw_macos_launch_encode(&in, data, sizeof(data), &n) ||
        bw_macos_launch_decode(data, n, &out) ||
        memcmp(&in, &out, sizeof(in)))
        return fail("BWRL round trip");
    if (!bw_macos_launch_is_gameplay(&out) || bw_macos_launch_page(&out, page) ||
        memcmp(page + 8, "\\Device\\CdRom0;default.xbe", 27) ||
        memcmp(page + 0x400, in.data, in.data_len) ||
        page[4] != 0x10 || page[5] != 0x00 || page[6] != 0x53 || page[7] != 0x4d)
        return fail("launch page or gameplay");
    if (bw_macos_launch_decode(data, n - 1, &out) == 0 ||
        bw_macos_launch_decode(data, n + 1, &out) == 0)
        return fail("truncated or trailing data");
    f = fopen("bw-launch-fixture.bin", "wb");
    if (!f || fwrite(data, 1, n, f) != n || fclose(f) ||
        bw_macos_launch_read_file("bw-launch-fixture.bin", &out) ||
        memcmp(&in, &out, sizeof(in)))
        return fail("launch file");
    unlink("bw-launch-fixture.bin");
    if (mkfifo("bw-launch-fixture.fifo", 0600) != 0 ||
        bw_macos_launch_read_file("bw-launch-fixture.fifo", &out) == 0)
        return fail("non-regular launch file");
    unlink("bw-launch-fixture.fifo");
    sentinel = in;
    data[4] = 2;
    if (bw_macos_launch_decode(data, n, &sentinel) == 0 ||
        memcmp(&sentinel, &in, sizeof(in)))
        return fail("bad version or output mutated");
    data[4] = 1;
    data[12] = 0;
    if (bw_macos_launch_decode(data, n, &out) == 0)
        return fail("embedded path NUL");
    data[12] = 'D';
    if (bw_macos_launch_encode(&in, data, n - 1, &n) == 0)
        return fail("short output");
    strcpy(out.path, "D:\\other.xbe");
    out.data_len = 0;
    if (bw_macos_launch_page(&out, page) == 0)
        return fail("wrong title accepted");
    in.data_len = 1;
    if (bw_macos_launch_encode(&in, data, sizeof(data), &n) == 0)
        return fail("bad launch data size");
    in.data_len = 0;
    if (bw_macos_launch_encode(&in, data, sizeof(data), &n) ||
        bw_macos_launch_decode(data, n, &out) ||
        out.data_len || bw_macos_launch_is_gameplay(&out))
        return fail("empty launch data");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--bw-native-child"))
        return run_child();
    if (test_format())
        return 1;
    unsetenv("BW_LAUNCH_DATA_FILE");
    setenv("BW_TEST_BOOT_MODE", "normal", 1);
    setenv("BW_HOST_BOOTS", "2", 1);
    if (bw_macos_supervise(argc, argv) != 0)
        return fail("supervised relaunch");
    setenv("BW_HOST_BOOTS", "1", 1);
    if (bw_macos_supervise(argc, argv) == 0)
        return fail("boot cap");
    setenv("BW_HOST_BOOTS", "2", 1);
    setenv("BW_TEST_BOOT_MODE", "malformed", 1);
    if (bw_macos_supervise(argc, argv) == 0)
        return fail("invalid relaunch frame");
    setenv("BW_TEST_BOOT_MODE", "bad-pointer", 1);
    if (bw_macos_supervise(argc, argv) != 9)
        return fail("invalid guest pointer");
    setenv("BW_TEST_BOOT_MODE", "exit-error", 1);
    if (bw_macos_supervise(argc, argv) != 7)
        return fail("child exit status");
    puts("boot format and supervised relaunch: synthetic checks passed");
    return 0;
}

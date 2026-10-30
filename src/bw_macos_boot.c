#define _DARWIN_C_SOURCE 1
#include "bw_macos_boot.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
extern ptrdiff_t g_xbox_mem_offset;

static int s_relaunch_fd = -1;
static unsigned s_relaunch_recorded;

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void write_le32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
    p[2] = (uint8_t)(n >> 16);
    p[3] = (uint8_t)(n >> 24);
}

int bw_macos_launch_decode(const uint8_t *bytes, size_t size, BwMacosLaunch *out)
{
    BwMacosLaunch launch = {0};
    uint32_t path_len, data_len;
    size_t off;

    if (!bytes || !out || size < 16 || memcmp(bytes, "BWRL", 4) ||
        read_le32(bytes + 4) != 1)
        return -1;
    path_len = read_le32(bytes + 8);
    if (path_len > BW_MACOS_LAUNCH_PATH_MAX ||
        size < 16u + (size_t)path_len ||
        memchr(bytes + 12, 0, path_len))
        return -1;
    memcpy(launch.path, bytes + 12, path_len);
    off = 12u + path_len;
    data_len = read_le32(bytes + off);
    off += 4;
    if ((data_len != 0 && data_len != BW_MACOS_LAUNCH_DATA_BYTES) ||
        size != off + data_len)
        return -1;
    launch.data_len = data_len;
    memcpy(launch.data, bytes + off, data_len);
    *out = launch;
    return 0;
}

int bw_macos_launch_encode(const BwMacosLaunch *launch, uint8_t *bytes,
                           size_t capacity, size_t *written)
{
    size_t path_len, size, off;
    if (!launch || !bytes || !written)
        return -1;
    path_len = strnlen(launch->path, sizeof(launch->path));
    if (path_len > BW_MACOS_LAUNCH_PATH_MAX ||
        (launch->data_len != 0 && launch->data_len != BW_MACOS_LAUNCH_DATA_BYTES))
        return -1;
    size = 16u + path_len + launch->data_len;
    if (capacity < size)
        return -1;
    memcpy(bytes, "BWRL", 4);
    write_le32(bytes + 4, 1);
    write_le32(bytes + 8, (uint32_t)path_len);
    memcpy(bytes + 12, launch->path, path_len);
    off = 12u + path_len;
    write_le32(bytes + off, launch->data_len);
    memcpy(bytes + off + 4, launch->data, launch->data_len);
    *written = size;
    return 0;
}

static int read_bounded(int fd, uint8_t *bytes, size_t capacity, size_t *length)
{
    size_t used = 0;
    for (;;) {
        ssize_t n = read(fd, bytes + used, capacity - used);
        if (n > 0) {
            used += (size_t)n;
            if (used == capacity)
                return -1;
        } else if (n == 0) {
            *length = used;
            return 0;
        } else if (errno != EINTR) {
            return -1;
        }
    }
}

static int write_all(int fd, const uint8_t *bytes, size_t length)
{
    while (length) {
        ssize_t n = write(fd, bytes, length);
        if (n > 0) {
            bytes += (size_t)n;
            length -= (size_t)n;
        } else if (n == 0 || errno != EINTR) {
            return -1;
        }
    }
    return 0;
}

int bw_macos_launch_read_file(const char *path, BwMacosLaunch *out)
{
    uint8_t bytes[BW_MACOS_LAUNCH_FILE_MAX + 1];
    struct stat st;
    size_t n;
    int fd, rc;
    if (!path || !out || (fd = open(path, O_RDONLY | O_NONBLOCK)) < 0)
        return -1;
    rc = fstat(fd, &st) == 0 && S_ISREG(st.st_mode)
         ? read_bounded(fd, bytes, sizeof(bytes), &n) : -1;
    if (close(fd) != 0)
        rc = -1;
    return rc == 0 ? bw_macos_launch_decode(bytes, n, out) : -1;
}

static int title_path(const char *path, char *device, size_t capacity)
{
    const char *name = path;
    int n;
    if (!strncasecmp(path, "D:\\", 3) || !strncasecmp(path, "D:/", 3))
        name = path + 3;
    else if (!strncasecmp(path, "\\Device\\CdRom0;", 15))
        name = path + 15;
    if (strcasecmp(name, "default.xbe"))
        return -1;
    n = snprintf(device, capacity, "\\Device\\CdRom0;%s", name);
    return n >= 0 && (size_t)n < capacity ? 0 : -1;
}

int bw_macos_launch_page(const BwMacosLaunch *launch, uint8_t page[4096])
{
    char device[520];
    if (!launch || !page ||
        (launch->data_len != 0 && launch->data_len != BW_MACOS_LAUNCH_DATA_BYTES) ||
        strnlen(launch->path, sizeof(launch->path)) > BW_MACOS_LAUNCH_PATH_MAX ||
        title_path(launch->path, device, sizeof(device)) != 0)
        return -1;
    memset(page, 0, 4096);
    write_le32(page + 4, 0x4d530010u);
    memcpy(page + 8, device, strlen(device) + 1);
    memcpy(page + 0x400, launch->data, launch->data_len);
    return 0;
}

int bw_macos_launch_is_gameplay(const BwMacosLaunch *launch)
{
    return launch && launch->data_len == BW_MACOS_LAUNCH_DATA_BYTES &&
           !memcmp(launch->data, "SPLASH", 6) &&
           read_le32(launch->data + 8) == 1;
}

static int pipe_fd(const char *name)
{
    const char *s = getenv(name);
    struct stat st;
    char *end;
    long fd;
    if (!s || !*s)
        return -1;
    errno = 0;
    fd = strtol(s, &end, 10);
    if (errno || *end || fd < 3 || fd > INT_MAX ||
        fstat((int)fd, &st) != 0 || !S_ISFIFO(st.st_mode))
        return -1;
    return (int)fd;
}

int bw_macos_boot_receive(BwMacosLaunch *launch, int *has_launch)
{
    uint8_t bytes[BW_MACOS_LAUNCH_FILE_MAX + 1];
    size_t n;
    int input = pipe_fd("BW_NATIVE_LAUNCH_IN_FD");
    s_relaunch_fd = pipe_fd("BW_NATIVE_LAUNCH_OUT_FD");
    unsetenv("BW_NATIVE_LAUNCH_IN_FD");
    unsetenv("BW_NATIVE_LAUNCH_OUT_FD");
    unsetenv("BW_NATIVE_CHILD");
    if (!launch || !has_launch || input < 0 || s_relaunch_fd < 0 ||
        input == s_relaunch_fd)
        return -1;
    {
        int flags = fcntl(s_relaunch_fd, F_GETFD);
        if (flags < 0 || fcntl(s_relaunch_fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
            close(input);
            close(s_relaunch_fd);
            s_relaunch_fd = -1;
            return -1;
        }
    }
    if (read_bounded(input, bytes, sizeof(bytes), &n) != 0) {
        close(input);
        return -1;
    }
    if (close(input) != 0)
        return -1;
    *has_launch = n != 0;
    return !n || bw_macos_launch_decode(bytes, n, launch) == 0 ? 0 : -1;
}

/* Read through Mach instead of dereferencing a guest-supplied pointer. An
 * invalid, unmapped, or protected guest buffer is a failed relaunch, not a
 * host fault inside the launch hook. */
static int copy_guest(uint32_t va, void *dest, size_t length)
{
    uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    mach_vm_size_t copied = 0;
    if (!va || length > UINT32_MAX || (uint64_t)va + length > 0x100000000ull ||
        base > UINTPTR_MAX - va || length > UINTPTR_MAX - (base + va))
        return -1;
    return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(base + va),
                                  (mach_vm_size_t)length, (mach_vm_address_t)dest,
                                  &copied) == KERN_SUCCESS && copied == length
           ? 0 : -1;
}

int bw_macos_record_relaunch(uint32_t path_va, uint32_t data_va)
{
    BwMacosLaunch launch = {0};
    uint8_t bytes[BW_MACOS_LAUNCH_FILE_MAX];
    size_t i, n;

    if (s_relaunch_fd < 0 ||
        __atomic_exchange_n(&s_relaunch_recorded, 1, __ATOMIC_ACQ_REL))
        return -1;
    for (i = 0; i <= BW_MACOS_LAUNCH_PATH_MAX; i++) {
        if ((uint64_t)path_va + i > UINT32_MAX ||
            copy_guest(path_va + (uint32_t)i, &launch.path[i], 1) != 0)
            return -1;
        if (!launch.path[i])
            break;
    }
    if (i > BW_MACOS_LAUNCH_PATH_MAX)
        return -1;
    if (data_va) {
        launch.data_len = BW_MACOS_LAUNCH_DATA_BYTES;
        if (copy_guest(data_va, launch.data, launch.data_len) != 0)
            return -1;
    }
    /* Reject another title rather than silently rebooting this executable. */
    {
        uint8_t page[4096];
        if (bw_macos_launch_page(&launch, page) != 0)
            return -1;
    }
    if (bw_macos_launch_encode(&launch, bytes, sizeof(bytes), &n) != 0 ||
        write_all(s_relaunch_fd, bytes, n) != 0)
        return -1;
    if (close(s_relaunch_fd) != 0)
        return -1;
    s_relaunch_fd = -1;
    return 0;
}

static int set_child_env(int in, int out)
{
    char a[24], b[24];
    snprintf(a, sizeof(a), "%d", in);
    snprintf(b, sizeof(b), "%d", out);
    return setenv("BW_NATIVE_CHILD", "1", 1) ||
           setenv("BW_NATIVE_LAUNCH_IN_FD", a, 1) ||
           setenv("BW_NATIVE_LAUNCH_OUT_FD", b, 1) ? -1 : 0;
}

static int wait_child(pid_t pid, int *status)
{
    while (waitpid(pid, status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

int bw_macos_supervise(int argc, char **argv)
{
    BwMacosLaunch launch;
    uint8_t bytes[BW_MACOS_LAUNCH_FILE_MAX + 1];
    char exe[PATH_MAX], *end;
    const char *file = getenv("BW_LAUNCH_DATA_FILE");
    const char *limit_env = getenv("BW_HOST_BOOTS");
    uint32_t exe_size = sizeof(exe);
    long limit = 200;
    int has_launch = file && *file;
    struct sigaction ignored = {0}, previous_pipe;

    (void)argc;
    (void)argv;
    if (has_launch && bw_macos_launch_read_file(file, &launch) != 0) {
        fprintf(stderr, "[BW] cannot read a valid BW_LAUNCH_DATA_FILE\n");
        return 1;
    }
    if (limit_env && *limit_env) {
        errno = 0;
        limit = strtol(limit_env, &end, 10);
        if (errno || *end || limit < 1 || limit > 10000) {
            fprintf(stderr, "[BW] BW_HOST_BOOTS must be between 1 and 10000\n");
            return 1;
        }
    }
    if (_NSGetExecutablePath(exe, &exe_size) != 0) {
        fprintf(stderr, "[BW] cannot locate the native executable\n");
        return 1;
    }
    ignored.sa_handler = SIG_IGN;
    sigemptyset(&ignored.sa_mask);
    if (sigaction(SIGPIPE, &ignored, &previous_pipe) != 0)
        return 1;

    for (long boot = 0; boot < limit; boot++) {
        int in[2] = {-1, -1}, out[2] = {-1, -1}, status, spawn_rc;
        int actions_ready = 0, rc = 1;
        pid_t pid;
        size_t n = 0;
        posix_spawn_file_actions_t actions;
        char *child_argv[] = {exe, "--bw-native-child", NULL};

        if (pipe(in) != 0 || pipe(out) != 0)
            goto boot_error;
        if (set_child_env(in[0], out[1]) != 0)
            goto boot_error;
        if (posix_spawn_file_actions_init(&actions) != 0)
            goto boot_error;
        actions_ready = 1;
        if (posix_spawn_file_actions_addclose(&actions, in[1]) != 0 ||
            posix_spawn_file_actions_addclose(&actions, out[0]) != 0)
            goto boot_error;
        spawn_rc = posix_spawn(&pid, exe, &actions, NULL, child_argv, environ);
        posix_spawn_file_actions_destroy(&actions);
        actions_ready = 0;
        unsetenv("BW_NATIVE_CHILD");
        unsetenv("BW_NATIVE_LAUNCH_IN_FD");
        unsetenv("BW_NATIVE_LAUNCH_OUT_FD");
        if (spawn_rc != 0) {
            fprintf(stderr, "[BW] cannot start native boot: %s\n", strerror(spawn_rc));
            goto boot_error;
        }
        close(in[0]); in[0] = -1;
        close(out[1]); out[1] = -1;
        if (has_launch &&
            (bw_macos_launch_encode(&launch, bytes, sizeof(bytes), &n) != 0 ||
             write_all(in[1], bytes, n) != 0)) {
            (void)kill(pid, SIGTERM);
            close(in[1]); in[1] = -1;
            close(out[0]); out[0] = -1;
            (void)wait_child(pid, &status);
            goto boot_error;
        }
        close(in[1]); in[1] = -1;
        if (read_bounded(out[0], bytes, sizeof(bytes), &n) != 0) {
            (void)kill(pid, SIGTERM);
            close(out[0]); out[0] = -1;
            (void)wait_child(pid, &status);
            goto boot_error;
        }
        close(out[0]); out[0] = -1;
        if (wait_child(pid, &status) != 0)
            goto boot_error;
        if (!WIFEXITED(status)) {
            rc = WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
            goto done;
        }
        rc = WEXITSTATUS(status);
        if (rc != 0 || n == 0)
            goto done;
        if (bw_macos_launch_decode(bytes, n, &launch) != 0) {
            fprintf(stderr, "[BW] invalid native relaunch request\n");
            rc = 1;
            goto done;
        }
        has_launch = 1;
        if (boot + 1 == limit) {
            fprintf(stderr, "[BW] relaunch limit reached (BW_HOST_BOOTS)\n");
            rc = 1;
            goto done;
        }
        continue;

boot_error:
        if (actions_ready)
            posix_spawn_file_actions_destroy(&actions);
        unsetenv("BW_NATIVE_CHILD");
        unsetenv("BW_NATIVE_LAUNCH_IN_FD");
        unsetenv("BW_NATIVE_LAUNCH_OUT_FD");
        if (in[0] >= 0) close(in[0]);
        if (in[1] >= 0) close(in[1]);
        if (out[0] >= 0) close(out[0]);
        if (out[1] >= 0) close(out[1]);
        fprintf(stderr, "[BW] native launch supervision failed\n");
done:
        sigaction(SIGPIPE, &previous_pipe, NULL);
        return rc;
    }
    sigaction(SIGPIPE, &previous_pipe, NULL);
    return 1;
}

#include "bw_edition.h"
#include "bw_framelog.h"
#include "bw_host.h"
#include "bw_profile.h"
#include "bw_settings.h"
#include "bw_wait.h"
#include "platform/host_time.h"
#include "platform/win32_compat.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

ptrdiff_t g_xbox_mem_offset;
void bw_debugflags_start(void);
void bw_state_start(void);

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 0; \
    } \
} while (0)

static void reset_environment(void)
{
    static const char *names[] = {
        "BW_SETTINGS", "BW_SETTINGS_APPLIED", "BW_EDITION", "BW_FIX",
        "BW_EDITION_SAVE_ROOT", "RECOMP_SAVE_DIR", "BW_GAME_DIR", "XDG_DATA_HOME", "XDG_CONFIG_HOME",
        "RECOMP_WINDOW_SCALE", "RECOMP_FULLSCREEN", "BW_ASPECT", "BW_CUSTOM_TEST",
        "BW_ACK_NAP", "BW_ACK_IDLE_US", "BW_ACK_NAP_US", "BW_KICKOFF_EVENT",
        "BW_KICKOFF_STATS", "BW_PB_SEGWALK", "BW_STATE", "BW_STATE_MS",
        "BW_STATE_DUMP", "BW_STATE_WATCH", "BW_STATE_SCAN", "BW_STATE_OBJECTS",
        "BW_STATE_OBJECTIVES", "BW_DEBUG_BYTES", "BW_PROFILE", "BW_PROFILE_FROM",
        "BW_PROFILE_TO", "BW_PROFILE_CALLERS", "BW_PROFILE_LINES", "BW_FRAMELOG",
        "BW_FRAMELOG_SAMPLE", "BW_FRAMELOG_MAXEV", "BW_HOST"
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        unsetenv(names[i]);
}

static int make_dir(const char *path)
{
    return mkdir(path, 0700) == 0;
}

static int write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    int ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

static int file_contains(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    char *data;
    long length;
    int found;
    if (!f || fseek(f, 0, SEEK_END) || (length = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET)) {
        if (f) fclose(f);
        return 0;
    }
    data = (char *)malloc((size_t)length + 1);
    if (!data) {
        fclose(f);
        return 0;
    }
    size_t read = fread(data, 1, (size_t)length, f);
    data[read] = 0;
    found = strstr(data, needle) != NULL;
    free(data);
    fclose(f);
    return found;
}

static void remove_fixture(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *entry;
    if (d) {
        while ((entry = readdir(d)) != NULL) {
            char child[1024];
            struct stat st;
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            if (snprintf(child, sizeof child, "%s/%s", path, entry->d_name) >=
                (int)sizeof child || lstat(child, &st) != 0)
                continue;
            if (S_ISDIR(st.st_mode))
                remove_fixture(child);
            else
                unlink(child);
        }
        closedir(d);
    }
    rmdir(path);
}

static int test_settings(const char *cwd)
{
    char expected[512], err[256];
    CHECK(setenv("HOME", cwd, 1) == 0);
    snprintf(expected, sizeof expected, "%s/.config/BloodWake/settings.ini", cwd);
    CHECK(!strcmp(bw_settings_path(), expected));
    snprintf(expected, sizeof expected, "%s/config/BloodWake/settings.ini", cwd);
    CHECK(setenv("XDG_CONFIG_HOME", "config", 1) == 0);
    CHECK(!strcmp(bw_settings_path(), "config/BloodWake/settings.ini"));
    CHECK(setenv("XDG_CONFIG_HOME", cwd, 1) == 0);
    snprintf(expected, sizeof expected, "%s/BloodWake/settings.ini", cwd);
    CHECK(!strcmp(bw_settings_path(), expected));
    CHECK(setenv("BW_SETTINGS", "none", 1) == 0);
    CHECK(!bw_settings_path());

    CHECK(write_text("choices.ini",
        "[Edition]\nedition=Remastered\n[Display]\nmode=fullscreen\n"
        "window_size=huge\naspect=16:9\n[Advanced]\nBW_CUSTOM_TEST=file-value\n"));
    CHECK(setenv("BW_SETTINGS", "choices.ini", 1) == 0);
    CHECK(setenv("RECOMP_WINDOW_SCALE", "2", 1) == 0);
    CHECK(setenv("BW_FIX", "-WIDESCREEN", 1) == 0);
    bw_settings_apply();
    CHECK(!strcmp(getenv("BW_EDITION"), "remastered"));
    CHECK(!strcmp(getenv("RECOMP_FULLSCREEN"), "1"));
    CHECK(!strcmp(getenv("RECOMP_WINDOW_SCALE"), "2"));
    CHECK(!strcmp(getenv("BW_ASPECT"), "16:9"));
    CHECK(!strcmp(getenv("BW_CUSTOM_TEST"), "file-value"));
    CHECK(strstr(getenv("BW_FIX"), "+WIDESCREEN,-WIDESCREEN") != NULL);
    CHECK(!strcmp(getenv("BW_SETTINGS_APPLIED"), "choices.ini"));
    bw_settings_apply();
    CHECK(!strcmp(getenv("BW_FIX"), "+WIDESCREEN,-WIDESCREEN"));
    CHECK(bw_edition_init(err, sizeof err) == 0);
    CHECK(!bw_fix_enabled(BW_FIX_WIDESCREEN));
    FILE *f = fopen("schema.json", "w");
    CHECK(f != NULL);
    bw_settings_print_schema(f);
    CHECK(fclose(f) == 0);
    CHECK(file_contains("schema.json", "\"presets\""));
    CHECK(file_contains("schema.json", "\"settings\""));
    return 1;
}

static int create_original_saves(void)
{
    CHECK(make_dir("original"));
    CHECK(make_dir("original/UDATA"));
    CHECK(make_dir("original/UDATA/title"));
    CHECK(make_dir("original/TDATA"));
    CHECK(write_text("original/UDATA/title/slot", "synthetic original save"));
    CHECK(write_text("original/TDATA/options", "synthetic original settings"));
    return 1;
}

static int set_save_environment(const char *cwd)
{
    char original[512], partitions[512], xdg[512], err[256];
    snprintf(original, sizeof original, "%s/original", cwd);
    snprintf(partitions, sizeof partitions, "%s/partitions", cwd);
    snprintf(xdg, sizeof xdg, "%s/xdg", cwd);
    CHECK(setenv("BW_EDITION", "remastered", 1) == 0);
    CHECK(setenv("BW_FIX", "none,+BOAT_SPLASH_RANGE,+SOUND_SHIELDHITS_NAME", 1) == 0);
    CHECK(setenv("RECOMP_SAVE_DIR", original, 1) == 0);
    CHECK(setenv("BW_EDITION_SAVE_ROOT", partitions, 1) == 0);
    CHECK(setenv("XDG_DATA_HOME", xdg, 1) == 0);
    CHECK(bw_edition_init(err, sizeof err) == 0);
    return 1;
}

static int test_edition(const char *cwd)
{
    char err[256], original[512], expected[512];
    struct stat st;
    uint8_t *guest;
    const uint8_t before[4] = { 0x81, 0x04, 0x35, 0xbf };
    const uint8_t after[4] = { 0xff, 0xff, 0x7f, 0x7f };
    CHECK(setenv("BW_FIX", "+NOT_A_FIX", 1) == 0);
    CHECK(bw_edition_init(err, sizeof err) == 0);
    CHECK(bw_edition() == BW_ED_ORIGINAL && bw_edition_prepare_saves() == NULL);
    CHECK(setenv("BW_EDITION", "unknown", 1) == 0);
    CHECK(bw_edition_init(err, sizeof err) != 0);
    CHECK(strstr(err, "not an edition") != NULL);
    CHECK(setenv("BW_EDITION", "remastered", 1) == 0);
    CHECK(bw_edition_init(err, sizeof err) != 0);
    CHECK(strstr(err, "no fix named") != NULL);
    for (int i = 0; i < BW_FIX_COUNT; i++)
        CHECK(!g_bw_fix_on[i]);
    CHECK(create_original_saves());
    CHECK(set_save_environment(cwd));

    guest = (uint8_t *)calloc(1, 0x200000);
    CHECK(guest != NULL);
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest;
    memcpy(guest + 0x174944, before, sizeof before);
    memcpy(guest + 0x174b34, "ShieldHitsDreadnought", sizeof "ShieldHitsDreadnought");
    bw_edition_apply_image_fixes(NULL, 0);
    CHECK(!memcmp(guest + 0x174944, after, sizeof after));
    CHECK(!memcmp(guest + 0x174b34, "ShieldHitsDreadnaught", sizeof "ShieldHitsDreadnaught"));
    free(guest);
    g_xbox_mem_offset = 0;

    snprintf(expected, sizeof expected, "%s/partitions/editions/remastered", cwd);
    CHECK(!strcmp(bw_edition_prepare_saves(), expected));
    snprintf(expected, sizeof expected, "%s/original/editions/remastered", cwd);
    CHECK(!strcmp(getenv("RECOMP_SAVE_DIR"), expected));
    CHECK(stat("original/editions/remastered", &st) == 0 && !(st.st_mode & 0077));
    CHECK(stat("original/editions/remastered/UDATA", &st) == 0 && !(st.st_mode & 0077));
    CHECK(file_contains("original/editions/remastered/UDATA/title/slot", "synthetic original save"));
    CHECK(file_contains("original/editions/remastered/TDATA/options", "synthetic original settings"));
    CHECK(file_contains("original/UDATA/title/slot", "synthetic original save"));
    CHECK(write_text("original/editions/remastered/UDATA/title/slot", "edited isolated save"));
    snprintf(original, sizeof original, "%s/original", cwd);
    CHECK(setenv("RECOMP_SAVE_DIR", original, 1) == 0);
    CHECK(bw_edition_prepare_saves() != NULL);
    CHECK(file_contains("original/editions/remastered/UDATA/title/slot", "edited isolated save"));
    CHECK(file_contains("original/UDATA/title/slot", "synthetic original save"));
    return 1;
}

static int child_status_ok(pid_t pid, int expected)
{
    int status;
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == expected;
}

static int test_save_retry(const char *cwd)
{
    struct stat st;
    CHECK(create_original_saves());
    CHECK(set_save_environment(cwd));
    CHECK(symlink("options", "original/TDATA/symlink") == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        bw_edition_prepare_saves();
        _exit(0);
    }
    CHECK(child_status_ok(pid, EXIT_FAILURE));
    CHECK(file_contains("original/editions/remastered/UDATA/title/slot", "synthetic original save"));
    CHECK(lstat("original/editions/remastered/.bw_edition_importing", &st) == 0);
    CHECK(lstat("original/editions/remastered/bw_edition_saves.txt", &st) != 0);
    CHECK(unlink("original/TDATA/symlink") == 0);
    CHECK(bw_edition_prepare_saves() != NULL);
    CHECK(file_contains("original/editions/remastered/TDATA/options", "synthetic original settings"));
    CHECK(lstat("original/editions/remastered/bw_edition_saves.txt", &st) == 0);
    CHECK(lstat("original/editions/remastered/.bw_edition_importing", &st) != 0);
    CHECK(file_contains("original/UDATA/title/slot", "synthetic original save"));
    return 1;
}

static int test_save_race(const char *cwd)
{
    CHECK(create_original_saves());
    CHECK(set_save_environment(cwd));
    pid_t first = fork();
    CHECK(first >= 0);
    if (!first) {
        bw_edition_prepare_saves();
        _exit(0);
    }
    pid_t second = fork();
    CHECK(second >= 0);
    if (!second) {
        bw_edition_prepare_saves();
        _exit(0);
    }
    CHECK(child_status_ok(first, 0));
    CHECK(child_status_ok(second, 0));
    CHECK(file_contains("original/editions/remastered/UDATA/title/slot", "synthetic original save"));
    CHECK(file_contains("original/editions/remastered/TDATA/options", "synthetic original settings"));
    CHECK(file_contains("original/UDATA/title/slot", "synthetic original save"));
    return 1;
}

static int test_save_fallback(const char *cwd)
{
    char game[512], xdg[512], partitions[512], err[256];
    CHECK(make_dir("disc"));
    CHECK(make_dir("disc/UDATA"));
    CHECK(make_dir("disc/TDATA"));
    CHECK(write_text("disc/UDATA/slot", "game-directory user save"));
    CHECK(write_text("disc/TDATA/options", "game-directory title save"));
    CHECK(make_dir("xdg"));
    CHECK(make_dir("xdg/xboxrecomp"));
    CHECK(make_dir("xdg/xboxrecomp/UDATA"));
    CHECK(write_text("xdg/xboxrecomp/UDATA/slot", "toolkit user save"));
    snprintf(game, sizeof game, "%s/disc", cwd);
    snprintf(xdg, sizeof xdg, "%s/xdg", cwd);
    snprintf(partitions, sizeof partitions, "%s/partitions", cwd);
    CHECK(setenv("BW_GAME_DIR", game, 1) == 0);
    CHECK(setenv("XDG_DATA_HOME", xdg, 1) == 0);
    CHECK(setenv("BW_EDITION_SAVE_ROOT", partitions, 1) == 0);
    CHECK(setenv("BW_EDITION", "remastered", 1) == 0);
    CHECK(setenv("BW_FIX", "none", 1) == 0);
    CHECK(bw_edition_init(err, sizeof err) == 0);
    CHECK(bw_edition_prepare_saves() != NULL);
    CHECK(file_contains("partitions/editions/remastered/UDATA/slot", "toolkit user save"));
    CHECK(file_contains("partitions/editions/remastered/TDATA/options", "game-directory title save"));
    CHECK(file_contains("disc/UDATA/slot", "game-directory user save"));
    CHECK(file_contains("disc/TDATA/options", "game-directory title save"));
    return 1;
}

static int test_save_conflict(const char *cwd)
{
    char scratch[256], protected[256];
    CHECK(create_original_saves());
    CHECK(set_save_environment(cwd));
    CHECK(make_dir("original/editions"));
    CHECK(make_dir("original/editions/remastered"));
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        snprintf(scratch, sizeof scratch, "original/editions/remastered/UDATA.import-%ld",
                 (long)getpid());
        snprintf(protected, sizeof protected, "%s/untouched", scratch);
        if (!make_dir(scratch) || !write_text(protected, "pre-existing synthetic content"))
            _exit(2);
        bw_edition_prepare_saves();
        _exit(0);
    }
    CHECK(child_status_ok(pid, EXIT_FAILURE));
    snprintf(protected, sizeof protected, "original/editions/remastered/UDATA.import-%ld/untouched",
             (long)pid);
    CHECK(file_contains(protected, "pre-existing synthetic content"));
    CHECK(file_contains("original/UDATA/title/slot", "synthetic original save"));
    return 1;
}

static void *kick_later(void *unused)
{
    (void)unused;
    xbox_HostSleepNs(40000000);
    bw_ack_kick();
    return NULL;
}

static unsigned s_produced, s_consumed;
static void *queue_producer(void *unused)
{
    (void)unused;
    for (unsigned i = 1; i <= 5000; i++) {
        while (__atomic_load_n(&s_consumed, __ATOMIC_ACQUIRE) != i - 1)
            sched_yield();
        bw_put_queue_push(i);
        __atomic_store_n(&s_produced, i, __ATOMIC_RELEASE);
    }
    return NULL;
}

static int test_wait(void)
{
    pthread_t thread;
    uint64_t start, elapsed;
    CHECK(setenv("BW_ACK_IDLE_US", "0", 1) == 0);
    CHECK(setenv("BW_ACK_NAP_US", "4000000", 1) == 0);
    bw_ack_idle(1);
    CHECK(pthread_create(&thread, NULL, kick_later, NULL) == 0);
    start = xbox_HostNowNs();
    bw_ack_idle(0);
    elapsed = xbox_HostNowNs() - start;
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(elapsed >= 10000000 && elapsed < 2000000000ull);
    bw_ack_kick();
    start = xbox_HostNowNs();
    bw_ack_idle(0);
    CHECK(xbox_HostNowNs() - start < 1000000000ull);
    bw_flush_acked();
    start = xbox_HostNowNs();
    bw_flush_wait();
    CHECK(xbox_HostNowNs() - start < 1000000000ull);

    bw_segwalk_default(1);
    bw_put_queue_push(10);
    bw_put_queue_push(20);
    bw_put_queue_push(30);
    CHECK(bw_put_queue_take(0, 30) == 10);
    CHECK(bw_put_queue_take(10, 30) == 20);
    CHECK(bw_put_queue_take(20, 30) == 30);
    bw_put_queue_push(0xfffffffcu);
    bw_put_queue_push(4);
    CHECK(bw_put_queue_take(0xfffffff8u, 4) == 0xfffffffcu);
    CHECK(bw_put_queue_take(0xfffffffcu, 4) == 4);
    for (unsigned i = 1; i <= 65; i++)
        bw_put_queue_push(i);
    for (unsigned i = 1; i <= 64; i++)
        CHECK(bw_put_queue_take(i - 1, 65) == i);
    CHECK(bw_put_queue_take(64, 65) == 65);
    CHECK(pthread_create(&thread, NULL, queue_producer, NULL) == 0);
    for (unsigned i = 1; i <= 5000; i++) {
        while (__atomic_load_n(&s_produced, __ATOMIC_ACQUIRE) != i)
            sched_yield();
        CHECK(bw_put_queue_take(i - 1, i) == i);
        __atomic_store_n(&s_consumed, i, __ATOMIC_RELEASE);
    }
    CHECK(pthread_join(thread, NULL) == 0);
    return 1;
}

static void put32(uint8_t *memory, unsigned address, uint32_t value)
{
    memcpy(memory + address, &value, sizeof value);
}

static int test_state(void)
{
    uint8_t *memory = (uint8_t *)VirtualAlloc(NULL, 0x04000000u,
                                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(memory != NULL);
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    put32(memory, 0x001cee58u + 0xc0, 1);
    put32(memory, 0x001cee58u + 8, 0x00302000u);
    put32(memory, 0x00302000u + 0x10, 0x00302000u);
    put32(memory, 0x00302000u + 0x20, 5);
    put32(memory, 0x00302000u + 0x24, 10);
    memcpy(memory + 0x00302000u + 0x34, "SyntheticBoat", sizeof "SyntheticBoat");
    CHECK(setenv("BW_STATE", "state.log", 1) == 0);
    CHECK(setenv("BW_STATE_MS", "5", 1) == 0);
    CHECK(setenv("BW_STATE_WATCH", "0x1cee58", 1) == 0);
    CHECK(setenv("BW_STATE_OBJECTS", "1", 1) == 0);
    bw_state_start();
    for (int i = 0; i < 300 && !file_contains("state.log", "SyntheticBoat"); i++)
        xbox_HostSleepNs(10000000);
    CHECK(file_contains("state.log", "# boot "));
    CHECK(file_contains("state.log", "00302000"));
    CHECK(file_contains("state.log", "SyntheticBoat"));
    CHECK(file_contains("state.log", " | W "));
    return 1;
}

static int test_debug(void)
{
    uint8_t *memory = (uint8_t *)VirtualAlloc(NULL, 0x200000u,
                                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(memory != NULL);
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    CHECK(setenv("BW_DEBUG_BYTES", "0x1E690B=7,0x1E6909=1,0x1E6920=99", 1) == 0);
    bw_debugflags_start();
    for (int i = 0; i < 100 && !memory[0x1e690b]; i++)
        xbox_HostSleepNs(1000000);
    CHECK(memory[0x1e690b] == 7 && memory[0x1e6909] == 1);
    CHECK(memory[0x1e6920] == 0);
    return 1;
}

static int s_worker_done;
__attribute__((noinline)) static void sampled_work(void)
{
    while (!__atomic_load_n(&s_worker_done, __ATOMIC_ACQUIRE))
        sched_yield();
}

static void *diagnostic_worker(void *unused)
{
    (void)unused;
    bw_fl_thread(1);
    sampled_work();
    return NULL;
}

static int test_diagnostics(void)
{
    pthread_t worker;
    FILE *f;
    char line[1024];
    int events = 0, game_samples = 0, nv_samples = 0, frames = 0, symbols = 0;
    unsigned long long total = 0;
    CHECK(setenv("BW_FRAMELOG", "frames.log", 1) == 0);
    CHECK(setenv("BW_FRAMELOG_SAMPLE", "500", 1) == 0);
    CHECK(setenv("BW_FRAMELOG_MAXEV", "256", 1) == 0);
    CHECK(setenv("BW_PROFILE", "profile.log", 1) == 0);
    CHECK(setenv("BW_PROFILE_CALLERS", "1", 1) == 0);
    bw_fl_init();
    CHECK(g_bw_fl_on);
    bw_fl_thread(0);
    bw_profile_init();
    CHECK(pthread_create(&worker, NULL, diagnostic_worker, NULL) == 0);
    for (unsigned i = 0; i < 30; i++) {
        bw_fl_event(BW_FL_G_FRAME, i, 0, 0);
        xbox_HostSleepNs(5000000);
    }
    bw_profile_dump("test");
    __atomic_store_n(&s_worker_done, 1, __ATOMIC_RELEASE);
    CHECK(pthread_join(worker, NULL) == 0);
    bw_fl_dump("test");
    f = fopen("profile.log", "r");
    CHECK(f != NULL);
    CHECK(fgets(line, sizeof line, f) != NULL);
    CHECK(sscanf(line, "Blood Wake profile (test): %llu samples", &total) == 1);
    fclose(f);
    CHECK(total > 0);
    CHECK(file_contains("profile.log", "Hottest instruction addresses"));
    CHECK(file_contains("profile.log", "BW_PROFILE_CALLERS"));
    f = fopen("frames.log", "r");
    CHECK(f != NULL);
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "E ", 2))
            events++;
        if (!strncmp(line, "Y ", 2))
            symbols++;
        if (!strncmp(line, "S ", 2)) {
            double timestamp;
            unsigned role;
            unsigned long long leaf, frame;
            int n = sscanf(line, "S %lf %u %llx %llx", &timestamp, &role, &leaf, &frame);
            if (n >= 3 && role == 0) game_samples++;
            if (n >= 3 && role == 1) nv_samples++;
            if (n == 4) frames++;
        }
    }
    fclose(f);
    CHECK(events == 30);
    CHECK(game_samples > 0 && nv_samples > 0 && frames > 0 && symbols > 0);
    return 1;
}

static int test_events_only(void)
{
    CHECK(setenv("BW_FRAMELOG", "events.log", 1) == 0);
    CHECK(setenv("BW_FRAMELOG_SAMPLE", "0", 1) == 0);
    CHECK(setenv("BW_FRAMELOG_MAXEV", "2", 1) == 0);
    bw_fl_init();
    CHECK(g_bw_fl_on);
    bw_fl_thread(0);
    bw_fl_event(BW_FL_G_FRAME, 1, 2, 3);
    bw_fl_event(BW_FL_G_FRAME, 2, 3, 4);
    bw_fl_event(BW_FL_G_FRAME, 3, 4, 5);
    bw_fl_dump("test");
    CHECK(file_contains("events.log", "E "));
    CHECK(file_contains("events.log", "2 events (1 dropped)"));
    CHECK(!file_contains("events.log", "\nS "));
    return 1;
}

static int test_host(void)
{
    CHECK(!bw_host_wanted());
    CHECK(setenv("BW_HOST", "1", 1) == 0);
    CHECK(bw_host_wanted());
    CHECK(bw_host_main() != 0);
    return 1;
}

int main(int argc, char **argv)
{
    char fixture[128], cwd[512];
    int ok = 0;
    if (argc != 2)
        return 2;
    snprintf(fixture, sizeof fixture, "bw-support-%ld", (long)getpid());
    if (!make_dir(fixture) || chdir(fixture) != 0 || !getcwd(cwd, sizeof cwd)) {
        fprintf(stderr, "Could not create the support-test fixture: %s\n", strerror(errno));
        return 2;
    }
    reset_environment();
    if (!strcmp(argv[1], "settings")) ok = test_settings(cwd);
    else if (!strcmp(argv[1], "edition")) ok = test_edition(cwd);
    else if (!strcmp(argv[1], "save_retry")) ok = test_save_retry(cwd);
    else if (!strcmp(argv[1], "save_race")) ok = test_save_race(cwd);
    else if (!strcmp(argv[1], "save_fallback")) ok = test_save_fallback(cwd);
    else if (!strcmp(argv[1], "save_conflict")) ok = test_save_conflict(cwd);
    else if (!strcmp(argv[1], "wait")) ok = test_wait();
    else if (!strcmp(argv[1], "state")) ok = test_state();
    else if (!strcmp(argv[1], "debug")) ok = test_debug();
    else if (!strcmp(argv[1], "diagnostics")) ok = test_diagnostics();
    else if (!strcmp(argv[1], "events_only")) ok = test_events_only();
    else if (!strcmp(argv[1], "host")) ok = test_host();
    else fprintf(stderr, "Unknown support test: %s\n", argv[1]);
    bw_profile_dump("cleanup");
    if (chdir("..") == 0)
        remove_fixture(fixture);
    return ok ? 0 : 1;
}

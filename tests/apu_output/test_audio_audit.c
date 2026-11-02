#include "bw_audio_audit.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NS_PER_MS INT64_C(1000000)

typedef struct {
    _Atomic int64_t now;
} FakeClock;

typedef struct {
    BwAudioAudit *audit;
    int iterations;
    _Atomic int *failed;
} ThreadArgs;

static int check(int condition, const char *description)
{
    if (!condition)
        fprintf(stderr, "FAIL: %s\n", description);
    return condition;
}

static int64_t fake_now(void *context)
{
    FakeClock *clock = context;
    return atomic_load_explicit(&clock->now, memory_order_relaxed);
}

static int nearly(long double actual, long double expected)
{
    return fabsl(actual - expected) < 0.0001L;
}

static int test_signal_aggregates(void)
{
    static const int16_t samples[][2] = {
        { 0, 0 },
        { INT16_MAX, INT16_MIN },
        { 16384, -16384 },
        { 0, 0 },
        { 0, 0 },
        { INT16_MIN, INT16_MAX },
    };
    FakeClock clock = {0};
    BwAudioAudit audit;
    BwAudioAuditSnapshot snapshot;
    long double sum[2] = {0, 0}, squares[2] = {0, 0};
    char report[2048];
    int ok = 1;

    ok &= check(bw_audio_audit_init(&audit, 128, 2, fake_now, &clock, NULL) == 0,
                "initialize deterministic signal audit");
    ok &= check(bw_audio_audit_submit(NULL, &samples[0][0], 6) == -1 &&
                bw_audio_audit_submit(&audit, NULL, 6) == -1 &&
                bw_audio_audit_submit(&audit, &samples[0][0], 0) == -1,
                "reject invalid submissions explicitly");
    ok &= check(bw_audio_audit_submit(&audit, &samples[0][0], 6) == 1,
                "accept synthetic stereo signal");
    ok &= check(bw_audio_audit_snapshot(&audit, &snapshot) == 0,
                "snapshot synthetic signal");
    for (int frame = 0; frame < 6; frame++) {
        for (int channel = 0; channel < 2; channel++) {
            sum[channel] += samples[frame][channel];
            squares[channel] +=
                (long double)samples[frame][channel] * samples[frame][channel];
        }
    }
    ok &= check(snapshot.frames == 6 && snapshot.submitted_frames == 6,
                "aggregate frame count");
    ok &= check(nearly(snapshot.sum[0], sum[0]) &&
                nearly(snapshot.sum[1], sum[1]) &&
                nearly(snapshot.sum_squares[0], squares[0]) &&
                nearly(snapshot.sum_squares[1], squares[1]),
                "per-channel DC and RMS accumulators");
    ok &= check(snapshot.peak[0] == 32768 && snapshot.peak[1] == 32768,
                "per-channel peak");
    ok &= check(snapshot.clipped[0] == 2 && snapshot.clipped[1] == 2,
                "per-channel clipping count");
    ok &= check(snapshot.silence_frames == 3 &&
                snapshot.silence_longest_frames == 2 &&
                snapshot.silence_current_frames == 0,
                "silence durations");
    ok &= check(bw_audio_audit_format(&snapshot, 0, report, sizeof(report)) == 0,
                "format aggregate-only report");
    ok &= check(strstr(report, "audible=no") &&
                strstr(report, "format=s16-stereo-48000") &&
                strstr(report, "frames=6") &&
                strstr(report, "L_clipped=2") &&
                strstr(report, "R_clipped=2") &&
                strstr(report, "submitted_hz=0.000") &&
                strstr(report, "consumed_hz=0.000"),
                "report identifies silent sink and required aggregates");
    ok &= check(bw_audio_audit_finish(&audit) == 0,
                "finish deterministic signal audit");
    return ok;
}

static int test_cadence_and_queue_errors(void)
{
    int16_t first[480][2] = {{0}};
    int16_t period[240][2] = {{0}};
    int16_t overflow[600][2] = {{0}};
    FakeClock clock;
    BwAudioAudit audit;
    BwAudioAuditSnapshot snapshot;
    uint64_t played;
    int queued;
    int ok = 1;

    atomic_init(&clock.now, INT64_C(1000000000));
    ok &= check(bw_audio_audit_init(&audit, 128, 3, fake_now, &clock, NULL) == 0,
                "initialize cadence audit");
    ok &= check(bw_audio_audit_submit(&audit, &first[0][0], 480) == 1,
                "accept ten milliseconds");
    atomic_fetch_add(&clock.now, 5 * NS_PER_MS);
    ok &= check(bw_audio_audit_queued(&audit, &queued) == 0 && queued == 240,
                "consume exactly 48 kHz after five milliseconds");
    ok &= check(bw_audio_audit_samples_played(&audit, &played) == 0 && played == 240,
                "consumed-frame accounting follows cadence");
    ok &= check(bw_audio_audit_submit(&audit, &period[0][0], 240) == 1,
                "refill bounded queue");

    atomic_fetch_add(&clock.now, 15 * NS_PER_MS);
    ok &= check(bw_audio_audit_samples_played(&audit, &played) == 0 && played == 720,
                "consume queued frames without inventing played samples");
    ok &= check(bw_audio_audit_submit(&audit, &overflow[0][0], 600) == 0,
                "reject whole overflowing submission");
    ok &= check(bw_audio_audit_submit(&audit, &period[0][0], 240) == 1,
                "recover after reported overflow");
    ok &= check(bw_audio_audit_expect_drain(&audit) == 0,
                "announce intentional queue drain");
    atomic_fetch_add(&clock.now, 10 * NS_PER_MS);
    ok &= check(bw_audio_audit_snapshot(&audit, &snapshot) == 0,
                "snapshot cadence metrics");

    ok &= check(snapshot.consumed_frames == 960 && snapshot.queue_frames == 0,
                "48 kHz cadence drains accepted frames");
    ok &= check(snapshot.queue_capacity_frames == 512 &&
                snapshot.queue_max_frames == 480,
                "queue memory and observed latency are bounded");
    ok &= check(snapshot.underflow_events == 1 &&
                snapshot.underflow_frames == 240,
                "unannounced underflow duration is counted");
    ok &= check(snapshot.overflow_events == 1 &&
                snapshot.overflow_frames == 600,
                "overflow and rejected frames are counted");
    ok &= check(snapshot.submit_gap_count == 2 &&
                snapshot.submit_gap_min_ns == 5 * NS_PER_MS &&
                snapshot.submit_gap_max_ns == 15 * NS_PER_MS &&
                snapshot.submit_jitter_max_ns == 10 * NS_PER_MS,
                "submission cadence and jitter are deterministic");
    ok &= check(snapshot.silence_frames == 960 &&
                snapshot.silence_longest_frames == 960,
                "silent duration spans accepted zero frames");
    ok &= check(bw_audio_audit_finish(&audit) == 0, "finish cadence audit");
    return ok;
}

static int test_configuration_and_clock_failures(void)
{
    FakeClock clock;
    BwAudioAudit audit;
    BwAudioAuditSnapshot snapshot;
    int16_t frame[2] = {1, -1};
    int queued;
    int ok = 1;

    atomic_init(&clock.now, 0);
    ok &= check(bw_audio_audit_init(&audit, 0, 2, fake_now, &clock, NULL) != 0,
                "reject zero-size queue");
    ok &= check(strstr(bw_audio_audit_error(&audit), "invalid bounded queue") != NULL,
                "configuration failure is explicit");
    ok &= check(bw_audio_audit_init(&audit, 4096, 16, fake_now, &clock, NULL) != 0,
                "reject queue above memory bound");

    atomic_store(&clock.now, -1);
    ok &= check(bw_audio_audit_init(&audit, 128, 2, fake_now, &clock, NULL) != 0,
                "reject failed initial clock");
    ok &= check(strstr(bw_audio_audit_error(&audit), "clock") != NULL,
                "initial clock failure is explicit");

    atomic_store(&clock.now, 1000);
    ok &= check(bw_audio_audit_init(&audit, 128, 2, fake_now, &clock, NULL) == 0,
                "initialize clock-error audit");
    ok &= check(bw_audio_audit_submit(&audit, frame, 1) == 1,
                "prime clock-error audit");
    atomic_store(&clock.now, 999);
    ok &= check(bw_audio_audit_queued(&audit, &queued) != 0,
                "backwards clock fails operation");
    ok &= check(bw_audio_audit_snapshot(&audit, &snapshot) != 0 && snapshot.failed,
                "internal failure is persistent and reported");
    ok &= check(strstr(bw_audio_audit_error(&audit), "backwards") != NULL,
                "backwards clock failure is explicit");
    ok &= check(bw_audio_audit_finish(&audit) != 0,
                "failed audit cannot finish successfully");
    return ok;
}

static void *submit_worker(void *opaque)
{
    ThreadArgs *args = opaque;
    int16_t frame[2] = {1, -1};
    for (int i = 0; i < args->iterations; i++) {
        if (bw_audio_audit_submit(args->audit, frame, 1) != 1) {
            atomic_store(args->failed, 1);
            break;
        }
    }
    return NULL;
}

static void *snapshot_worker(void *opaque)
{
    ThreadArgs *args = opaque;
    BwAudioAuditSnapshot snapshot;
    for (int i = 0; i < args->iterations; i++) {
        if (bw_audio_audit_snapshot(args->audit, &snapshot) != 0) {
            atomic_store(args->failed, 1);
            break;
        }
    }
    return NULL;
}

static int test_thread_safe_aggregates(void)
{
    enum { PRODUCERS = 4, ITERATIONS = 1000 };
    FakeClock clock;
    BwAudioAudit audit;
    BwAudioAuditSnapshot snapshot;
    pthread_t producers[PRODUCERS], reader;
    _Atomic int failed = 0;
    ThreadArgs args = {&audit, ITERATIONS, &failed};
    int ok = 1;

    atomic_init(&clock.now, INT64_C(5000000000));
    ok &= check(bw_audio_audit_init(&audit, 4096, 8, fake_now, &clock, NULL) == 0,
                "initialize concurrent aggregate audit");
    for (int i = 0; i < PRODUCERS; i++)
        ok &= check(pthread_create(&producers[i], NULL, submit_worker, &args) == 0,
                    "start aggregate producer");
    ok &= check(pthread_create(&reader, NULL, snapshot_worker, &args) == 0,
                "start concurrent snapshot reader");
    for (int i = 0; i < PRODUCERS; i++)
        ok &= check(pthread_join(producers[i], NULL) == 0,
                    "join aggregate producer");
    ok &= check(pthread_join(reader, NULL) == 0,
                "join concurrent snapshot reader");
    ok &= check(!atomic_load(&failed), "concurrent operations succeed");
    ok &= check(bw_audio_audit_snapshot(&audit, &snapshot) == 0 &&
                snapshot.frames == PRODUCERS * ITERATIONS &&
                snapshot.queue_frames == PRODUCERS * ITERATIONS,
                "thread-safe aggregate totals");
    ok &= check(bw_audio_audit_finish(&audit) == 0,
                "finish concurrent aggregate audit");
    return ok;
}

int main(void)
{
    int ok = 1;
    ok &= test_signal_aggregates();
    ok &= test_cadence_and_queue_errors();
    ok &= test_configuration_and_clock_failures();
    ok &= test_thread_safe_aggregates();
    return ok ? 0 : 1;
}

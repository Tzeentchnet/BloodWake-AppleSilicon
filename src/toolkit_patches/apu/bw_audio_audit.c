#include "bw_audio_audit.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NS_PER_SECOND UINT64_C(1000000000)
#define REPORT_INTERVAL_NS INT64_C(1000000000)
#define MAX_QUEUE_FRAMES UINT64_C(65536)

static int add_u64(uint64_t *value, uint64_t add)
{
    if (*value > UINT64_MAX - add)
        return -1;
    *value += add;
    return 0;
}

static int fail_locked(BwAudioAudit *audit, const char *message)
{
    if (!audit->failed) {
        audit->failed = 1;
        snprintf(audit->error, sizeof(audit->error), "%s", message);
        audit->error[sizeof(audit->error) - 1] = 0;
    }
    return -1;
}

int64_t bw_audio_audit_monotonic_ns(void *context)
{
    struct timespec now;
    (void)context;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        now.tv_sec < 0 ||
        (uint64_t)now.tv_sec > (uint64_t)INT64_MAX / NS_PER_SECOND)
        return -1;
    return (int64_t)((uint64_t)now.tv_sec * NS_PER_SECOND +
                     (uint64_t)now.tv_nsec);
}

static void observe_queue_locked(BwAudioAudit *audit)
{
    if (audit->queue_frames < audit->queue_min_frames)
        audit->queue_min_frames = audit->queue_frames;
    if (audit->queue_frames > audit->queue_max_frames)
        audit->queue_max_frames = audit->queue_frames;
    audit->queue_observation_sum += (long double)audit->queue_frames;
    audit->queue_observations++;
}

static int sync_locked(BwAudioAudit *audit, int64_t now)
{
    uint64_t elapsed, whole_seconds, remaining_ns, scaled, due;

    if (audit->failed)
        return -1;
    if (now < 0)
        return fail_locked(audit, "monotonic clock read failed");
    if (now < audit->last_update_ns)
        return fail_locked(audit, "monotonic clock moved backwards");
    if (!audit->started) {
        audit->last_update_ns = now;
        observe_queue_locked(audit);
        return 0;
    }

    elapsed = (uint64_t)(now - audit->last_update_ns);
    whole_seconds = elapsed / NS_PER_SECOND;
    remaining_ns = elapsed % NS_PER_SECOND;
    if (whole_seconds > UINT64_MAX / BW_AUDIO_AUDIT_SAMPLE_RATE)
        return fail_locked(audit, "cadence counter overflow");
    due = whole_seconds * BW_AUDIO_AUDIT_SAMPLE_RATE;
    scaled = remaining_ns * BW_AUDIO_AUDIT_SAMPLE_RATE + audit->phase;
    if (due > UINT64_MAX - scaled / NS_PER_SECOND)
        return fail_locked(audit, "cadence counter overflow");
    due += scaled / NS_PER_SECOND;
    audit->phase = scaled % NS_PER_SECOND;

    if (due > audit->queue_frames) {
        uint64_t missing = due - audit->queue_frames;
        if (add_u64(&audit->consumed_frames, audit->queue_frames) != 0)
            return fail_locked(audit, "consumed-frame counter overflow");
        audit->queue_frames = 0;
        if (!audit->expected_drain) {
            if (add_u64(&audit->underflow_frames, missing) != 0)
                return fail_locked(audit, "underflow counter overflow");
            if (!audit->dry && add_u64(&audit->underflow_events, 1) != 0)
                return fail_locked(audit, "underflow counter overflow");
        }
        audit->dry = 1;
    } else {
        audit->queue_frames -= due;
        if (add_u64(&audit->consumed_frames, due) != 0)
            return fail_locked(audit, "consumed-frame counter overflow");
        if (audit->queue_frames)
            audit->dry = 0;
    }
    audit->last_update_ns = now;
    observe_queue_locked(audit);
    return 0;
}

static int read_clock_locked(BwAudioAudit *audit, int64_t *now)
{
    *now = audit->clock(audit->clock_context);
    return sync_locked(audit, *now);
}

int bw_audio_audit_init(BwAudioAudit *audit, int buffer_frames, int buffer_count,
                        BwAudioAuditClock clock, void *clock_context, FILE *log)
{
    uint64_t capacity;
    int64_t now;

    if (!audit)
        return -1;
    memset(audit, 0, sizeof(*audit));
    if (pthread_mutex_init(&audit->mutex, NULL) != 0) {
        snprintf(audit->error, sizeof(audit->error),
                 "cannot initialize aggregate lock");
        return -1;
    }
    audit->mutex_ready = 1;
    audit->clock = clock ? clock : bw_audio_audit_monotonic_ns;
    audit->clock_context = clock_context;
    audit->log = log;
    audit->queue_min_frames = UINT64_MAX;
    audit->submit_gap_min_ns = UINT64_MAX;

    if (buffer_frames <= 0 || buffer_count <= 0 ||
        (uint64_t)buffer_frames > MAX_QUEUE_FRAMES ||
        (uint64_t)buffer_count + 1 > MAX_QUEUE_FRAMES / (uint64_t)buffer_frames) {
        snprintf(audit->error, sizeof(audit->error),
                 "invalid bounded queue configuration");
        pthread_mutex_destroy(&audit->mutex);
        audit->mutex_ready = 0;
        return -1;
    }
    capacity = (uint64_t)buffer_frames * ((uint64_t)buffer_count + 1);
    if (capacity > MAX_QUEUE_FRAMES) {
        snprintf(audit->error, sizeof(audit->error),
                 "audio audit queue exceeds its memory bound");
        pthread_mutex_destroy(&audit->mutex);
        audit->mutex_ready = 0;
        return -1;
    }
    now = audit->clock(audit->clock_context);
    if (now < 0) {
        snprintf(audit->error, sizeof(audit->error),
                 "monotonic clock read failed");
        pthread_mutex_destroy(&audit->mutex);
        audit->mutex_ready = 0;
        return -1;
    }
    audit->buffer_frames = buffer_frames;
    audit->buffer_count = buffer_count;
    audit->capacity_frames = capacity;
    audit->last_update_ns = now;
    audit->initialized = 1;
    return 0;
}

int bw_audio_audit_submit(BwAudioAudit *audit, const int16_t *samples, int frames)
{
    int64_t now;
    uint64_t n;

    if (!audit || !audit->mutex_ready || !samples || frames <= 0)
        return -1;
    pthread_mutex_lock(&audit->mutex);
    if (!audit->initialized || read_clock_locked(audit, &now) != 0) {
        pthread_mutex_unlock(&audit->mutex);
        return -1;
    }
    n = (uint64_t)frames;
    if (n > audit->capacity_frames - audit->queue_frames) {
        if (add_u64(&audit->overflow_events, 1) != 0 ||
            add_u64(&audit->overflow_frames, n) != 0) {
            fail_locked(audit, "overflow counter overflow");
            pthread_mutex_unlock(&audit->mutex);
            return -1;
        }
        pthread_mutex_unlock(&audit->mutex);
        return 0;
    }
    if (audit->frames > UINT64_MAX - n ||
        audit->submitted_frames > UINT64_MAX - n) {
        fail_locked(audit, "submitted-frame counter overflow");
        pthread_mutex_unlock(&audit->mutex);
        return -1;
    }

    for (int i = 0; i < frames; i++) {
        int silent = 1;
        for (int channel = 0; channel < BW_AUDIO_AUDIT_CHANNELS; channel++) {
            int16_t sample = samples[i * BW_AUDIO_AUDIT_CHANNELS + channel];
            int32_t magnitude = sample < 0 ? -(int32_t)sample : sample;
            audit->sum[channel] += sample;
            audit->sum_squares[channel] += (long double)sample * sample;
            if ((uint32_t)magnitude > audit->peak[channel])
                audit->peak[channel] = (uint32_t)magnitude;
            if (sample == INT16_MIN || sample == INT16_MAX)
                audit->clipped[channel]++;
            if (sample)
                silent = 0;
        }
        if (silent) {
            audit->silence_frames++;
            audit->silence_current_frames++;
            if (audit->silence_current_frames > audit->silence_longest_frames)
                audit->silence_longest_frames = audit->silence_current_frames;
        } else {
            audit->silence_current_frames = 0;
        }
    }

    if (audit->submitted_frames) {
        uint64_t gap = (uint64_t)(now - audit->last_submit_ns);
        uint64_t expected = audit->previous_submit_frames * NS_PER_SECOND /
                            BW_AUDIO_AUDIT_SAMPLE_RATE;
        uint64_t jitter = gap > expected ? gap - expected : expected - gap;
        if (add_u64(&audit->submit_gap_count, 1) != 0 ||
            add_u64(&audit->submit_gap_sum_ns, gap) != 0) {
            fail_locked(audit, "cadence aggregate overflow");
            pthread_mutex_unlock(&audit->mutex);
            return -1;
        }
        if (gap < audit->submit_gap_min_ns)
            audit->submit_gap_min_ns = gap;
        if (gap > audit->submit_gap_max_ns)
            audit->submit_gap_max_ns = gap;
        if (jitter > audit->submit_jitter_max_ns)
            audit->submit_jitter_max_ns = jitter;
    }
    audit->last_submit_ns = now;
    audit->previous_submit_frames = n;
    if (!audit->started) {
        audit->started = 1;
        audit->start_ns = now;
        audit->next_report_ns = now;
        audit->phase = 0;
    }
    audit->frames += n;
    audit->submitted_frames += n;
    audit->queue_frames += n;
    audit->expected_drain = 0;
    audit->dry = 0;
    observe_queue_locked(audit);
    pthread_mutex_unlock(&audit->mutex);
    return 1;
}

int bw_audio_audit_queued(BwAudioAudit *audit, int *frames)
{
    int64_t now;
    if (!audit || !frames || !audit->mutex_ready)
        return -1;
    pthread_mutex_lock(&audit->mutex);
    if (!audit->initialized || read_clock_locked(audit, &now) != 0 ||
        audit->queue_frames > INT_MAX) {
        if (audit->queue_frames > INT_MAX)
            fail_locked(audit, "queue result exceeds backend contract");
        pthread_mutex_unlock(&audit->mutex);
        return -1;
    }
    *frames = (int)audit->queue_frames;
    pthread_mutex_unlock(&audit->mutex);
    return 0;
}

int bw_audio_audit_samples_played(BwAudioAudit *audit, uint64_t *frames)
{
    int64_t now;
    if (!audit || !frames || !audit->mutex_ready)
        return -1;
    pthread_mutex_lock(&audit->mutex);
    if (!audit->initialized || read_clock_locked(audit, &now) != 0) {
        pthread_mutex_unlock(&audit->mutex);
        return -1;
    }
    *frames = audit->consumed_frames;
    pthread_mutex_unlock(&audit->mutex);
    return 0;
}

int bw_audio_audit_expect_drain(BwAudioAudit *audit)
{
    int64_t now;
    if (!audit || !audit->mutex_ready)
        return -1;
    pthread_mutex_lock(&audit->mutex);
    if (!audit->initialized || read_clock_locked(audit, &now) != 0) {
        pthread_mutex_unlock(&audit->mutex);
        return -1;
    }
    audit->expected_drain = 1;
    audit->dry = 1;
    pthread_mutex_unlock(&audit->mutex);
    return 0;
}

unsigned bw_audio_audit_underruns(BwAudioAudit *audit)
{
    uint64_t count = 0;
    if (!audit || !audit->mutex_ready)
        return 0;
    pthread_mutex_lock(&audit->mutex);
    count = audit->underflow_events;
    pthread_mutex_unlock(&audit->mutex);
    return count > UINT_MAX ? UINT_MAX : (unsigned)count;
}

static void copy_snapshot_locked(const BwAudioAudit *audit, int64_t now,
                                 BwAudioAuditSnapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->frames = audit->frames;
    for (int channel = 0; channel < BW_AUDIO_AUDIT_CHANNELS; channel++) {
        snapshot->sum[channel] = audit->sum[channel];
        snapshot->sum_squares[channel] = audit->sum_squares[channel];
        snapshot->peak[channel] = audit->peak[channel];
        snapshot->clipped[channel] = audit->clipped[channel];
    }
    snapshot->silence_frames = audit->silence_frames;
    snapshot->silence_longest_frames = audit->silence_longest_frames;
    snapshot->silence_current_frames = audit->silence_current_frames;
    snapshot->submitted_frames = audit->submitted_frames;
    snapshot->consumed_frames = audit->consumed_frames;
    snapshot->queue_frames = audit->queue_frames;
    snapshot->queue_capacity_frames = audit->capacity_frames;
    snapshot->queue_min_frames = audit->queue_min_frames == UINT64_MAX
                               ? 0 : audit->queue_min_frames;
    snapshot->queue_max_frames = audit->queue_max_frames;
    snapshot->queue_observation_sum = audit->queue_observation_sum;
    snapshot->queue_observations = audit->queue_observations;
    snapshot->underflow_events = audit->underflow_events;
    snapshot->underflow_frames = audit->underflow_frames;
    snapshot->overflow_events = audit->overflow_events;
    snapshot->overflow_frames = audit->overflow_frames;
    snapshot->submit_gap_count = audit->submit_gap_count;
    snapshot->submit_gap_sum_ns = audit->submit_gap_sum_ns;
    snapshot->submit_gap_min_ns = audit->submit_gap_min_ns == UINT64_MAX
                                ? 0 : audit->submit_gap_min_ns;
    snapshot->submit_gap_max_ns = audit->submit_gap_max_ns;
    snapshot->submit_jitter_max_ns = audit->submit_jitter_max_ns;
    snapshot->elapsed_ns = audit->started && now >= audit->start_ns
                         ? (uint64_t)(now - audit->start_ns) : 0;
    snapshot->failed = audit->failed;
}

int bw_audio_audit_snapshot(BwAudioAudit *audit, BwAudioAuditSnapshot *snapshot)
{
    int64_t now;
    int result;
    if (!audit || !snapshot || !audit->mutex_ready)
        return -1;
    pthread_mutex_lock(&audit->mutex);
    now = audit->clock(audit->clock_context);
    result = sync_locked(audit, now);
    copy_snapshot_locked(audit, now < 0 ? audit->last_update_ns : now, snapshot);
    pthread_mutex_unlock(&audit->mutex);
    return result;
}

int bw_audio_audit_format(const BwAudioAuditSnapshot *snapshot, int final,
                          char *text, size_t text_size)
{
    long double rms[2] = {0, 0}, dc[2] = {0, 0};
    long double peak[2] = {0, 0};
    long double elapsed_s, queue_average, gap_average;
    int length;

    if (!snapshot || !text || !text_size)
        return -1;
    elapsed_s = snapshot->elapsed_ns / 1000000000.0L;
    queue_average = snapshot->queue_observations
                  ? snapshot->queue_observation_sum /
                    snapshot->queue_observations : 0;
    gap_average = snapshot->submit_gap_count
                ? (long double)snapshot->submit_gap_sum_ns /
                  snapshot->submit_gap_count / 1000000.0L : 0;
    if (snapshot->frames) {
        for (int channel = 0; channel < 2; channel++) {
            rms[channel] = sqrtl(snapshot->sum_squares[channel] /
                                 snapshot->frames) / 32768.0L;
            dc[channel] = snapshot->sum[channel] / snapshot->frames / 32768.0L;
            peak[channel] = snapshot->peak[channel] / 32768.0L;
        }
    }
    length = snprintf(
        text, text_size,
        "[AUDIO-AUDIT] reason=%s audible=no format=s16-stereo-48000 "
        "frames=%" PRIu64 " "
        "L_rms=%.8Lf L_peak=%.8Lf L_dc=%.8Lf L_clipped=%" PRIu64 " "
        "R_rms=%.8Lf R_peak=%.8Lf R_dc=%.8Lf R_clipped=%" PRIu64 " "
        "silence_ms=%.3Lf silence_longest_ms=%.3Lf "
        "silence_current_ms=%.3Lf queue=%" PRIu64 "/%" PRIu64 " "
        "queue_min=%" PRIu64 " queue_max=%" PRIu64 " "
        "underflows=%" PRIu64 " underflow_frames=%" PRIu64 " "
        "overflows=%" PRIu64 " overflow_frames=%" PRIu64 " "
        "latency_current_ms=%.3Lf latency_average_ms=%.3Lf "
        "latency_max_ms=%.3Lf submitted_hz=%.3Lf consumed_hz=%.3Lf "
        "submit_gap_average_ms=%.3Lf submit_gap_min_ms=%.3Lf "
        "submit_gap_max_ms=%.3Lf submit_jitter_max_ms=%.3Lf failed=%s\n",
        final ? "final" : "periodic", snapshot->frames,
        rms[0], peak[0], dc[0], snapshot->clipped[0],
        rms[1], peak[1], dc[1], snapshot->clipped[1],
        snapshot->silence_frames / 48.0L,
        snapshot->silence_longest_frames / 48.0L,
        snapshot->silence_current_frames / 48.0L,
        snapshot->queue_frames, snapshot->queue_capacity_frames,
        snapshot->queue_min_frames, snapshot->queue_max_frames,
        snapshot->underflow_events, snapshot->underflow_frames,
        snapshot->overflow_events, snapshot->overflow_frames,
        snapshot->queue_frames / 48.0L, queue_average / 48.0L,
        snapshot->queue_max_frames / 48.0L,
        snapshot->elapsed_ns >= NS_PER_SECOND
            ? snapshot->submitted_frames / elapsed_s : 0,
        snapshot->elapsed_ns >= NS_PER_SECOND
            ? snapshot->consumed_frames / elapsed_s : 0,
        gap_average, snapshot->submit_gap_min_ns / 1000000.0L,
        snapshot->submit_gap_max_ns / 1000000.0L,
        snapshot->submit_jitter_max_ns / 1000000.0L,
        snapshot->failed ? "yes" : "no");
    return length < 0 || (size_t)length >= text_size ? -1 : 0;
}

int bw_audio_audit_report(BwAudioAudit *audit, int final)
{
    BwAudioAuditSnapshot snapshot;
    char text[2048];
    int64_t now;
    int due, result;

    if (!audit || !audit->mutex_ready)
        return -1;
    pthread_mutex_lock(&audit->mutex);
    now = audit->clock(audit->clock_context);
    result = sync_locked(audit, now);
    due = final || (audit->started && now >= audit->next_report_ns);
    if (due && !final) {
        do {
            if (audit->next_report_ns > INT64_MAX - REPORT_INTERVAL_NS) {
                result = fail_locked(audit, "report cadence overflow");
                break;
            }
            audit->next_report_ns += REPORT_INTERVAL_NS;
        } while (now >= audit->next_report_ns);
    }
    copy_snapshot_locked(audit, now < 0 ? audit->last_update_ns : now, &snapshot);
    pthread_mutex_unlock(&audit->mutex);
    if (!due || !audit->log)
        return result;
    if (bw_audio_audit_format(&snapshot, final, text, sizeof(text)) != 0 ||
        fputs(text, audit->log) == EOF ||
        fflush(audit->log) != 0) {
        pthread_mutex_lock(&audit->mutex);
        fail_locked(audit, "cannot write aggregate audit report");
        pthread_mutex_unlock(&audit->mutex);
        return -1;
    }
    return result;
}

int bw_audio_audit_finish(BwAudioAudit *audit)
{
    int result = 0;
    if (!audit || !audit->mutex_ready)
        return -1;
    if (audit->initialized && bw_audio_audit_report(audit, 1) != 0)
        result = -1;
    pthread_mutex_lock(&audit->mutex);
    audit->initialized = 0;
    pthread_mutex_unlock(&audit->mutex);
    if (pthread_mutex_destroy(&audit->mutex) != 0)
        result = -1;
    audit->mutex_ready = 0;
    return result;
}

const char *bw_audio_audit_error(const BwAudioAudit *audit)
{
    return audit && audit->error[0] ? audit->error : "unknown audio audit failure";
}

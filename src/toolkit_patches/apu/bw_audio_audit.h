#ifndef BW_AUDIO_AUDIT_H
#define BW_AUDIO_AUDIT_H

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#define BW_AUDIO_AUDIT_SAMPLE_RATE 48000
#define BW_AUDIO_AUDIT_CHANNELS 2

typedef int64_t (*BwAudioAuditClock)(void *context);

typedef struct {
    uint64_t frames;
    long double sum[BW_AUDIO_AUDIT_CHANNELS];
    long double sum_squares[BW_AUDIO_AUDIT_CHANNELS];
    uint32_t peak[BW_AUDIO_AUDIT_CHANNELS];
    uint64_t clipped[BW_AUDIO_AUDIT_CHANNELS];
    uint64_t silence_frames;
    uint64_t silence_longest_frames;
    uint64_t silence_current_frames;
    uint64_t submitted_frames;
    uint64_t consumed_frames;
    uint64_t queue_frames;
    uint64_t queue_capacity_frames;
    uint64_t queue_min_frames;
    uint64_t queue_max_frames;
    long double queue_observation_sum;
    uint64_t queue_observations;
    uint64_t underflow_events;
    uint64_t underflow_frames;
    uint64_t overflow_events;
    uint64_t overflow_frames;
    uint64_t submit_gap_count;
    uint64_t submit_gap_sum_ns;
    uint64_t submit_gap_min_ns;
    uint64_t submit_gap_max_ns;
    uint64_t submit_jitter_max_ns;
    uint64_t elapsed_ns;
    int failed;
} BwAudioAuditSnapshot;

typedef struct {
    pthread_mutex_t mutex;
    int mutex_ready;
    int initialized;
    int failed;
    int started;
    int expected_drain;
    int dry;
    int buffer_frames;
    int buffer_count;
    BwAudioAuditClock clock;
    void *clock_context;
    FILE *log;
    char error[128];
    uint64_t capacity_frames;
    uint64_t queue_frames;
    uint64_t submitted_frames;
    uint64_t consumed_frames;
    uint64_t phase;
    int64_t start_ns;
    int64_t last_update_ns;
    int64_t last_submit_ns;
    int64_t next_report_ns;
    uint64_t previous_submit_frames;
    long double sum[BW_AUDIO_AUDIT_CHANNELS];
    long double sum_squares[BW_AUDIO_AUDIT_CHANNELS];
    uint32_t peak[BW_AUDIO_AUDIT_CHANNELS];
    uint64_t clipped[BW_AUDIO_AUDIT_CHANNELS];
    uint64_t frames;
    uint64_t silence_frames;
    uint64_t silence_longest_frames;
    uint64_t silence_current_frames;
    uint64_t queue_min_frames;
    uint64_t queue_max_frames;
    long double queue_observation_sum;
    uint64_t queue_observations;
    uint64_t underflow_events;
    uint64_t underflow_frames;
    uint64_t overflow_events;
    uint64_t overflow_frames;
    uint64_t submit_gap_count;
    uint64_t submit_gap_sum_ns;
    uint64_t submit_gap_min_ns;
    uint64_t submit_gap_max_ns;
    uint64_t submit_jitter_max_ns;
} BwAudioAudit;

int bw_audio_audit_init(BwAudioAudit *audit, int buffer_frames, int buffer_count,
                        BwAudioAuditClock clock, void *clock_context, FILE *log);
int bw_audio_audit_finish(BwAudioAudit *audit);
int bw_audio_audit_submit(BwAudioAudit *audit, const int16_t *samples, int frames);
int bw_audio_audit_queued(BwAudioAudit *audit, int *frames);
int bw_audio_audit_samples_played(BwAudioAudit *audit, uint64_t *frames);
int bw_audio_audit_expect_drain(BwAudioAudit *audit);
unsigned bw_audio_audit_underruns(BwAudioAudit *audit);
int bw_audio_audit_snapshot(BwAudioAudit *audit, BwAudioAuditSnapshot *snapshot);
int bw_audio_audit_report(BwAudioAudit *audit, int final);
int bw_audio_audit_format(const BwAudioAuditSnapshot *snapshot, int final,
                          char *text, size_t text_size);
const char *bw_audio_audit_error(const BwAudioAudit *audit);
int64_t bw_audio_audit_monotonic_ns(void *context);

#endif

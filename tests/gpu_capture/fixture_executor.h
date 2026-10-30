#ifndef GPU_CAPTURE_TEST_FIXTURE_EXECUTOR_H
#define GPU_CAPTURE_TEST_FIXTURE_EXECUTOR_H

#include "bw_gpu_capture.h"

int fixture_capture(const char *path, int audit);
int fixture_replay(const char *path, int audit, int expect_mismatch);
int fixture_replay_rejection(const char *path, BwGpuCaptureStatus expected);
int fixture_thread_violation(void);
int fixture_late_attach(void);
int fixture_bulk_disabled(void);

#endif

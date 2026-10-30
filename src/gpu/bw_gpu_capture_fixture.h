#ifndef BW_GPU_CAPTURE_FIXTURE_H
#define BW_GPU_CAPTURE_FIXTURE_H

#include "bw_gpu_capture.h"

/*
 * Available only when the carried executor is compiled with
 * BW_GPU_CAPTURE_FIXTURE. Attach an empty recorder before its first method,
 * with no semaphore target and while the synthetic executor is idle. All
 * method and target calls must stay on the attaching thread. The fixture owns
 * all guest memory and records each host write/barrier explicitly.
 * This is not safe for a running guest.
 */
int bw_gpu_capture_fixture_begin(BwGpuCapture *capture);
BwGpuCaptureStatus bw_gpu_capture_fixture_end(void);

#endif

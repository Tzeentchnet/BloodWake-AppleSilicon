#ifndef BW_GPU_CAPTURE_INTERNAL_H
#define BW_GPU_CAPTURE_INTERNAL_H

#include "bw_gpu_capture.h"

/* Fixture observer lifetime, not a live-guest capture interface. */
int bw_gpu_capture_set_attached(BwGpuCapture *capture, int attached);

#endif

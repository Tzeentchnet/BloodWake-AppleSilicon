/* bw_pscache.h - persistent cache of the combiner pixel shaders' bytecode
 * (agent T1, every edition; see bw_pscache.c). */
#ifndef BW_PSCACHE_H
#define BW_PSCACHE_H

#include <stddef.h>

/* At device setup: read every valid entry from the cache folder. */
void bw_pscache_init(void);

/* The compiled bytecode for this shader text (ps_5_0, O3), if the cache has
 * it; NULL otherwise. The bytes stay owned by the cache. */
const void *bw_pscache_find(const char *src, size_t len, size_t *size);

/* A shader just compiled: keep it for later runs. */
void bw_pscache_store(const char *src, size_t len, const void *blob, size_t size);

/* BW_GPU_PSCACHE_VERIFY=1: compile on hits too and compare (test aid). */
int bw_pscache_verify(void);

/* A line for the log: the shader came from the cache (compiled 0) or was
 * compiled (1) in `ms`, with this run's totals. */
void bw_pscache_note(int compiled, double ms);

#endif

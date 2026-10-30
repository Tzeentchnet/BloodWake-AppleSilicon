/* bw_profile.h - Blood Wake sampling profiler (BW_PROFILE). See bw_profile.c.
 * macOS caller stacks use frame pointers; source lines need external atos. */
#ifndef BW_PROFILE_H
#define BW_PROFILE_H

void bw_profile_init(void);
void bw_profile_dump(const char *why);
void bw_profile_atexit(void);

#endif

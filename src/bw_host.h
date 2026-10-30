#ifndef BW_HOST_H
#define BW_HOST_H

/* The play launcher's host window (BW_HOST=1, set by tools/play.py): one
 * window for the whole session, with each boot of the game drawn inside it.
 * See bw_host.c. macOS uses the native main's supervisor instead of the
 * Windows embedding host; calling bw_host_main directly there returns an error. */
int bw_host_wanted(void);
int bw_host_main(void);

#endif

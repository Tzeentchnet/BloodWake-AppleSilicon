#ifndef BW_MACOS_BOOT_H
#define BW_MACOS_BOOT_H

#include <stddef.h>
#include <stdint.h>

enum {
    BW_MACOS_LAUNCH_PATH_MAX = 260,
    BW_MACOS_LAUNCH_DATA_BYTES = 3072,
    BW_MACOS_LAUNCH_FILE_MAX = 16 + BW_MACOS_LAUNCH_PATH_MAX +
                               BW_MACOS_LAUNCH_DATA_BYTES
};

typedef struct {
    char path[BW_MACOS_LAUNCH_PATH_MAX + 1];
    uint32_t data_len;
    uint8_t data[BW_MACOS_LAUNCH_DATA_BYTES];
} BwMacosLaunch;

/* BWRL is the same on-disk interchange format as the Windows launcher. */
int bw_macos_launch_decode(const uint8_t *bytes, size_t size, BwMacosLaunch *out);
int bw_macos_launch_encode(const BwMacosLaunch *launch, uint8_t *bytes,
                           size_t capacity, size_t *written);
int bw_macos_launch_read_file(const char *path, BwMacosLaunch *out);
int bw_macos_launch_page(const BwMacosLaunch *launch, uint8_t page[4096]);
int bw_macos_launch_is_gameplay(const BwMacosLaunch *launch);

/* The supervisor provides a private pipe to each single-use game process. */
int bw_macos_boot_receive(BwMacosLaunch *launch, int *has_launch);
int bw_macos_record_relaunch(uint32_t path_va, uint32_t data_va);
int bw_macos_supervise(int argc, char **argv);

#endif

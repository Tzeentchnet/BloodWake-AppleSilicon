#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Exercise the real path resolver against synthetic files, not disc data. */
#include "../../src/bw_fix_hooks.c"

int main(void)
{
    char root[80], dir[128], path[192], actual[256];
    FILE *f;
    snprintf(root, sizeof root, "hook-case-%ld", (long)getpid());
    snprintf(dir, sizeof dir, "%s/MiSsIoNs", root);
    assert(mkdir(root, 0700) == 0 && mkdir(dir, 0700) == 0);

    snprintf(path, sizeof path, "%s/01_VoIcE_EnGlIsH.PaK", dir);
    f = fopen(path, "wb");
    assert(f && fputc('v', f) == 'v' && fclose(f) == 0);
    assert(mission_component(root, "missions", actual, sizeof actual));
    assert(!strcmp(actual, "MiSsIoNs"));
    assert(mission_component(dir, "01_voice_english.pak", actual, sizeof actual));
    assert(!strcmp(actual, "01_VoIcE_EnGlIsH.PaK"));
    f = open_mission_pak(root, 1, 1);
    assert(f && fgetc(f) == 'v' && fclose(f) == 0);
    assert(unlink(path) == 0);

    snprintf(path, sizeof path, "%s/02.PAK", dir);
    f = fopen(path, "wb");
    assert(f && fputc('s', f) == 's' && fclose(f) == 0);
    f = open_mission_pak(root, 2, 0);
    assert(f && fgetc(f) == 's' && fclose(f) == 0);
    assert(!open_mission_pak(root, 3, 0));
    assert(unlink(path) == 0 && rmdir(dir) == 0 && rmdir(root) == 0);
    puts("mission path case tests passed");
    return 0;
}

#include <assert.h>
#include <fenv.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Include the real driver with a synthetic batch, never generated game code. */
#include "../../src/bw_ocean_native.c"

static uint32_t guest_words[0x100000];
ptrdiff_t g_xbox_mem_offset;
__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi;
__thread uint32_t g_ebp, g_seh_ebp;
__thread int g_fp_top, g_fp_cmp;
__thread uint16_t g_fp_control_word, g_fp_cc;
unsigned char g_bw_fix_on[BW_FIX_COUNT];
static bw_edition_t edition;
static unsigned generated_calls;

bw_edition_t bw_edition(void) { return edition; }
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
int64_t recomp_fist(double value, uint16_t control, unsigned bits)
{
    (void)value; (void)control; (void)bits;
    assert(!"synthetic batch should not call recomp_fist");
    return 0;
}
void sub_00098130_gen(void)
{
    generated_calls++;
    g_eax = 0x7777u;
    g_esp += 12;
}

static void test_fenv(void)
{
    fenv_t original;
    NbRegs r = { 0 };
    int saved_flags;
    assert(fegetenv(&original) == 0);
    assert(fesetround(FE_DOWNWARD) == 0);
    assert(feclearexcept(FE_ALL_EXCEPT) == 0);
    assert(feraiseexcept(FE_DIVBYZERO) == 0);
    saved_flags = fetestexcept(FE_ALL_EXCEPT);
    g_fp_control_word = 0;

    guest_words[0] = 0;
    assert(nb_run(&r, 0) && r.eax == 1);
    assert(fegetround() == FE_DOWNWARD && fetestexcept(FE_ALL_EXCEPT) == saved_flags);

    guest_words[0] = 1;
    assert(!nb_run(&r, 0));
    assert(fegetround() == FE_DOWNWARD && fetestexcept(FE_ALL_EXCEPT) == saved_flags);
    guest_words[0] = 2;
    assert(!nb_run(&r, 0));
    assert(fegetround() == FE_DOWNWARD && fetestexcept(FE_ALL_EXCEPT) == saved_flags);
    guest_words[0] = 3;
    assert(!nb_run(&r, 0));
    assert(fegetround() == FE_DOWNWARD && fetestexcept(FE_ALL_EXCEPT) == saved_flags);
    guest_words[0] = 4;
    assert(!nb_run(&r, 0) && s_n_fb_range == 1 && s_n_fb_bail == 3);
    assert(fegetround() == FE_DOWNWARD && fetestexcept(FE_ALL_EXCEPT) == saved_flags);

    guest_words[0] = 5;
    assert(!nb_run(&r, 0) && s_n_fb_bail == 4);
    assert(fegetround() == FE_DOWNWARD && fetestexcept(FE_ALL_EXCEPT) == saved_flags);
    guest_words[0] = 6;
    assert(!nb_run(&r, 0) && s_n_fb_bail == 5);
    assert(fegetround() == FE_DOWNWARD && fetestexcept(FE_ALL_EXCEPT) == saved_flags);
    guest_words[0] = 7;
    assert(nb_run(&r, 0) && r.eax == 3);

    guest_words[0] = 0;
    g_fp_control_word = 0x300u;
    assert(nb_run(&r, 0) && r.eax == 5);
    guest_words[0] = 5;
    assert(nb_run(&r, 0) && r.eax == 7);
    assert(fegetround() == FE_DOWNWARD && fetestexcept(FE_ALL_EXCEPT) == saved_flags);
    assert(fesetenv(&original) == 0);
}

static void test_guest_fallback(void)
{
    const uint32_t sp = 0x10000u;
    edition = BW_ED_ORIGINAL;
    s_init = 0; g_esp = sp; g_eax = 10;
    guest_words[sp / 4] = 0;
    guest_words[sp / 4 + 1] = 0x1Du;
    guest_words[sp / 4 + 2] = 25;
    bw_ocean_eval();
    assert(generated_calls == 1 && g_eax == 0x7777u && g_esp == sp + 12);

    edition = BW_ED_REMASTERED;
    g_bw_fix_on[BW_FIX_OCEAN_NATIVE] = 1;
    s_init = 0; g_fp_control_word = 0; g_esp = sp; g_eax = 10;
    bw_ocean_eval();
    assert(generated_calls == 1 && g_eax == 11 && g_esp == sp + 12);

    g_esp = sp; g_eax = 10;
    guest_words[sp / 4] = 1;
    guest_words[sp / 4 + 1] = 0x1Du;
    guest_words[sp / 4 + 2] = 25;
    bw_ocean_eval();
    assert(generated_calls == 2 && g_eax == 0x7777u && g_esp == sp + 12);
    assert(guest_words[sp / 4] == 1 && guest_words[sp / 4 + 1] == 0x1Du &&
           guest_words[sp / 4 + 2] == 25);
}

int main(void)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest_words;
    unsetenv("BW_OCEAN_NATIVE_LOG");
    unsetenv("BW_OCEAN_NATIVE_VERIFY");
    unsetenv("BW_OCEAN_NATIVE");
    unsetenv("BW_OCEAN_TILEDUMP");
    unsetenv("BW_OCEAN_SEAM_TEST");
    unsetenv("BW_OCEAN_PASSES");
    unsetenv("BW_OCEAN_TINT");
    unsetenv("BW_OCEAN_CUBETEST");
    unsetenv("BW_OCEAN_NODIST");
    unsetenv("BW_OCEAN_FLATTEN");
    test_fenv();
    test_guest_fallback();
    puts("ocean environment and guest fallback tests passed");
    return 0;
}

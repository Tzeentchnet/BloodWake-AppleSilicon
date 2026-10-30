/* Synthetic batch used to exercise the real driver's fenv and fallback paths. */
#ifndef NB_COVER_TABLE
#define NB_COVER_TABLE
#define NB_COVER_N 1
#define NBX_BATCH_RETPOP 12u
static const char *const nb_cover_names[NB_COVER_N] = { "synthetic" };
#endif

static double nbx_batch(NbCtx *NBC, NbRegs *NBR, uint32_t SP, unsigned TOP)
{
    uint32_t scenario = *(uint32_t *)(NBC->mo + SP);
    (void)TOP;
    if (scenario == 1u || scenario == 4u) {
        volatile double large = 1e300;
        volatile float narrowed = (float)large;
        (void)narrowed;
        if (scenario == 1u)
            NB_COMMIT_CHECK();
    } else if (scenario == 2u) {
        volatile double small = 1e-300;
        volatile float narrowed = (float)small;
        (void)narrowed;
        NB_COMMIT_CHECK();
    } else if (scenario == 3u) {
        NB_BAIL();
    } else if (scenario == 5u) {
        volatile double just_below_normal = 0x1.fffffe8p-127;
        volatile double rounded = NB_R(just_below_normal);
        (void)rounded;
    } else if (scenario == 6u) {
        volatile double just_above_max = 0x1.fffffe1p127;
        volatile double rounded = NB_R(just_above_max);
        (void)rounded;
    } else if (scenario == 7u) {
        volatile double smallest_normal = 0x1p-126;
        volatile double rounded = NB_R(smallest_normal);
        (void)rounded;
    }
    NBR->eax += NBC->pc24 ? 1u : 2u;
    return 0.0;
}

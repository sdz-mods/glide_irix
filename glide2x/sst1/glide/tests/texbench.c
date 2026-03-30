/*
** texbench.c - Glide2x TMU texture upload timing benchmark
**
** Uploads textures of various sizes/formats and measures time per upload.
** Tests two phases for each type:
**   Fill : cold uploads, each TMU slot written once
**   Wrap : uploads past TMU capacity (address wraps back to start)
**          — this is the scenario that caused minutes-per-frame in Q2
**
** Flags any single upload over 10 ms as SLOW.
**
** Build: smake -f Makefile.tests_irix texbench
** Usage: ./texbench
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <glide.h>

/* -----------------------------------------------------------------------
 * Timing
 * --------------------------------------------------------------------- */
static long
usec_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000000L + (long)tv.tv_usec;
}

/* -----------------------------------------------------------------------
 * Texture data — large enough for 256x256 ARGB full mipchain
 * 256*256*2 * 4/3 ≈ 174762 bytes; use 256*256*4 to be safe.
 * --------------------------------------------------------------------- */
#define MAX_TEX_BYTES (256*256*4)
static unsigned char texbuf[MAX_TEX_BYTES];

/* -----------------------------------------------------------------------
 * Test cases
 *
 * Glide2x GrTexInfo convention (this build: GR_LOD_256=0x0, GR_LOD_1=0x8):
 *
 *   largeLod = LOD of the BASE (largest) level  — e.g. GR_LOD_256 = 0
 *   smallLod = LOD of the SMALLEST included mip — e.g. GR_LOD_1   = 8
 *              = largeLod for no mipmaps (base level only)
 *
 * Download loop:  for (lod = largeLod; lod <= smallLod; lod++)
 * Size formula:   offset[smallLod+1] - offset[largeLod]
 * --------------------------------------------------------------------- */
typedef struct {
    const char       *name;
    GrLOD_t           smallLod;   /* smallest mip in chain (GR_LOD_1 for full chain) */
    GrLOD_t           largeLod;   /* base (largest) level  (GR_LOD_256 for 256x256)  */
    GrAspectRatio_t   aspect;
    GrTextureFormat_t fmt;
} TexCase;

static const TexCase cases[] = {
    /* Full mipmap chains (Q2 world/skin textures) */
    { "256x256 RGB565  full-mip",   GR_LOD_1,   GR_LOD_256, GR_ASPECT_1x1, GR_TEXFMT_RGB_565   },
    { "128x128 RGB565  full-mip",   GR_LOD_1,   GR_LOD_128, GR_ASPECT_1x1, GR_TEXFMT_RGB_565   },
    { "64x64   RGB565  full-mip",   GR_LOD_1,   GR_LOD_64,  GR_ASPECT_1x1, GR_TEXFMT_RGB_565   },
    /* No mipmaps (Q2 pics / UI) */
    { "256x256 ARGB4444 no-mip",    GR_LOD_256, GR_LOD_256, GR_ASPECT_1x1, GR_TEXFMT_ARGB_4444 },
    { "128x128 ARGB4444 no-mip",    GR_LOD_128, GR_LOD_128, GR_ASPECT_1x1, GR_TEXFMT_ARGB_4444 },
    { "64x64   ARGB4444 no-mip",    GR_LOD_64,  GR_LOD_64,  GR_ASPECT_1x1, GR_TEXFMT_ARGB_4444 },
    /* Paletted 8-bit (1 byte/texel, 4x smaller than ARGB) */
    { "256x256 P8       no-mip",    GR_LOD_256, GR_LOD_256, GR_ASPECT_1x1, GR_TEXFMT_P_8       },
    { "128x128 P8       no-mip",    GR_LOD_128, GR_LOD_128, GR_ASPECT_1x1, GR_TEXFMT_P_8       },
    { "64x64   P8       no-mip",    GR_LOD_64,  GR_LOD_64,  GR_ASPECT_1x1, GR_TEXFMT_P_8       },
};
#define N_CASES (int)(sizeof(cases)/sizeof(cases[0]))

/* -----------------------------------------------------------------------
 * Upload one mipmap and wait for idle; return elapsed microseconds.
 * grSstIdle() ensures the TMU has actually consumed the data, not just
 * that it's been written into the Glide FIFO.
 * --------------------------------------------------------------------- */
static long
timed_upload(GrTexInfo *info, FxU32 addr)
{
    long t0, t1;
    t0 = usec_now();
    grTexDownloadMipMap(GR_TMU0, addr, GR_MIPMAPLEVELMASK_BOTH, info);
    grSstIdle();
    t1 = usec_now();
    return t1 - t0;
}

/* -----------------------------------------------------------------------
 * Run one test case through fill + wrap phases.
 * --------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
 * Advance addr by sz, skipping over the 2 MB TMU boundary.
 * The Voodoo1 TMU cannot hold a texture that straddles 0x200000.
 * If the next slot would span it, jump to 0x200000.
 * If we fall off the end of TMU space, wrap back to tmu_min.
 * --------------------------------------------------------------------- */
static FxU32
next_addr(FxU32 addr, FxU32 sz, FxU32 tmu_min, FxU32 tmu_max)
{
    addr += sz;
    if (addr + sz > tmu_max)
        addr = tmu_min;
    /* skip slot that would span the 2 MB boundary */
    if (addr < 0x200000UL && addr + sz > 0x200000UL)
        addr = 0x200000UL;
    if (addr + sz > tmu_max)
        addr = tmu_min;
    return addr;
}

static void
run_case(const TexCase *c, FxU32 tmu_min, FxU32 tmu_max)
{
    GrTexInfo info;
    FxU32     sz, n_fit, addr;
    long      us, total;
    int       i, extra;
    /* histogram buckets: <1ms, 1-9ms, 10-99ms, >=100ms */
    int       bkt[4];

    memset(&info, 0, sizeof(info));
    info.smallLod    = c->smallLod;
    info.largeLod    = c->largeLod;
    info.aspectRatio = c->aspect;
    info.format      = c->fmt;
    info.data        = texbuf;

    sz = grTexCalcMemRequired(c->smallLod, c->largeLod, c->aspect, c->fmt);
    if (sz == 0 || sz > MAX_TEX_BYTES) {
        printf("[%s]  sz=%lu — skipped\n\n", c->name, (unsigned long)sz);
        return;
    }

    /* Count how many non-boundary-spanning slots exist in TMU */
    n_fit = 0;
    addr  = tmu_min;
    while (addr + sz <= tmu_max) {
        if (!(addr < 0x200000UL && addr + sz > 0x200000UL))
            n_fit++;
        addr += sz;
        /* skip boundary-spanning slot */
        if (addr < 0x200000UL && addr + sz > 0x200000UL)
            addr = 0x200000UL;
    }
    if (n_fit == 0) n_fit = 1;

    printf("[%s]\n", c->name);
    printf("  bytes/tex=%lu  fits_in_TMU=%lu\n",
           (unsigned long)sz, (unsigned long)n_fit);

    /* ---- Phase 1: fill (cold, each slot once) ---- */
    addr  = tmu_min;
    /* start addr must itself not span the boundary */
    if (addr < 0x200000UL && addr + sz > 0x200000UL)
        addr = 0x200000UL;
    total = 0;
    for (i = 0; i < (int)n_fit; i++) {
        us     = timed_upload(&info, addr);
        total += us;
        addr   = next_addr(addr, sz, tmu_min, tmu_max);
    }
    printf("  Fill  %2lu uploads: total=%5ld ms  avg=%6ld us\n",
           (unsigned long)n_fit, total / 1000L, total / (long)n_fit);

    /* ---- Phase 2: wrap (overflow, address cycles back to start) ---- */
    extra = (int)n_fit + 8;
    addr  = tmu_min;
    if (addr < 0x200000UL && addr + sz > 0x200000UL)
        addr = 0x200000UL;
    total = 0;
    bkt[0] = bkt[1] = bkt[2] = bkt[3] = 0;

    for (i = 0; i < extra; i++) {
        FxU32 cur = addr;
        us     = timed_upload(&info, cur);
        total += us;
        addr   = next_addr(addr, sz, tmu_min, tmu_max);

        if      (us <   1000) bkt[0]++;
        else if (us <  10000) bkt[1]++;
        else if (us < 100000) bkt[2]++;
        else                  bkt[3]++;

        if (us >= 10000)
            printf("  SLOW #%02d addr=0x%08lx  %ld ms\n",
                   i, (unsigned long)cur, us / 1000L);
    }
    printf("  Wrap  %2d uploads: total=%5ld ms  avg=%6ld us\n",
           extra, total / 1000L, total / (long)extra);
    printf("  Histogram: <1ms=%d  1-9ms=%d  10-99ms=%d  >=100ms=%d\n\n",
           bkt[0], bkt[1], bkt[2], bkt[3]);
}

/* -----------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------- */
int
main(void)
{
    GrHwConfiguration hw;
    FxU32 tmu_min, tmu_max;
    int   i;

    /* fill texbuf with a visible pattern so the TMU gets real data */
    for (i = 0; i < MAX_TEX_BYTES; i++)
        texbuf[i] = (unsigned char)(i & 0xFF);

    printf("texbench: Glide2x TMU upload timing\n");
    printf("=====================================\n\n");

    grGlideInit();
    if (!grSstQueryHardware(&hw)) {
        fprintf(stderr, "grSstQueryHardware failed\n");
        grGlideShutdown();
        return 1;
    }
    grSstSelect(0);
    if (!grSstWinOpen(0, GR_RESOLUTION_640x480, GR_REFRESH_60Hz,
                      GR_COLORFORMAT_ABGR, GR_ORIGIN_UPPER_LEFT, 2, 1)) {
        fprintf(stderr, "grSstWinOpen failed\n");
        grGlideShutdown();
        return 1;
    }

    tmu_min = grTexMinAddress(GR_TMU0);
    tmu_max = grTexMaxAddress(GR_TMU0);

    printf("TMU0: 0x%08lx..0x%08lx  (%lu KB usable)\n\n",
           (unsigned long)tmu_min,
           (unsigned long)tmu_max,
           (unsigned long)((tmu_max - tmu_min) / 1024UL));

    for (i = 0; i < N_CASES; i++) {
        run_case(&cases[i], tmu_min, tmu_max);
        fflush(stdout);
    }

    printf("=====================================\n");
    printf("Done.\n");

    grSstIdle();
    grGlideShutdown();
    return 0;
}

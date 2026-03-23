/*
 * lfbtest.c - Minimal LFB write+readback test for IRIX/SGI O2
 *
 * Writes known asymmetric pixel patterns via grLfbWriteRegion and reads
 * them back via grLfbReadRegion.  Tests both 4-byte-aligned (even x)
 * and 4-byte-unaligned (odd x) start addresses.
 *
 * Avoids grBufferClear to eliminate GPU/CPU race conditions.
 * Prints every written/read pixel pair so byte-swap direction is visible.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <glide.h>
#include "tlib.h"

GrHwConfiguration hwconfig;
static char version[80];

/* 8 pixels with distinct, asymmetric values so byte-swap errors are obvious */
static const FxU16 pattern[8] = {
    0x1234, 0x5678, 0x9ABC, 0xDEF0,
    0x1357, 0x2468, 0x3579, 0x468A
};

static void run_test(int start_x, int n_pixels, FILE *f)
{
    FxU16 src[16];
    FxU16 dst[16];
    GrLfbInfo_t info;
    int i;

    /* fill src with n_pixels pixels from pattern */
    for (i = 0; i < n_pixels; i++)
        src[i] = pattern[i % 8];
    memset(dst, 0xCC, sizeof(dst));

    fprintf(f, "\n--- x=%d, n=%d (%s) ---\n", start_x, n_pixels,
            (start_x & 1) ? "odd-start" : "even-start");

    /* Write */
    info.size = sizeof(info);
    if (!grLfbWriteRegion(GR_BUFFER_BACKBUFFER,
                          (FxU32)start_x, 0,
                          GR_LFB_SRC_FMT_565,
                          (FxU32)n_pixels, 1,
                          n_pixels * 2,
                          src)) {
        fprintf(f, "  grLfbWriteRegion FAILED\n");
        return;
    }

    /* Read back */
    if (!grLfbReadRegion(GR_BUFFER_BACKBUFFER,
                         (FxU32)start_x, 0,
                         (FxU32)n_pixels, 1,
                         n_pixels * 2,
                         dst)) {
        fprintf(f, "  grLfbReadRegion FAILED\n");
        return;
    }

    /* Compare */
    for (i = 0; i < n_pixels; i++) {
        const char *mark = (src[i] != dst[i]) ? " <-- MISMATCH" : "";
        fprintf(f, "  px[%d]: wrote=0x%04X  got=0x%04X%s\n",
                i, (unsigned)src[i], (unsigned)dst[i], mark);
    }
    fflush(f);
}

int main(int argc, char **argv)
{
    FILE *f;
    GrScreenResolution_t resolution = GR_RESOLUTION_640x480;
    float scrWidth = 640.0f, scrHeight = 480.0f;

    grGlideInit();
    if (!grSstQueryHardware(&hwconfig)) {
        fprintf(stderr, "grSstQueryHardware failed\n");
        return 1;
    }
    grSstSelect(0);
    if (!grSstWinOpen(0, resolution, GR_REFRESH_60Hz,
                      GR_COLORFORMAT_ABGR, GR_ORIGIN_UPPER_LEFT,
                      2, 1)) {
        fprintf(stderr, "grSstWinOpen failed\n");
        return 1;
    }

    f = fopen("lfbtest.txt", "w");
    if (!f) f = stderr;

    fprintf(f, "lfbtest: LFB write+readback on IRIX O2\n");
    grGlideGetVersion(version);
    fprintf(f, "Glide: %s\n", version);

    /*
     * Test at x=0 (4-byte aligned, even) - 8 pixels (even count)
     * Test at x=1 (4-byte+2, odd)        - 8 pixels (even count, odd-start + trailing)
     * Test at x=2 (4-byte aligned, even) - 7 pixels (odd count = trailing pixel)
     * Test at x=1 (odd)                  - 4 pixels (small odd-start case)
     */
    run_test(0, 8, f);
    run_test(1, 8, f);
    run_test(2, 7, f);
    run_test(1, 4, f);

    /* Also test raw LFB ptr reads to understand byte layout */
    {
        GrLfbInfo_t info;
        FxU16 wbuf[4];
        int i;

        wbuf[0] = 0x1234;
        wbuf[1] = 0x5678;
        wbuf[2] = 0x9ABC;
        wbuf[3] = 0xDEF0;

        info.size = sizeof(info);
        fprintf(f, "\n--- Raw LFB ptr test (x=0, n=4) ---\n");

        if (grLfbLock(GR_LFB_WRITE_ONLY | GR_LFB_NOIDLE,
                      GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_565,
                      GR_ORIGIN_UPPER_LEFT,
                      FXFALSE, &info)) {
            volatile FxU16 *p = (volatile FxU16 *)info.lfbPtr;
            for (i = 0; i < 4; i++)
                p[i] = wbuf[i];
            grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
            fprintf(f, "  wrote: 0x%04X 0x%04X 0x%04X 0x%04X\n",
                    (unsigned)wbuf[0], (unsigned)wbuf[1],
                    (unsigned)wbuf[2], (unsigned)wbuf[3]);
        }

        if (grLfbLock(GR_LFB_READ_ONLY,
                      GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_ANY,
                      GR_ORIGIN_UPPER_LEFT,
                      FXFALSE, &info)) {
            volatile const FxU32 *p32 = (volatile const FxU32 *)info.lfbPtr;
            FxU32 w0 = p32[0];  /* columns 0+1 */
            FxU32 w1 = p32[1];  /* columns 2+3 */
            fprintf(f, "  32-bit word[0]=0x%08X  hi=0x%04X lo=0x%04X\n",
                    (unsigned)w0, (unsigned)(w0>>16), (unsigned)(w0&0xFFFF));
            fprintf(f, "  32-bit word[1]=0x%08X  hi=0x%04X lo=0x%04X\n",
                    (unsigned)w1, (unsigned)(w1>>16), (unsigned)(w1&0xFFFF));
            grLfbUnlock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER);
        }
    }

    /* 16-bit read test: does MACE 16-bit PCI read work on LFB aperture? */
    {
        GrLfbInfo_t info;
        info.size = sizeof(info);
        fprintf(f, "\n--- 16-bit read test ---\n");
        /* Write known values */
        if (grLfbLock(GR_LFB_WRITE_ONLY|GR_LFB_NOIDLE, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_565, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            volatile FxU16 *p = (volatile FxU16*)info.lfbPtr;
            p[0] = 0x1234;
            p[1] = 0x5678;
            p[2] = 0x9ABC;
            p[3] = 0xDEF0;
            grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
        }
        if (grLfbLock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_ANY, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            volatile const FxU16 *p16 = (volatile const FxU16*)info.lfbPtr;
            fprintf(f, "  16-bit reads: p[0]=0x%04X p[1]=0x%04X p[2]=0x%04X p[3]=0x%04X\n",
                    (unsigned)p16[0], (unsigned)p16[1],
                    (unsigned)p16[2], (unsigned)p16[3]);
            fprintf(f, "  (expected:   0x1234     0x5678     0x9ABC     0xDEF0)\n");
            grLfbUnlock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER);
        }
        fflush(f);
    }

    /* Isolation tests: write vs read side */
    {
        GrLfbInfo_t info;
        FxU32 w0;
        info.size = sizeof(info);
        fprintf(f, "\n--- Isolation tests ---\n");

        /* T1: write col1=0x5678 ONLY (col0 cleared separately first) */
        if (grLfbLock(GR_LFB_WRITE_ONLY|GR_LFB_NOIDLE, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_565, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            volatile FxU16 *p = (volatile FxU16*)info.lfbPtr;
            p[0] = 0x0000;
            grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
        }
        if (grLfbLock(GR_LFB_WRITE_ONLY|GR_LFB_NOIDLE, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_565, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            volatile FxU16 *p = (volatile FxU16*)info.lfbPtr;
            p[1] = 0x5678;
            grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
        }
        if (grLfbLock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_ANY, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            w0 = *(volatile const FxU32*)info.lfbPtr;
            fprintf(f, "  T1 col1-only 0x5678: word[0]=0x%08X hi=0x%04X lo=0x%04X\n",
                    (unsigned)w0, (unsigned)(w0>>16), (unsigned)(w0&0xFFFF));
            grLfbUnlock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER);
        }

        /* T2: write col0=0x5678 ONLY (col1 cleared) */
        if (grLfbLock(GR_LFB_WRITE_ONLY|GR_LFB_NOIDLE, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_565, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            volatile FxU16 *p = (volatile FxU16*)info.lfbPtr;
            p[0] = 0x5678;
            p[1] = 0x0000;
            grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
        }
        if (grLfbLock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_ANY, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            w0 = *(volatile const FxU32*)info.lfbPtr;
            fprintf(f, "  T2 col0-only 0x5678: word[0]=0x%08X hi=0x%04X lo=0x%04X\n",
                    (unsigned)w0, (unsigned)(w0>>16), (unsigned)(w0&0xFFFF));
            grLfbUnlock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER);
        }

        /* T3: write both col0=0x5678 col1=0x5678 in ONE lock */
        if (grLfbLock(GR_LFB_WRITE_ONLY|GR_LFB_NOIDLE, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_565, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            volatile FxU16 *p = (volatile FxU16*)info.lfbPtr;
            p[0] = 0x5678;
            p[1] = 0x5678;
            grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
        }
        if (grLfbLock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_ANY, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            w0 = *(volatile const FxU32*)info.lfbPtr;
            fprintf(f, "  T3 both=0x5678:      word[0]=0x%08X hi=0x%04X lo=0x%04X\n",
                    (unsigned)w0, (unsigned)(w0>>16), (unsigned)(w0&0xFFFF));
            grLfbUnlock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER);
        }

        /* T4: write col1=0x5678, then read back twice (second read same or differs?) */
        if (grLfbLock(GR_LFB_WRITE_ONLY|GR_LFB_NOIDLE, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_565, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            volatile FxU16 *p = (volatile FxU16*)info.lfbPtr;
            p[0] = 0x1234;
            p[1] = 0x5678;
            grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
        }
        if (grLfbLock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_ANY, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            volatile const FxU32 *p32 = (volatile const FxU32*)info.lfbPtr;
            FxU32 a = p32[0];
            FxU32 b = p32[0];
            fprintf(f, "  T4 1234+5678 read1=0x%08X read2=0x%08X\n",
                    (unsigned)a, (unsigned)b);
            grLfbUnlock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER);
        }
        fflush(f);
    }

    /*
     * 32-bit write test: write two pixels as a single 32-bit store.
     *
     * MACE 2-byte-pair swap theory: V=(P0<<16)|P1  → col0=P0, col1=P1
     * MACE full 4-byte reversal:    V=(P1<<16)|P0  → col0=P0, col1=P1
     *
     * Write V=0x12345678 = (0x1234<<16)|0x5678:
     *   2-byte-pair: col0=0x1234, col1=0x5678  (formula correct)
     *   full-reversal: col0=0x5678, col1=0x1234 (pixels swapped)
     *
     * Then write V=0x56781234 = (0x5678<<16)|0x1234:
     *   2-byte-pair: col0=0x5678, col1=0x1234
     *   full-reversal: col0=0x1234, col1=0x5678 (formula correct)
     */
    {
        GrLfbInfo_t info;
        info.size = sizeof(info);
        fprintf(f, "\n--- 32-bit write test ---\n");

        /* Write V=(P0<<16)|P1 = 0x12345678 */
        if (grLfbLock(GR_LFB_WRITE_ONLY|GR_LFB_NOIDLE, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_565, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            volatile FxU32 *p32 = (volatile FxU32*)info.lfbPtr;
            *p32 = (FxU32)0x12345678u;  /* V=(P0<<16)|P1 */
            grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
        }
        if (grLfbLock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_ANY, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            volatile const FxU16 *p16 = (volatile const FxU16*)info.lfbPtr;
            FxU16 c0 = p16[0], c1 = p16[1];
            fprintf(f, "  V=0x12345678: col0=0x%04X col1=0x%04X\n",
                    (unsigned)c0, (unsigned)c1);
            fprintf(f, "  (2byte-pair expect: 0x1234 0x5678)\n");
            fprintf(f, "  (full-rev  expect:  0x5678 0x1234)\n");
            grLfbUnlock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER);
        }

        /* Write V=(P1<<16)|P0 = 0x56781234 */
        if (grLfbLock(GR_LFB_WRITE_ONLY|GR_LFB_NOIDLE, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_565, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            volatile FxU32 *p32 = (volatile FxU32*)info.lfbPtr;
            *p32 = (FxU32)0x56781234u;  /* V=(P1<<16)|P0 */
            grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
        }
        if (grLfbLock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_ANY, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            volatile const FxU16 *p16 = (volatile const FxU16*)info.lfbPtr;
            FxU16 c0 = p16[0], c1 = p16[1];
            fprintf(f, "  V=0x56781234: col0=0x%04X col1=0x%04X\n",
                    (unsigned)c0, (unsigned)c1);
            fprintf(f, "  (2byte-pair expect: 0x5678 0x1234)\n");
            fprintf(f, "  (full-rev  expect:  0x1234 0x5678)\n");
            grLfbUnlock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER);
        }
        fflush(f);
    }

    /*
     * Prefetch-order test: read word at offset+4 BEFORE offset+0.
     * CRIME prefetches 8 bytes at a time; reading the second word first
     * may force a separate PCI transaction for the first word, bypassing
     * the prefetch corruption.
     *
     * Write col0=0x5678 (even), col1=0x1234 (odd).
     * Read p32[1] first (cols 2,3 — just for ordering side effect),
     * then read p32[0] (cols 0,1) — is col_even correct now?
     * Also try reading p32[0] twice (are they both corrupted or does the
     * second read give a correct value?).
     */
    {
        GrLfbInfo_t info;
        info.size = sizeof(info);
        fprintf(f, "\n--- Prefetch-order test ---\n");
        /* Write col0=0x5678, col1=0x1234 via correct 32-bit write */
        if (grLfbLock(GR_LFB_WRITE_ONLY|GR_LFB_NOIDLE, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_565, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            volatile FxU32 *p32 = (volatile FxU32*)info.lfbPtr;
            p32[0] = ((FxU32)0x1234u << 16) | 0x5678u;  /* col0=0x5678, col1=0x1234 */
            p32[1] = ((FxU32)0x9ABCu << 16) | 0xDEF0u;  /* col2=0xDEF0, col3=0x9ABC */
            grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
        }
        if (grLfbLock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_ANY, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            volatile const FxU32 *p32 = (volatile const FxU32*)info.lfbPtr;
            FxU32 w0_first, w0_second, w1_before, w0_after, w0_a, w0_b, dummy;

            /* Standard order: read p32[0] first */
            w0_first  = p32[0];
            w0_second = p32[0];  /* read p32[0] again immediately */

            /* Reversed order: read p32[1] first, then p32[0] */
            w1_before = p32[1];
            w0_after  = p32[0];

            /* Also: read p32[1] again after reading p32[0] to see if order matters */
            w0_a  = p32[0];
            dummy = p32[1]; (void)dummy;
            w0_b  = p32[0];

            fprintf(f, "  wrote col0=0x5678 col1=0x1234 col2=0xDEF0 col3=0x9ABC\n");
            fprintf(f, "  p32[0] direct (1st read): col_even=0x%04X col_odd=0x%04X\n",
                    (unsigned)(w0_first&0xFFFF), (unsigned)(w0_first>>16));
            fprintf(f, "  p32[0] direct (2nd read): col_even=0x%04X col_odd=0x%04X\n",
                    (unsigned)(w0_second&0xFFFF), (unsigned)(w0_second>>16));
            fprintf(f, "  p32[1] before p32[0]: p32[1]=0x%08X p32[0]=0x%08X\n",
                    (unsigned)w1_before, (unsigned)w0_after);
            fprintf(f, "    -> col_even=0x%04X (expect 0x5678) col_odd=0x%04X\n",
                    (unsigned)(w0_after&0xFFFF), (unsigned)(w0_after>>16));
            fprintf(f, "  p32[0] then p32[1] then p32[0]: first=0x%04X second=0x%04X\n",
                    (unsigned)(w0_a&0xFFFF), (unsigned)(w0_b&0xFFFF));
            grLfbUnlock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER);
        }
        fflush(f);
    }

    /*
     * Byte-read test A: byte reads AFTER a 32-bit read (uses CRIME's buffer).
     * Expected: same corrupted value as word & 0xFFFF.
     */
    {
        GrLfbInfo_t info;
        info.size = sizeof(info);
        fprintf(f, "\n--- Byte-read test A (32-bit read first, then bytes) ---\n");
        if (grLfbLock(GR_LFB_WRITE_ONLY|GR_LFB_NOIDLE, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_565, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            volatile FxU32 *p32 = (volatile FxU32*)info.lfbPtr;
            *p32 = ((FxU32)0x1234u << 16) | 0x5678u; /* col0=0x5678, col1=0x1234 */
            grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
        }
        if (grLfbLock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_ANY, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            volatile const FxU32 *p32 = (volatile const FxU32*)info.lfbPtr;
            volatile const FxU8  *p8  = (volatile const FxU8 *)p32;
            FxU32 word;
            FxU16 col_even_32, col_odd_32, col_even_b8, col_odd_b8;
            word         = *p32;                              /* 32-bit read first */
            col_even_32  = (FxU16)(word & 0xFFFF);
            col_odd_32   = (FxU16)(word >> 16);
            col_even_b8  = ((FxU16)p8[2] << 8) | (FxU16)p8[3]; /* bytes from buffer */
            col_odd_b8   = ((FxU16)p8[0] << 8) | (FxU16)p8[1];
            fprintf(f, "  wrote col0(even)=0x5678  col1(odd)=0x1234\n");
            fprintf(f, "  32-bit: col_even=0x%04X col_odd=0x%04X\n",
                    (unsigned)col_even_32, (unsigned)col_odd_32);
            fprintf(f, "  bytes after 32-bit: col_even=0x%04X col_odd=0x%04X\n",
                    (unsigned)col_even_b8, (unsigned)col_odd_b8);
            grLfbUnlock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER);
        }
        fflush(f);
    }

    /*
     * Byte-read test B: PURE byte reads with NO prior 32-bit read.
     * CRIME only issues 8-byte PCI bursts for 32-bit (or wider) CPU loads.
     * For 1-byte CPU loads it should issue a 1-byte PCI read, which cannot
     * trigger the Voodoo LFB burst-read corruption.
     * If col_even comes back correct here, the fix is to use byte reads in
     * glfb.c's MIPS read path instead of 32-bit reads.
     */
    {
        GrLfbInfo_t info;
        info.size = sizeof(info);
        fprintf(f, "\n--- Byte-read test B (pure byte reads, no prior 32-bit) ---\n");
        if (grLfbLock(GR_LFB_WRITE_ONLY|GR_LFB_NOIDLE, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_565, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            volatile FxU32 *p32 = (volatile FxU32*)info.lfbPtr;
            *p32 = ((FxU32)0x1234u << 16) | 0x5678u; /* col0=0x5678, col1=0x1234 */
            grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
        }
        if (grLfbLock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER,
                      GR_LFBWRITEMODE_ANY, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            /* ONLY byte reads — no 32-bit access to this address at all */
            volatile const FxU8 *p8 = (volatile const FxU8*)info.lfbPtr;
            FxU16 col_even, col_odd;
            col_even = ((FxU16)p8[2] << 8) | (FxU16)p8[3];
            col_odd  = ((FxU16)p8[0] << 8) | (FxU16)p8[1];
            fprintf(f, "  wrote col0(even)=0x5678  col1(odd)=0x1234\n");
            fprintf(f, "  pure-byte: col_even=0x%04X col_odd=0x%04X\n",
                    (unsigned)col_even, (unsigned)col_odd);
            fprintf(f, "  %s\n",
                    (col_even == 0x5678) ? "CORRECT! pure-byte reads bypass burst bug"
                                        : "WRONG: pure-byte reads still corrupt");
            grLfbUnlock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER);
        }
        fflush(f);
    }

    /*
     * Corruption symmetry test: determine if the col_even MACE read bug is
     * a symmetric bit-swap (self-inverse) or one-directional.
     *
     * Known:  high-byte bit4=1,bit3=0  -> XOR 0x18 (e.g. 0x10xx reads as 0x08xx)
     *         low-byte  bit6=1,bit5=0  -> XOR 0x60 (e.g. xx40 reads as xx20)
     *         low-byte  bit1=1,bit0=0  -> XOR 0x03 (e.g. xx02 reads as xx01)
     *
     * Symmetric swap means the reverse direction also corrupts:
     *         high-byte bit4=0,bit3=1  -> XOR 0x18 (0x08xx -> 0x10xx)
     *         low-byte  bit6=0,bit5=1  -> XOR 0x60 (xx20 -> xx40)
     *         low-byte  bit1=0,bit0=1  -> XOR 0x03 (xx01 -> xx02)
     *
     * If all "symmetric?" entries show SWAPPED, the corruption is reversible
     * in software by applying the same XOR conditions to the read value.
     */
    {
        static const struct {
            FxU16 write;
            FxU16 expect_swapped;
            const char *desc;
        } cases[] = {
            { 0x1000, 0x0800, "h bit4=1,bit3=0 (known corrupt)" },
            { 0x0800, 0x1000, "h bit4=0,bit3=1 (symmetric?)" },
            { 0x0040, 0x0020, "l bit6=1,bit5=0 (known corrupt)" },
            { 0x0020, 0x0040, "l bit6=0,bit5=1 (symmetric?)" },
            { 0x0002, 0x0001, "l bit1=1,bit0=0 (known corrupt)" },
            { 0x0001, 0x0002, "l bit1=0,bit0=1 (symmetric?)" },
            { 0x0000, 0x0000, "all-zero (no corruption expected)" },
            { 0xFFFF, 0xFFFF, "all-ones (no corruption expected)" },
            { 0x5678, 0x4E78, "original test pixel (known corrupt)" },
        };
        int nc = (int)(sizeof(cases) / sizeof(cases[0]));
        int i;
        GrLfbInfo_t info;
        info.size = sizeof(info);
        fprintf(f, "\n--- Corruption symmetry test ---\n");
        for (i = 0; i < nc; i++) {
            if (grLfbLock(GR_LFB_WRITE_ONLY|GR_LFB_NOIDLE, GR_BUFFER_BACKBUFFER,
                          GR_LFBWRITEMODE_565, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
                volatile FxU32 *p32 = (volatile FxU32*)info.lfbPtr;
                /* col0=even(8B-aligned), col1=0x1234(odd) */
                p32[0] = ((FxU32)0x1234u << 16) | (FxU32)cases[i].write;
                grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
            }
            if (grLfbLock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER,
                          GR_LFBWRITEMODE_ANY, GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
                volatile const FxU32 *p32 = (volatile const FxU32*)info.lfbPtr;
                FxU16 got = (FxU16)(p32[0] & 0xFFFF);
                const char *verdict;
                if (got == cases[i].write)
                    verdict = "CLEAN";
                else if (got == cases[i].expect_swapped)
                    verdict = "SWAPPED";
                else
                    verdict = "???";
                fprintf(f, "  write=0x%04X read=0x%04X [%s] %s\n",
                        (unsigned)cases[i].write, (unsigned)got,
                        verdict, cases[i].desc);
                grLfbUnlock(GR_LFB_READ_ONLY, GR_BUFFER_BACKBUFFER);
            }
        }
        fflush(f);
    }

    /*
     * Visual display test: write stripe patterns, buffer-swap to display.
     * Bypasses grLfbReadRegion — what you SEE is what Voodoo DRAM has.
     *
     * Screen layout (640x480):
     *
     *  rows   0-159  TOP BAND:    32-bit writes, 80-pixel-wide alternating RED / GREEN
     *  rows 160-319  MIDDLE BAND: 16-bit writes, 80-pixel-wide alternating RED / GREEN
     *  rows 320-479  BOTTOM BAND: solid BLUE (reference, both methods same result)
     *
     * All bands should look identical (80px red | 80px green | 80px red ...).
     * If any band looks different (wrong colours or shifted edges) that band's
     * write method has a bug.
     */
    fprintf(f, "\n--- Visual test: see screen ---\n");
    fprintf(f, "TOP(32-bit) MID(16-bit) BOT(blue ref) — all should match\n");
    fflush(f);

    {
        const FxU16 RED   = 0xF800u;
        const FxU16 GREEN = 0x07E0u;
        const FxU16 BLUE  = 0x001Fu;
        const int   STRIPE = 81;          /* pixels per colour stripe (odd = mixed-colour pairs at boundary) */
        GrLfbInfo_t info;
        int y, x;

        info.size = sizeof(info);
        grBufferClear(0, 0, 0);

        /* TOP BAND: 32-bit writes, 80-px stripes */
        if (grLfbLock(GR_LFB_WRITE_ONLY|GR_LFB_NOIDLE,
                      GR_BUFFER_BACKBUFFER, GR_LFBWRITEMODE_565,
                      GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            FxU32 stride32 = info.strideInBytes / 4;
            volatile FxU32 *base = (volatile FxU32 *)info.lfbPtr;
            for (y = 0; y < 160; y++) {
                volatile FxU32 *row = base + y * stride32;
                for (x = 0; x < 320; x++) {  /* 320 FxU32 = 640 pixels */
                    int col0 = x * 2;          /* left  (even) pixel of this pair */
                    int col1 = x * 2 + 1;      /* right (odd)  pixel of this pair */
                    FxU16 c0 = ((col0 / STRIPE) & 1) ? GREEN : RED;
                    FxU16 c1 = ((col1 / STRIPE) & 1) ? GREEN : RED;
                    /* MACE full 4-byte endian reversal: to place c0 at col_even
                     * and c1 at col_odd, the 32-bit value must be (c1<<16)|c0. */
                    row[x] = ((FxU32)c1 << 16) | c0;
                }
            }
            grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
        }

        /* MIDDLE BAND: 16-bit writes, 80-px stripes */
        if (grLfbLock(GR_LFB_WRITE_ONLY|GR_LFB_NOIDLE,
                      GR_BUFFER_BACKBUFFER, GR_LFBWRITEMODE_565,
                      GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            FxU32 stride16 = info.strideInBytes / 2;
            volatile FxU16 *base16 = (volatile FxU16 *)info.lfbPtr;
            for (y = 160; y < 320; y++) {
                volatile FxU16 *row = base16 + y * stride16;
                for (x = 0; x < 640; x++) {
                    row[x] = ((x / STRIPE) & 1) ? GREEN : RED;
                }
            }
            grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
        }

        /* BOTTOM BAND: solid blue via 32-bit writes (reference) */
        if (grLfbLock(GR_LFB_WRITE_ONLY|GR_LFB_NOIDLE,
                      GR_BUFFER_BACKBUFFER, GR_LFBWRITEMODE_565,
                      GR_ORIGIN_UPPER_LEFT, FXFALSE, &info)) {
            FxU32 stride32 = info.strideInBytes / 4;
            volatile FxU32 *base = (volatile FxU32 *)info.lfbPtr;
            FxU32 bb = ((FxU32)BLUE << 16) | BLUE;
            for (y = 320; y < 480; y++) {
                volatile FxU32 *row = base + y * stride32;
                for (x = 0; x < 320; x++)
                    row[x] = bb;
            }
            grLfbUnlock(GR_LFB_WRITE_ONLY, GR_BUFFER_BACKBUFFER);
        }

        grBufferSwap(1);
        tlSleep(8);
    }

    if (f != stderr) fclose(f);
    grGlideShutdown();
    return 0;
}

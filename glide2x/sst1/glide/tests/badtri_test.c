/*
** badtri_test.c - Malformed triangle hang reproducer for Voodoo1/IRIX
**
** Tests whether submitting malformed triangle data to the Voodoo1 causes
** the GPU pipeline to hang permanently (same symptom as Quake2/miniGL freeze).
**
** Phase 1: Draw VALID_FRAMES valid flat-shaded colored triangles (chip should work)
** Phase 2: Submit one malformed triangle (type selected via -t flag)
** Phase 3: Attempt RECOVER_FRAMES more valid triangles, report chip status
**
** If the chip hangs, grBufferSwap/grSstStatus will block and this program
** will freeze — matching the observed Quake2/miniGL behavior.
**
** Usage:  badtri_test [-t <type>] [-n <frames>]
**
**   -t 0   zero-area triangle (all 3 vertices identical)          [default]
**   -t 1   zero-area triangle (collinear vertices)
**   -t 2   NaN in all vertex X/Y coordinates  (0.0f/0.0f)
**   -t 3   Inf in all vertex X/Y coordinates  (1.0f/0.0f)
**   -t 4   oow = 0.0f  (infinite depth / division by zero)
**   -t 5   oow = -1.0f (behind-camera vertex)
**   -t 6   extreme screen coordinates (far outside viewport)
**   -n N   number of valid frames before the bad triangle (default 30)
**
** Build: smake -f Makefile.tests_irix badtri_test
** Run:   ./badtri_test -t 0
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <glide.h>
#include "tlib.h"

/* How many valid frames to draw after the bad triangle */
#define RECOVER_FRAMES 10

/* Screen dimensions */
#define SCR_W 640.0f
#define SCR_H 480.0f

/* ------------------------------------------------------------------ */
/* grSstStatus bits we care about                                       */
/* ------------------------------------------------------------------ */
#define SST_BUSY      (1u << 9)

/* ------------------------------------------------------------------ */
/* Build a valid colored triangle vertex                                */
/* ------------------------------------------------------------------ */
static void make_vertex(GrVertex *v, float x, float y, float r, float g, float b)
{
    v->x   = x;
    v->y   = y;
    v->z   = 1.0f;
    v->oow = 1.0f;      /* orthographic: 1/w = 1 */
    v->r   = r;
    v->g   = g;
    v->b   = b;
    v->a   = 255.0f;
    /* Texture coords not used (TMU disabled) */
    v->tmuvtx[0].sow = 0.0f;
    v->tmuvtx[0].tow = 0.0f;
}

/* ------------------------------------------------------------------ */
/* Draw one valid frame: a large brightly colored triangle              */
/* ------------------------------------------------------------------ */
static void draw_valid_frame(int frame_num)
{
    GrVertex v0, v1, v2;
    float phase = (float)frame_num * 0.08f;

    /* Slowly rotating color cycle to make it obvious the chip is alive */
    float r0 = 255.0f, g0 = (float)(50  + (frame_num * 5) % 200), b0 = 50.0f;
    float r1 = 50.0f,  g1 = 255.0f,                                b1 = (float)(50  + (frame_num * 7) % 200);
    float r2 = 50.0f,  g2 = 50.0f,                                 b2 = 255.0f;

    /* Screen-space triangle — large enough to fill most of the screen */
    float cx = SCR_W * 0.5f;
    float cy = SCR_H * 0.5f;
    float radius = 180.0f;

    make_vertex(&v0,
                cx + radius * (float)cos(phase),
                cy + radius * (float)sin(phase),
                r0, g0, b0);
    make_vertex(&v1,
                cx + radius * (float)cos(phase + 2.094f),   /* +120 deg */
                cy + radius * (float)sin(phase + 2.094f),
                r1, g1, b1);
    make_vertex(&v2,
                cx + radius * (float)cos(phase + 4.189f),   /* +240 deg */
                cy + radius * (float)sin(phase + 4.189f),
                r2, g2, b2);

    grDrawTriangle(&v0, &v1, &v2);
}

/* ------------------------------------------------------------------ */
/* Print chip status word with interpretation                           */
/* ------------------------------------------------------------------ */
static void print_status(int phase, int frame)
{
    FxU32 st = grSstStatus();
    int busy = (st & SST_BUSY) ? 1 : 0;
    int fifo = (int)((st >> 12) & 0xffff);

    fprintf(stderr,
        "[phase%d frame%02d] grSstStatus=0x%08x  busy=%d  fifoLevel=%d\n",
        phase, frame, (unsigned)st, busy, fifo);
    fflush(stderr);
}

/* ------------------------------------------------------------------ */
/* Submit one malformed triangle and print what we did                  */
/* ------------------------------------------------------------------ */
static void send_bad_triangle(int type)
{
    GrVertex v0, v1, v2;
    float nan_val, inf_val;

    /* Compute NaN/Inf portably without relying on math.h macros */
    {
        volatile float zero = 0.0f;
        nan_val = zero / zero;   /* NaN  */
        inf_val = 1.0f / zero;   /* +Inf */
    }

    /* Start with valid-looking vertices, then corrupt per type */
    make_vertex(&v0, 100.0f, 100.0f, 255.0f, 0.0f, 0.0f);
    make_vertex(&v1, 300.0f, 400.0f,   0.0f, 255.0f, 0.0f);
    make_vertex(&v2, 500.0f, 100.0f,   0.0f, 0.0f, 255.0f);

    switch (type) {
    case 0:
        fprintf(stderr, "[badtri] type 0: zero-area triangle (3 identical vertices)\n");
        fflush(stderr);
        /* All three vertices at same point → area = 0 */
        make_vertex(&v0, 320.0f, 240.0f, 255.0f,   0.0f,   0.0f);
        make_vertex(&v1, 320.0f, 240.0f,   0.0f, 255.0f,   0.0f);
        make_vertex(&v2, 320.0f, 240.0f,   0.0f,   0.0f, 255.0f);
        break;

    case 1:
        fprintf(stderr, "[badtri] type 1: zero-area triangle (collinear vertices)\n");
        fflush(stderr);
        /* Three points on the same horizontal line → area = 0 */
        make_vertex(&v0, 100.0f, 240.0f, 255.0f,   0.0f,   0.0f);
        make_vertex(&v1, 320.0f, 240.0f,   0.0f, 255.0f,   0.0f);
        make_vertex(&v2, 540.0f, 240.0f,   0.0f,   0.0f, 255.0f);
        break;

    case 2:
        fprintf(stderr, "[badtri] type 2: NaN in all vertex X/Y coordinates\n");
        fflush(stderr);
        v0.x = nan_val; v0.y = nan_val;
        v1.x = nan_val; v1.y = nan_val;
        v2.x = nan_val; v2.y = nan_val;
        break;

    case 3:
        fprintf(stderr, "[badtri] type 3: Inf in all vertex X/Y coordinates\n");
        fflush(stderr);
        v0.x = inf_val; v0.y = inf_val;
        v1.x = inf_val; v1.y = inf_val;
        v2.x = inf_val; v2.y = inf_val;
        break;

    case 4:
        fprintf(stderr, "[badtri] type 4: oow = 0.0f on all vertices\n");
        fflush(stderr);
        v0.oow = 0.0f;
        v1.oow = 0.0f;
        v2.oow = 0.0f;
        break;

    case 5:
        fprintf(stderr, "[badtri] type 5: oow = -1.0f (behind-camera) on all vertices\n");
        fflush(stderr);
        v0.oow = -1.0f;
        v1.oow = -1.0f;
        v2.oow = -1.0f;
        break;

    case 6:
        fprintf(stderr, "[badtri] type 6: extreme screen coordinates (x=1e9, y=1e9)\n");
        fflush(stderr);
        v0.x = 1.0e9f; v0.y = 1.0e9f;
        v1.x = 1.0e9f; v1.y = 2.0e9f;
        v2.x = 2.0e9f; v2.y = 1.5e9f;
        break;

    default:
        fprintf(stderr, "[badtri] unknown type %d, using type 0 (zero-area)\n", type);
        make_vertex(&v0, 320.0f, 240.0f, 255.0f,   0.0f,   0.0f);
        make_vertex(&v1, 320.0f, 240.0f,   0.0f, 255.0f,   0.0f);
        make_vertex(&v2, 320.0f, 240.0f,   0.0f,   0.0f, 255.0f);
        break;
    }

    fflush(stderr);
    grDrawTriangle(&v0, &v1, &v2);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    GrHwConfiguration hwconfig;
    char version[80];
    int valid_frames = 30;
    int bad_type     = 0;
    int i;
    char match;
    char **remArgs;
    int rv;

    /* --- Parse arguments --- */
    while ((rv = tlGetOpt(argc, argv, "nt", &match, &remArgs)) != 0) {
        if (rv == -1) break;
        if (match == 'n') valid_frames = atoi(remArgs[0]);
        if (match == 't') bad_type     = atoi(remArgs[0]);
    }

    tlSetScreen(SCR_W, SCR_H);
    grGlideGetVersion(version);
    fprintf(stderr, "badtri_test: malformed triangle hang reproducer\n");
    fprintf(stderr, "Glide: %s\n", version);
    fprintf(stderr, "Phase 1: %d valid frames\n", valid_frames);
    fprintf(stderr, "Phase 2: 1 malformed triangle (type %d)\n", bad_type);
    fprintf(stderr, "Phase 3: %d recovery frames + status poll\n", RECOVER_FRAMES);
    fflush(stderr);

    /* --- Glide init --- */
    grGlideInit();
    if (!grSstQueryHardware(&hwconfig)) {
        fprintf(stderr, "badtri_test: no 3Dfx hardware detected\n");
        grGlideShutdown();
        return 1;
    }
    grSstSelect(0);
    if (!grSstWinOpen(0,
                      GR_RESOLUTION_640x480,
                      GR_REFRESH_60Hz,
                      GR_COLORFORMAT_ABGR,
                      GR_ORIGIN_UPPER_LEFT,
                      2, 1)) {
        fprintf(stderr, "badtri_test: grSstWinOpen failed\n");
        grGlideShutdown();
        return 1;
    }

    /* --- Render state: flat-shaded, no texture, no depth buffer --- */
    grColorCombine(GR_COMBINE_FUNCTION_LOCAL,
                   GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_LOCAL_ITERATED,
                   GR_COMBINE_OTHER_NONE,
                   FXFALSE);
    grAlphaBlendFunction(GR_BLEND_ONE,  GR_BLEND_ZERO,
                         GR_BLEND_ONE,  GR_BLEND_ZERO);
    grDepthBufferMode(GR_DEPTHBUFFER_DISABLE);
    grCullMode(GR_CULL_DISABLE);
    grClipWindow(0, 0, 640, 480);

    /* ================================================================
     * PHASE 1: valid frames
     * ================================================================ */
    fprintf(stderr, "\n--- PHASE 1: valid triangles (%d frames) ---\n", valid_frames);
    fflush(stderr);

    for (i = 0; i < valid_frames; i++) {
        grBufferClear(0x000010, 0, 0);
        draw_valid_frame(i);
        grBufferSwap(1);

        if (i == 0 || i == valid_frames - 1)
            print_status(1, i);

        if (tlKbHit()) {
            tlGetCH();
            fprintf(stderr, "badtri_test: key pressed, aborting phase 1 early at frame %d\n", i);
            break;
        }
    }

    fprintf(stderr, "Phase 1 complete. Chip appears %s.\n",
            (grSstStatus() & SST_BUSY) ? "BUSY" : "idle");
    fflush(stderr);

    /* Brief pause so the operator can see the valid output */
    sleep(1);

    /* ================================================================
     * PHASE 2: one malformed triangle
     * ================================================================ */
    fprintf(stderr, "\n--- PHASE 2: sending malformed triangle (type %d) ---\n", bad_type);
    fflush(stderr);

    grBufferClear(0x200000, 0, 0);   /* dark red background = "danger" frame */
    send_bad_triangle(bad_type);

    fprintf(stderr, "[phase2] grDrawTriangle returned — attempting grBufferSwap...\n");
    fflush(stderr);

    grBufferSwap(1);

    fprintf(stderr, "[phase2] grBufferSwap returned.\n");
    fflush(stderr);

    print_status(2, 0);

    /* ================================================================
     * PHASE 3: recovery attempt
     * ================================================================ */
    fprintf(stderr, "\n--- PHASE 3: attempting %d recovery frames ---\n", RECOVER_FRAMES);
    fflush(stderr);

    for (i = 0; i < RECOVER_FRAMES; i++) {
        fprintf(stderr, "[phase3 frame%02d] clearing buffer...\n", i);
        fflush(stderr);

        grBufferClear(0x001000, 0, 0);   /* dark green = "recovery" frame */

        fprintf(stderr, "[phase3 frame%02d] drawing valid triangle...\n", i);
        fflush(stderr);

        draw_valid_frame(valid_frames + i);

        fprintf(stderr, "[phase3 frame%02d] calling grBufferSwap...\n", i);
        fflush(stderr);

        grBufferSwap(1);

        print_status(3, i);

        if (tlKbHit()) {
            tlGetCH();
            fprintf(stderr, "badtri_test: key pressed, stopping recovery at frame %d\n", i);
            break;
        }
    }

    /* ================================================================
     * Final status poll
     * ================================================================ */
    fprintf(stderr, "\n--- Final status ---\n");
    {
        int polls;
        int still_busy = 0;
        for (polls = 0; polls < 5; polls++) {
            FxU32 st = grSstStatus();
            fprintf(stderr, "  poll %d: 0x%08x  busy=%d  fifoLevel=%d\n",
                    polls,
                    (unsigned)st,
                    (st & SST_BUSY) ? 1 : 0,
                    (int)((st >> 12) & 0xffff));
            fflush(stderr);
            if (st & SST_BUSY) still_busy++;
            usleep(5000);
        }
        fprintf(stderr, "\nResult: chip was busy on %d/5 final polls.\n", still_busy);
        if (still_busy == 5)
            fprintf(stderr, "=> HANG REPRODUCED: chip is stuck busy after malformed triangle\n");
        else if (still_busy == 0)
            fprintf(stderr, "=> Chip recovered OK (no hang from this malformation type)\n");
        else
            fprintf(stderr, "=> Chip was transiently busy (%d/5) -- uncertain\n", still_busy);
    }
    fflush(stderr);

    grGlideShutdown();
    return 0;
}

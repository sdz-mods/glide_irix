/*
** bad_cube.c - cube.c WITHOUT the zero-area triangle guard.
**
** Identical to cube.c except draw_face() no longer checks screen-space area
** before calling grDrawTriangle.  When the cube rotates to an edge-on angle,
** degenerate (zero-area) triangles will be submitted to the Voodoo1.
**
** Purpose: confirm whether the chip hangs on zero-area triangles submitted
** via the real textured-triangle path (vs the synthetic badtri_test which
** found no hang with flat-shaded triangles).
**
** Controls: any key quits.
** Usage:    bad_cube [-n <frames>]
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glide.h>
#include "tlib.h"

/* ------------------------------------------------------------------ */
/* System info collected from /sbin/hinv                               */
/* ------------------------------------------------------------------ */
#define INFO_LEN 80

typedef struct {
    char sys[INFO_LEN];
    char cpu[INFO_LEN];
    char mhz[16];
    char fpu[INFO_LEN];
    char mem[INFO_LEN];
    char icache[INFO_LEN];
    char dcache[INFO_LEN];
    char scache[INFO_LEN];
    char tcache[INFO_LEN];
} SysInfo;

static void parse_hinv(SysInfo *si)
{
    FILE *fp;
    char  line[256];
    char *p;

    si->sys[0]    = '\0';
    si->cpu[0]    = '\0';
    si->mhz[0]    = '\0';
    si->fpu[0]    = '\0';
    si->mem[0]    = '\0';
    si->icache[0] = '\0';
    si->dcache[0] = '\0';
    si->scache[0] = '\0';
    si->tcache[0] = '\0';

    fp = popen("/sbin/hinv 2>/dev/null", "r");
    if (!fp) return;

    while (fgets(line, (int)sizeof(line), fp)) {
        p = strchr(line, '\n');
        if (p) *p = '\0';

        if (line[0] >= '1' && line[0] <= '9' && strstr(line, " MHZ ")) {
            char *mhz_tok = strstr(line, " MHZ ");
            if (mhz_tok) {
                char *sp1 = strchr(line, ' ');
                if (sp1 && sp1 < mhz_tok) {
                    int len = (int)(mhz_tok - (sp1 + 1));
                    if (len > 0 && len < 16) {
                        strncpy(si->mhz, sp1 + 1, len);
                        si->mhz[len] = '\0';
                    }
                }
                {
                    char *ip = mhz_tok + 5;
                    char *sp2 = strchr(ip, ' ');
                    if (strncmp(ip, "IP", 2) == 0) {
                        int len = sp2 ? (int)(sp2 - ip) : (int)strlen(ip);
                        if (len >= INFO_LEN) len = INFO_LEN - 1;
                        strncpy(si->sys, ip, len);
                        si->sys[len] = '\0';
                    }
                }
            }
        } else if (strncmp(line, "CPU:", 4) == 0) {
            const char *p = line + 4;
            while (*p == ' ') p++;
            while (*p && *p != ' ') p++;
            while (*p == ' ') p++;
            {
                const char *word = p;
                const char *sp = strchr(p, ' ');
                int len = sp ? (int)(sp - word) : (int)strlen(word);
                if (len >= INFO_LEN) len = INFO_LEN - 1;
                strncpy(si->cpu, word, len);
                si->cpu[len] = '\0';
            }
        } else if (strncmp(line, "FPU:", 4) == 0)
            strncpy(si->fpu, line, INFO_LEN - 1);
        else if (strstr(line, "Main memory size:"))
            strncpy(si->mem, line, INFO_LEN - 1);
        else if (strstr(line, "Instruction cache size:"))
            strncpy(si->icache, line, INFO_LEN - 1);
        else if (strstr(line, "Data cache size:"))
            strncpy(si->dcache, line, INFO_LEN - 1);
        else if (strstr(line, "Secondary unified") ||
                 strstr(line, "Secondary cache size:"))
            strncpy(si->scache, line, INFO_LEN - 1);
        else if (strstr(line, "Ternary unified") ||
                 strstr(line, "Ternary cache size:"))
            strncpy(si->tcache, line, INFO_LEN - 1);
    }
    pclose(fp);

    p = strstr(si->scache, " on Processor");
    if (p) *p = '\0';
    p = strstr(si->tcache, " on Processor");
    if (p) *p = '\0';

    si->sys[INFO_LEN-1]    = '\0';
    si->cpu[INFO_LEN-1]    = '\0';
    si->mhz[15]            = '\0';
    si->fpu[INFO_LEN-1]    = '\0';
    si->mem[INFO_LEN-1]    = '\0';
    si->icache[INFO_LEN-1] = '\0';
    si->dcache[INFO_LEN-1] = '\0';
    si->scache[INFO_LEN-1] = '\0';
    si->tcache[INFO_LEN-1] = '\0';
}

/* ------------------------------------------------------------------ */
/* Procedural 64x64 RGB565 checkerboard texture                        */
/* ------------------------------------------------------------------ */
#define TEX_DIM 64

static FxU16 tex_pixels[TEX_DIM * TEX_DIM];

static void make_texture(void)
{
    int x;
    int y;
    for (y = 0; y < TEX_DIM; y++) {
        for (x = 0; x < TEX_DIM; x++) {
            int check = ((x >> 3) ^ (y >> 3)) & 1;
            tex_pixels[y * TEX_DIM + x] = check ? 0xFFFF : 0x8410;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Minimal 3-D vector and rotation                                      */
/* ------------------------------------------------------------------ */
typedef struct { float x, y, z; } v3;

static v3 v3_xform(v3 v, float ax, float ay)
{
    float cy, sy, cx, sx;
    float rx, ry, rz;
    v3 r;

    cy = (float)cos(ay);  sy = (float)sin(ay);
    cx = (float)cos(ax);  sx = (float)sin(ax);

    rx =  v.x * cy + v.z * sy;
    ry =  v.y;
    rz = -v.x * sy + v.z * cy;

    r.x = rx;
    r.y = ry * cx - rz * sx;
    r.z = ry * sx + rz * cx;
    return r;
}

/* ------------------------------------------------------------------ */
/* Cube geometry                                                        */
/* ------------------------------------------------------------------ */

static const float cv[8][3] = {
    {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
    {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1}
};

static const int cf[6][4] = {
    {4,5,6,7},
    {1,0,3,2},
    {0,4,7,3},
    {5,1,2,6},
    {3,7,6,2},
    {4,0,1,5}
};

static const float quv[4][2] = {
    {0,0}, {TEX_DIM,0}, {TEX_DIM,TEX_DIM}, {0,TEX_DIM}
};

static const float tint[6][3] = {
    {1.0f, 0.2f, 0.2f},
    {0.2f, 1.0f, 0.2f},
    {0.2f, 0.2f, 1.0f},
    {1.0f, 1.0f, 0.2f},
    {0.2f, 1.0f, 1.0f},
    {1.0f, 0.2f, 1.0f}
};

#define CAM_Z   4.5f
#define SCALE   260.0f
#define CX      320.0f
#define CY      318.0f
#define ZNEAR   0.1f
#define TEXT_TOP 156

/* ------------------------------------------------------------------ */
/* Render one face — NO zero-area guard                                 */
/* ------------------------------------------------------------------ */
static void draw_face(int fi, float ax, float ay)
{
    GrVertex gv[4];
    float r;
    float g;
    float b;
    int vi;

    r = tint[fi][0] * 255.0f;
    g = tint[fi][1] * 255.0f;
    b = tint[fi][2] * 255.0f;

    for (vi = 0; vi < 4; vi++) {
        v3 p;
        float oow;

        p.x = cv[cf[fi][vi]][0];
        p.y = cv[cf[fi][vi]][1];
        p.z = cv[cf[fi][vi]][2];
        p   = v3_xform(p, ax, ay);
        p.z += CAM_Z;

        if (p.z < ZNEAR) return;

        oow = 1.0f / p.z;

        gv[vi].x   =  p.x * SCALE * oow + CX;
        gv[vi].y   = -p.y * SCALE * oow + CY;
        gv[vi].z   = 0.0f;
        gv[vi].oow = oow;
        gv[vi].r   = r;
        gv[vi].g   = g;
        gv[vi].b   = b;
        gv[vi].a   = 255.0f;
        gv[vi].tmuvtx[0].sow = quv[vi][0] * oow;
        gv[vi].tmuvtx[0].tow = quv[vi][1] * oow;
    }

    /* NO guard — degenerate triangles go straight to the hardware */
    grDrawTriangle(&gv[0], &gv[1], &gv[2]);
    grDrawTriangle(&gv[0], &gv[2], &gv[3]);
}

/* ------------------------------------------------------------------ */
/* Sort face indices farthest-first (painter's algorithm)              */
/* ------------------------------------------------------------------ */
static void sort_faces(int order[6], float ax, float ay)
{
    float cz[6];
    int i;
    int j;

    for (i = 0; i < 6; i++) {
        int vi;
        float sum = 0.0f;
        order[i] = i;
        for (vi = 0; vi < 4; vi++) {
            v3 p;
            p.x = cv[cf[i][vi]][0];
            p.y = cv[cf[i][vi]][1];
            p.z = cv[cf[i][vi]][2];
            p   = v3_xform(p, ax, ay);
            sum += p.z;
        }
        cz[i] = sum * 0.25f;
    }

    for (i = 1; i < 6; i++) {
        int   key = order[i];
        float kz  = cz[key];
        j = i - 1;
        while (j >= 0 && cz[order[j]] < kz) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
}

/* ------------------------------------------------------------------ */
/* Glide state                                                          */
/* ------------------------------------------------------------------ */

GrHwConfiguration hwconfig;
static char version[80];

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    GrTexInfo   texInfo;
    FxU32       texAddr;
    float       ax;
    float       ay;
    int         order[6];
    int         fi;
    int         frames;
    char        match;
    char      **remArgs;
    int         rv;
    SysInfo     si;

    frames = -1;
    while ((rv = tlGetOpt(argc, argv, "n", &match, &remArgs)) != 0) {
        if (rv == -1) break;
        if (match == 'n') frames = atoi(remArgs[0]);
    }

    parse_hinv(&si);

    tlSetScreen(640.0f, 480.0f);
    grGlideGetVersion(version);
    printf("bad_cube: cube WITHOUT zero-area guard\n");
    printf("%s\n", version);
    if (frames == -1)
        printf("Press any key to quit.\n");

    grGlideInit();
    if (!grSstQueryHardware(&hwconfig)) {
        fprintf(stderr, "bad_cube: no 3Dfx hardware detected\n");
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
        fprintf(stderr, "bad_cube: grSstWinOpen failed\n");
        grGlideShutdown();
        return 1;
    }

    tlConSet(0.0f, 0.0f, 1.0f, 1.0f, 80, 40, 0xffffff);

    if (si.sys[0])    tlConOutput("%s\n", si.sys);
    if (si.cpu[0]) {
        if (si.mhz[0])
            tlConOutput("CPU: %s/%sMHZ\n", si.cpu, si.mhz);
        else
            tlConOutput("CPU: %s\n", si.cpu);
    }
    if (si.fpu[0])    tlConOutput("%s\n", si.fpu);
    if (si.mem[0])    tlConOutput("%s\n", si.mem);
    if (si.icache[0]) tlConOutput("%s\n", si.icache);
    if (si.dcache[0]) tlConOutput("%s\n", si.dcache);
    if (si.scache[0]) tlConOutput("%s\n", si.scache);
    if (si.tcache[0]) tlConOutput("%s\n", si.tcache);

    {
        GrVoodooConfig_t *vc;
        int j;

        tlConOutput("\n");
        tlConOutput("NO ZERO-AREA GUARD -- may hang!\n");
        tlConOutput("3DFX boards: %d\n", hwconfig.num_sst);

        if (hwconfig.num_sst > 0 &&
            (hwconfig.SSTs[0].type == GR_SSTTYPE_VOODOO ||
             hwconfig.SSTs[0].type == GR_SSTTYPE_Voodoo2)) {
            vc = &hwconfig.SSTs[0].sstBoard.VoodooConfig;
            tlConOutput("FBI rev: %d  FBI RAM: %d MB\n",
                        vc->fbiRev, vc->fbRam);
            for (j = 0; j < vc->nTexelfx; j++) {
                tlConOutput("TMU %d: rev %d  RAM: %d MB\n",
                            j,
                            vc->tmuConfig[j].tmuRev,
                            vc->tmuConfig[j].tmuRam);
            }
        }
    }

    make_texture();

    texInfo.smallLod    = GR_LOD_64;
    texInfo.largeLod    = GR_LOD_64;
    texInfo.aspectRatio = GR_ASPECT_1x1;
    texInfo.format      = GR_TEXFMT_RGB_565;
    texInfo.data        = tex_pixels;

    texAddr = grTexMinAddress(GR_TMU0);
    grTexDownloadMipMap(GR_TMU0, texAddr, GR_MIPMAPLEVELMASK_BOTH, &texInfo);
    grTexSource(GR_TMU0, texAddr, GR_MIPMAPLEVELMASK_BOTH, &texInfo);

    grTexMipMapMode(GR_TMU0, GR_MIPMAP_DISABLE, FXFALSE);
    grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_BILINEAR, GR_TEXTUREFILTER_BILINEAR);
    grTexClampMode(GR_TMU0, GR_TEXTURECLAMP_WRAP, GR_TEXTURECLAMP_WRAP);
    grTexCombineFunction(GR_TMU0, GR_TEXTURECOMBINE_DECAL);

    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER,
                   GR_COMBINE_FACTOR_LOCAL,
                   GR_COMBINE_LOCAL_ITERATED,
                   GR_COMBINE_OTHER_TEXTURE,
                   FXFALSE);

    grDepthBufferMode(GR_DEPTHBUFFER_DISABLE);
    grCullMode(GR_CULL_DISABLE);

    {
        int page;
        for (page = 0; page < 2; page++) {
            grClipWindow(0, 0, 640, 480);
            grBufferClear(0x002040, 0, 0);
            tlConRender();
            grBufferSwap(1);
        }
    }

    grTexSource(GR_TMU0, texAddr, GR_MIPMAPLEVELMASK_BOTH, &texInfo);
    grTexCombineFunction(GR_TMU0, GR_TEXTURECOMBINE_DECAL);
    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER,
                   GR_COMBINE_FACTOR_LOCAL,
                   GR_COMBINE_LOCAL_ITERATED,
                   GR_COMBINE_OTHER_TEXTURE,
                   FXFALSE);
    grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO,
                         GR_BLEND_ONE, GR_BLEND_ZERO);
    grDepthBufferMode(GR_DEPTHBUFFER_DISABLE);
    grCullMode(GR_CULL_DISABLE);

    ax = 0.0f;
    ay = 0.0f;

    while (frames-- && tlOkToRender()) {

        ax += 0.013f;
        ay += 0.021f;

        grClipWindow(0, TEXT_TOP, 640, 480);
        grBufferClear(0x002040, 0, 0);

        sort_faces(order, ax, ay);
        for (fi = 0; fi < 6; fi++)
            draw_face(order[fi], ax, ay);

        grBufferSwap(1);

        while (tlKbHit()) {
            switch (tlGetCH()) {
            default:
                frames = 0;
                break;
            }
        }
    }

    grGlideShutdown();
    return 0;
}

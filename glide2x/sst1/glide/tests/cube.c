/*
** cube.c - Textured spinning cube demo for Glide 2.x (IRIX / Voodoo 1)
**
** Renders a cube with a 64x64 procedural checkerboard texture.
** Each face has a distinct tint colour so the six faces are easily
** distinguished.  Uses the painter's algorithm (depth-sorted faces,
** no depth buffer) since a convex shape like a cube is handled
** correctly without hardware depth testing.
**
** Overlays system info (parsed from /sbin/hinv) and Voodoo hardware
** details in the top-left corner using the tlib console renderer.
**
** Controls: any key quits.
** Usage:    cube [-n <frames>]
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
    char cpu[INFO_LEN];   /* chip model only, e.g. "RM7000" */
    char mhz[16];         /* speed digits, e.g. "800" */
    char fpu[INFO_LEN];
    char mem[INFO_LEN];
    char icache[INFO_LEN];
    char dcache[INFO_LEN];
    char scache[INFO_LEN];
    char tcache[INFO_LEN];
} SysInfo;

/*
 * Run /sbin/hinv and parse the lines we care about.
 * The " on Processor N" suffix is stripped from cache lines so they
 * fit within the 60-column console grid.
 */
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

        /*
         * "1 800 MHZ IP32 Processor"
         * Extract IP type ("IP32") into sys and speed ("800") into mhz.
         * This line starts with a digit (processor count), not "IP".
         */
        if (line[0] >= '1' && line[0] <= '9' && strstr(line, " MHZ ")) {
            char *mhz_tok = strstr(line, " MHZ ");
            if (mhz_tok) {
                /* speed sits between first space and " MHZ" */
                char *sp1 = strchr(line, ' ');
                if (sp1 && sp1 < mhz_tok) {
                    int len = (int)(mhz_tok - (sp1 + 1));
                    if (len > 0 && len < 16) {
                        strncpy(si->mhz, sp1 + 1, len);
                        si->mhz[len] = '\0';
                    }
                }
                /* IP type is the token right after "MHZ " */
                {
                    char *ip = mhz_tok + 5; /* skip " MHZ " */
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
            /* Extract chip model: second word after "CPU: " (skip vendor) */
            const char *p = line + 4;
            while (*p == ' ') p++;
            while (*p && *p != ' ') p++;  /* skip vendor */
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

    /* Strip " on Processor N" so lines fit in 80 cols */
    p = strstr(si->scache, " on Processor");
    if (p) *p = '\0';
    p = strstr(si->tcache, " on Processor");
    if (p) *p = '\0';

    /* Guarantee null termination after strncpy */
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
            /* White squares and mid-grey squares */
            tex_pixels[y * TEX_DIM + x] = check ? 0xFFFF : 0x8410;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Minimal 3-D vector and rotation                                      */
/* ------------------------------------------------------------------ */
typedef struct { float x, y, z; } v3;

/*
 * Rotate v first around Y by ay, then around X by ax.
 */
static v3 v3_xform(v3 v, float ax, float ay)
{
    float cy, sy, cx, sx;
    float rx, ry, rz;
    v3 r;

    cy = (float)cos(ay);  sy = (float)sin(ay);
    cx = (float)cos(ax);  sx = (float)sin(ax);

    /* Y rotation */
    rx =  v.x * cy + v.z * sy;
    ry =  v.y;
    rz = -v.x * sy + v.z * cy;

    /* X rotation */
    r.x = rx;
    r.y = ry * cx - rz * sx;
    r.z = ry * sx + rz * cx;
    return r;
}

/* ------------------------------------------------------------------ */
/* Cube geometry                                                        */
/* ------------------------------------------------------------------ */

/* 8 unit-cube vertices */
static const float cv[8][3] = {
    {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
    {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1}
};

/* 6 faces as CCW quads (vertex indices) */
static const int cf[6][4] = {
    {4,5,6,7},  /* +Z  front  */
    {1,0,3,2},  /* -Z  back   */
    {0,4,7,3},  /* -X  left   */
    {5,1,2,6},  /* +X  right  */
    {3,7,6,2},  /* +Y  top    */
    {4,0,1,5}   /* -Y  bottom */
};

/* Texture coordinates for each quad corner (in texels, 0..TEX_DIM) */
static const float quv[4][2] = {
    {0,0}, {TEX_DIM,0}, {TEX_DIM,TEX_DIM}, {0,TEX_DIM}
};

/* Per-face tint colours (multiplied with texture in colour combiner) */
static const float tint[6][3] = {
    {1.0f, 0.2f, 0.2f},  /* +Z  red      */
    {0.2f, 1.0f, 0.2f},  /* -Z  green    */
    {0.2f, 0.2f, 1.0f},  /* -X  blue     */
    {1.0f, 1.0f, 0.2f},  /* +X  yellow   */
    {0.2f, 1.0f, 1.0f},  /* +Y  cyan     */
    {1.0f, 0.2f, 1.0f}   /* -Y  magenta  */
};

/* Projection constants */
#define CAM_Z   4.5f    /* camera distance along +Z from cube centre  */
#define SCALE   260.0f  /* perspective scale factor (pixels/unit at z=1) */
#define CX      320.0f  /* screen centre X */
#define CY      318.0f  /* screen centre Y (shifted down; text occupies top 156px) */
#define ZNEAR   0.1f    /* near clip distance */
#define TEXT_TOP 156    /* first row below the text overlay (13 rows x 12px) */

/* ------------------------------------------------------------------ */
/* Render one face                                                      */
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
        p.z += CAM_Z;               /* translate to camera space */

        if (p.z < ZNEAR) return;    /* whole face treated as clipped */

        oow = 1.0f / p.z;

        gv[vi].x   =  p.x * SCALE * oow + CX;
        gv[vi].y   = -p.y * SCALE * oow + CY;  /* screen Y is flipped */
        gv[vi].z   = 0.0f;
        gv[vi].oow = oow;
        gv[vi].r   = r;
        gv[vi].g   = g;
        gv[vi].b   = b;
        gv[vi].a   = 255.0f;
        gv[vi].tmuvtx[0].sow = quv[vi][0] * oow;
        gv[vi].tmuvtx[0].tow = quv[vi][1] * oow;
    }

    /* Guard: skip edge-on faces (zero/near-zero screen-space area).
     * Submitting zero-area triangles to the Voodoo1 corrupts internal
     * hardware state and causes a GPU pipeline hang. */
    {
        float area1 = 0.5f * ((gv[1].x - gv[0].x) * (gv[2].y - gv[0].y) -
                               (gv[2].x - gv[0].x) * (gv[1].y - gv[0].y));
        float area2 = 0.5f * ((gv[2].x - gv[0].x) * (gv[3].y - gv[0].y) -
                               (gv[3].x - gv[0].x) * (gv[2].y - gv[0].y));
        if (area1 > -1.0f && area1 < 1.0f) return;
        if (area2 > -1.0f && area2 < 1.0f) return;
    }

    /* Quad as two triangles */
    grDrawTriangle(&gv[0], &gv[1], &gv[2]);
    grDrawTriangle(&gv[0], &gv[2], &gv[3]);
}

/* ------------------------------------------------------------------ */
/* Sort face indices farthest-first (painter's algorithm)              */
/* Faces with the highest average Z in rotated space are drawn first.  */
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
        cz[i] = sum * 0.25f;  /* average Z before camera translation */
    }

    /* Insertion sort: highest cz first (farthest from camera) */
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

    /* --- argument parsing --- */
    frames = -1;
    while ((rv = tlGetOpt(argc, argv, "n", &match, &remArgs)) != 0) {
        if (rv == -1) break;
        if (match == 'n') frames = atoi(remArgs[0]);
    }

    /* --- Collect system info before Glide takes over --- */
    parse_hinv(&si);

    tlSetScreen(640.0f, 480.0f);
    grGlideGetVersion(version);
    printf("cube: textured spinning cube\n");
    printf("%s\n", version);
    if (frames == -1)
        printf("Press any key to quit.\n");

    /* --- Glide init --- */
    grGlideInit();
    if (!grSstQueryHardware(&hwconfig)) {
        fprintf(stderr, "cube: no 3Dfx hardware detected\n");
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
        fprintf(stderr, "cube: grSstWinOpen failed\n");
        grGlideShutdown();
        return 1;
    }

    /*
     * Set up the text overlay console.
     * tlConSet reserves space at the TOP of TMU0 texture RAM for the
     * font; the checkerboard texture is uploaded at the BOTTOM
     * (grTexMinAddress), so there is no collision.
     * tlConRender saves and restores full Glide state each frame, so
     * the cube render state is not disturbed.
     *
     * Note: tlConOutput uppercases all text automatically.
     */
    /* 80 cols x 40 rows = 8x12px per char (~75% of original 10.7x16px) */
    tlConSet(0.0f, 0.0f, 1.0f, 1.0f, 80, 40, 0xffffff);

    /* System lines from hinv */
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

    /* 3Dfx board info from grSstQueryHardware */
    {
        GrVoodooConfig_t *vc;
        int j;

        tlConOutput("\n");
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

    /* --- Texture upload --- */
    make_texture();

    texInfo.smallLod    = GR_LOD_64;
    texInfo.largeLod    = GR_LOD_64;
    texInfo.aspectRatio = GR_ASPECT_1x1;
    texInfo.format      = GR_TEXFMT_RGB_565;
    texInfo.data        = tex_pixels;

    texAddr = grTexMinAddress(GR_TMU0);
    grTexDownloadMipMap(GR_TMU0, texAddr, GR_MIPMAPLEVELMASK_BOTH, &texInfo);
    grTexSource(GR_TMU0, texAddr, GR_MIPMAPLEVELMASK_BOTH, &texInfo);

    /* --- Render state --- */
    grTexMipMapMode(GR_TMU0, GR_MIPMAP_DISABLE, FXFALSE);
    grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_BILINEAR, GR_TEXTUREFILTER_BILINEAR);
    grTexClampMode(GR_TMU0, GR_TEXTURECLAMP_WRAP, GR_TEXTURECLAMP_WRAP);
    grTexCombineFunction(GR_TMU0, GR_TEXTURECOMBINE_DECAL);

    /*
     * Colour combine: output = texture * iterated_vertex_colour
     * This multiplies the checkerboard texture by the per-face tint
     * colour set in the GrVertex r/g/b fields, giving each face a
     * distinct colour while retaining the texture pattern.
     */
    grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER,
                   GR_COMBINE_FACTOR_LOCAL,
                   GR_COMBINE_LOCAL_ITERATED,
                   GR_COMBINE_OTHER_TEXTURE,
                   FXFALSE);

    grDepthBufferMode(GR_DEPTHBUFFER_DISABLE);
    grCullMode(GR_CULL_DISABLE);

    /*
     * Pre-render the text overlay to BOTH double-buffer pages.
     * tlConRender downloads the font texture and draws the console text.
     * We call it once per page so the text is visible on every frame.
     * After this, tlConRender is NEVER called in the main loop; instead,
     * grClipWindow restricts grBufferClear (and all drawing) to the area
     * below TEXT_TOP so the pre-rendered text rows are never overwritten.
     */
    {
        int page;
        for (page = 0; page < 2; page++) {
            grClipWindow(0, 0, 640, 480);
            grBufferClear(0x002040, 0, 0);
            tlConRender();
            grBufferSwap(1);
        }
    }

    /* Re-establish cube render state after tlConRender altered it */
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

    /* --- Main loop --- */
    ax = 0.0f;
    ay = 0.0f;

    while (frames-- && tlOkToRender()) {

        ax += 0.013f;
        ay += 0.021f;

        /*
         * Restrict clear and drawing to the non-text area (y >= TEXT_TOP).
         * The text rows pre-rendered above are never touched again.
         */
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

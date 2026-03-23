/*
 * fxirix.c - IRIX platform I/O layer for 3Dfx Glide 2.x
 *
 * Replaces swlibs/newpci/pcilib/fxlinux.c for IRIX builds.
 * Implements the FxPlatformIOProcs vtable using /dev/tdfx.
 *
 * Install: copy to swlibs/newpci/pcilib/fxirix.c
 *
 * The key function is pciMapLinearIrix() — sst1init calls it with the
 * physical BAR0 address and expects a virtual address back.  That VA
 * becomes vgBaseAddr, from which Glide derives all register, LFB, and
 * texture pointers via SST_LFB_ADDRESS() / SST_TEX_ADDRESS() macros.
 *
 * Endianness:
 *   O2/IP32: MACE PCI bridge auto-swaps 32-bit MMIO, so register
 *   accesses are correct without software swapping.  The LFB byte-swap
 *   bit (fbiInit1 bit 9) is set here after mapping the register window,
 *   before sst1init's own init sequence runs.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <3dfx.h>
#include "fxpci.h"
#include "pcilib.h"
#include "tdfx_irix.h"

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */
static const char *pciIdentifyIrix(void);
static FxBool pciOutputStringIrix(const char *msg);
static FxBool pciInitializeIrix(void);
static FxBool pciShutdownIrix(void);
static FxBool pciMapLinearIrix(FxU32 bus, FxU32 physical_addr,
                               unsigned long *linear_addr, FxU32 *length);
static FxBool pciUnmapLinearIrix(unsigned long linear_addr, FxU32 length);
static FxBool pciSetPermissionIrix(const unsigned long, const FxU32,
                                   const FxBool);
static FxU8   pciPortInByteIrix(unsigned short port);
static FxU16  pciPortInWordIrix(unsigned short port);
static FxU32  pciPortInLongIrix(unsigned short port);
static FxBool pciPortOutByteIrix(unsigned short port, FxU8 data);
static FxBool pciPortOutWordIrix(unsigned short port, FxU16 data);
static FxBool pciPortOutLongIrix(unsigned short port, FxU32 data);
static FxBool pciMsrGetIrix(MSRInfo *, MSRInfo *);
static FxBool pciMsrSetIrix(MSRInfo *, MSRInfo *);
static FxBool pciSetPassThroughBaseIrix(FxU32 *, FxU32);

/* ------------------------------------------------------------------ */
/* Platform I/O vtable — mirrors ioProcsLinux in fxlinux.c             */
/* ------------------------------------------------------------------ */
const FxPlatformIOProcs ioProcsIrix = {
    pciInitializeIrix,
    pciShutdownIrix,
    pciIdentifyIrix,
    pciPortInByteIrix,
    pciPortInWordIrix,
    pciPortInLongIrix,
    pciPortOutByteIrix,
    pciPortOutWordIrix,
    pciPortOutLongIrix,
    pciMapLinearIrix,
    pciUnmapLinearIrix,
    pciSetPermissionIrix,
    pciMsrGetIrix,
    pciMsrSetIrix,
    pciOutputStringIrix,
    pciSetPassThroughBaseIrix
};

/* ------------------------------------------------------------------ */
/* Module state                                                         */
/* ------------------------------------------------------------------ */
#define IRIX_MAX_CARDS  2

static int irixDevFd[IRIX_MAX_CARDS] = { -1, -1 };
static int irixNumCards = 0;

/* Mapping table for pciUnmapLinearIrix */
typedef struct { unsigned long addr; FxU32 len; } IrixMapping;
#define IRIX_MAX_MAPPINGS 8
static IrixMapping irixMappings[IRIX_MAX_MAPPINGS];
static int         irixNumMappings = 0;

/* ------------------------------------------------------------------ */
/* pciPlatformInit — installs this vtable as the active platform.       */
/* Called from pcilib before any other pci* function.                   */
/* ------------------------------------------------------------------ */
FxBool
pciPlatformInit(void)
{
    gCurPlatformIO = &ioProcsIrix;
    return FXTRUE;
}

FxBool
hasDev3DfxIrix(void)
{
    return (irixDevFd[0] != -1) ? FXTRUE : FXFALSE;
}

/* ------------------------------------------------------------------ */
/* pciInitializeIrix                                                    */
/* ------------------------------------------------------------------ */
static FxBool
pciInitializeIrix(void)
{
    int i, num = 0;

    if (getenv("SST_NO_DEV3DFX")) {
        pciErrorCode = PCI_ERR_NO_IO_PERM;
        return FXFALSE;
    }

    for (i = 0; i < IRIX_MAX_CARDS; i++) {
        const char *node = (i == 0) ? TDFX_DEVICE_NODE : TDFX_DEVICE_NODE_1;
        irixDevFd[i] = open(node, O_RDWR);
        if (irixDevFd[i] < 0) { irixDevFd[i] = -1; break; }
        num++;
    }

    irixNumCards = num;
    if (irixNumCards == 0) {
        fprintf(stderr, "fxirix: cannot open %s: %s\n",
                TDFX_DEVICE_NODE, strerror(errno));
        pciErrorCode = PCI_ERR_NO_IO_PERM;
        return FXFALSE;
    }

#ifdef GLIDE_IRIX_DBG_INIT
    fprintf(stderr, "fxirix: %d 3Dfx card(s) opened\n", irixNumCards);
#endif
    return FXTRUE;
}

/* ------------------------------------------------------------------ */
/* pciShutdownIrix                                                      */
/* ------------------------------------------------------------------ */
static FxBool
pciShutdownIrix(void)
{
    int i;
    for (i = 0; i < IRIX_MAX_CARDS; i++) {
        if (irixDevFd[i] >= 0) { close(irixDevFd[i]); irixDevFd[i] = -1; }
    }
    irixNumCards = irixNumMappings = 0;
    return FXTRUE;
}

/* ------------------------------------------------------------------ */
/* pciMapLinearIrix                                                     */
/*                                                                      */
/* sst1init calls this with the physical BAR0 address it found via PCI  */
/* config space.  We match it against the addresses our kernel driver   */
/* knows about, then mmap through /dev/tdfx at the right offset.        */
/*                                                                      */
/* Offset layout in /dev/tdfx (from tdfx_irix.h):                      */
/*   TDFX_MMAP_REG_OFFSET (0)        -> register window                */
/*   reg_size                         -> framebuffer window             */
/* ------------------------------------------------------------------ */
static FxBool
pciMapLinearIrix(FxU32 bus, FxU32 physical_addr,
                 unsigned long *linear_addr, FxU32 *length)
{
    int i;
    unsigned long reg_base, fb_base, reg_size, fb_size;
    off_t  mmap_offset = (off_t)TDFX_MMAP_REG_OFFSET;
    int    fd = -1;
    void  *ptr;

    /* Match physical_addr to a known card BAR using simple per-fd ioctls */
    for (i = 0; i < irixNumCards; i++) {
        if (irixDevFd[i] < 0) continue;
        reg_base = fb_base = reg_size = fb_size = 0;
        ioctl(irixDevFd[i], TDFX_GET_REGBASE, &reg_base);
        ioctl(irixDevFd[i], TDFX_GET_FBBASE,  &fb_base);
        ioctl(irixDevFd[i], TDFX_GET_REGSIZE, &reg_size);
        ioctl(irixDevFd[i], TDFX_GET_FBSIZE,  &fb_size);
#ifdef GLIDE_IRIX_DBG_INIT
        fprintf(stderr,
            "fxirix: card %d reg_base=0x%lx fb_base=0x%lx"
            " reg_size=%lu fb_size=%lu\n",
            i, reg_base, fb_base, reg_size, fb_size);
#endif

        if (physical_addr != 0 && physical_addr == (FxU32)reg_base) {
            fd = irixDevFd[i];
            mmap_offset = (off_t)TDFX_MMAP_REG_OFFSET;
            if (*length == 0) *length = (FxU32)reg_size;
#ifdef GLIDE_IRIX_DBG_INIT
            fprintf(stderr,
                "fxirix: map card %d reg window phys=0x%08x size=%u\n",
                i, physical_addr, *length);
#endif
            break;
        }
        if (physical_addr != 0 && physical_addr == (FxU32)fb_base) {
            fd = irixDevFd[i];
            mmap_offset = (off_t)reg_size; /* fb immediately after regs */
            if (*length == 0) *length = (FxU32)fb_size;
#ifdef GLIDE_IRIX_DBG_INIT
            fprintf(stderr,
                "fxirix: map card %d fb window phys=0x%08x size=%u\n",
                i, physical_addr, *length);
#endif
            break;
        }
    }

    /* Fallback: use card 0 register window */
    if (fd < 0) {
        unsigned long sz = 0;
#ifdef GLIDE_IRIX_DBG_INIT
        fprintf(stderr,
            "fxirix: phys=0x%08x unmatched, defaulting to card 0 regs\n",
            physical_addr);
#endif
        fd = irixDevFd[0];
        mmap_offset = (off_t)TDFX_MMAP_REG_OFFSET;
        if (*length == 0) {
            if (ioctl(fd, TDFX_GET_REGSIZE, &sz) < 0)
                fprintf(stderr, "fxirix: TDFX_GET_REGSIZE failed: %s\n",
                        strerror(errno));
            *length = (FxU32)sz;
        }
    }

#ifdef GLIDE_IRIX_DBG_INIT
    fprintf(stderr, "fxirix: mmap fd=%d offset=0x%lx length=%u\n",
            fd, (unsigned long)mmap_offset, *length);
#endif
    ptr = mmap(NULL, (size_t)*length,
               PROT_READ | PROT_WRITE, MAP_SHARED, fd, mmap_offset);
    if (ptr == MAP_FAILED) {
        fprintf(stderr, "fxirix: mmap failed errno=%d: %s\n",
                errno, errno ? strerror(errno) : "(errno=0?)");
        pciErrorCode = PCI_ERR_NO_MEM_PERM;
        return FXFALSE;
    }

    *linear_addr = (unsigned long)ptr;

    /* Track for unmap */
    if (irixNumMappings < IRIX_MAX_MAPPINGS) {
        irixMappings[irixNumMappings].addr = *linear_addr;
        irixMappings[irixNumMappings].len  = *length;
        irixNumMappings++;
    }

    /*
     * No explicit byte-swap setup needed here.
     * On IP32/O2, the MACE PCI bridge automatically byte-swaps 32-bit MMIO
     * accesses, so register reads/writes are correct without any software
     * intervention.  fbiInit1 bit 9 is SST_HVSYNC_OVERRIDE, not a byte-swap
     * control — there is no software LFB byte-swap bit on the SST-1.
     * CPU-to-LFB writes via grLfbWriteRegion must handle endianness in the
     * Glide application layer if needed.
     */
    return FXTRUE;
}

/* ------------------------------------------------------------------ */
/* pciUnmapLinearIrix                                                   */
/* ------------------------------------------------------------------ */
static FxBool
pciUnmapLinearIrix(unsigned long linear_addr, FxU32 length)
{
    munmap((void *)linear_addr, (size_t)length);
    return FXTRUE;
}

/* ------------------------------------------------------------------ */
/* pciFetchRegisterIrix / pciUpdateRegisterIrix                         */
/* Called by newpci/sst1init to read PCI config space BARs.             */
/* We return the addresses our kernel driver already knows.             */
/* ------------------------------------------------------------------ */
FxU32
pciFetchRegisterIrix(FxU32 cmd, FxU32 size, FxU32 device)
{
    unsigned long   val = 0;
    tdfx_pci_cfg_t  cfg;

    if (device >= (FxU32)irixNumCards || irixDevFd[device] < 0) return 0;

    /* Vendor/Device ID: return compile-time constants */
    if (cmd == 0x00) return 0x121A;  /* PCI Vendor ID: 3Dfx */
    if (cmd == 0x02) return 0x0001;  /* PCI Device ID: Voodoo1/SST1 */

    /* BAR0/BAR1: use dedicated ioctls (already proven to work) */
    if (cmd == 0x10) {
        if (ioctl(irixDevFd[device], TDFX_GET_REGBASE, &val) < 0) return 0;
#ifdef GLIDE_IRIX_DBG_INIT
        fprintf(stderr, "fxirix: device %u TDFX_GET_REGBASE=0x%lx\n",
                (unsigned)device, val);
#endif
        return (FxU32)val;
    }
    if (cmd == 0x14) {
        if (ioctl(irixDevFd[device], TDFX_GET_FBBASE, &val) < 0) return 0;
        return (FxU32)val;
    }

    /* General PCI config space read via TDFX_PCI_CFG_RD */
    cfg.offset = cmd;
    cfg.size   = size ? size : 4;
    cfg.value  = 0;
    if (ioctl(irixDevFd[device], TDFX_PCI_CFG_RD, &cfg) < 0) {
        fprintf(stderr, "fxirix: TDFX_PCI_CFG_RD offset=0x%x failed: %s\n",
                cmd, strerror(errno));
        return 0;
    }
    return cfg.value;
}

FxBool
pciUpdateRegisterIrix(FxU32 cmd, FxU32 data, FxU32 size, FxU32 device)
{
    tdfx_pci_cfg_t cfg;

    if (device >= (FxU32)irixNumCards || irixDevFd[device] < 0) return FXTRUE;

    cfg.offset = cmd;
    cfg.size   = size ? size : 4;
    cfg.value  = data;
    if (ioctl(irixDevFd[device], TDFX_PCI_CFG_WR, &cfg) < 0) {
        fprintf(stderr, "fxirix: TDFX_PCI_CFG_WR offset=0x%x failed: %s\n",
                cmd, strerror(errno));
    }
    return FXTRUE;
}

int
getNumDevicesIrix(void)
{
    return irixNumCards;
}

/*
 * sst1InitGetenv - used by sst1init code to read environment variables.
 * On IRIX we have no voodoo.ini, so just wrap getenv().
 */
char *
sst1InitGetenv(char *string)
{
    return getenv(string);
}

/* ------------------------------------------------------------------ */
/* I/O port stubs — no I/O ports on MIPS                               */
/* ------------------------------------------------------------------ */
static FxU8   pciPortInByteIrix(unsigned short p)             { return 0; }
static FxU16  pciPortInWordIrix(unsigned short p)             { return 0; }
static FxU32  pciPortInLongIrix(unsigned short p)             { return 0; }
static FxBool pciPortOutByteIrix(unsigned short p, FxU8 d)    { return FXTRUE; }
static FxBool pciPortOutWordIrix(unsigned short p, FxU16 d)   { return FXTRUE; }
static FxBool pciPortOutLongIrix(unsigned short p, FxU32 d)   { return FXTRUE; }

/* ------------------------------------------------------------------ */
/* Remaining vtable stubs                                               */
/* ------------------------------------------------------------------ */
static const char *pciIdentifyIrix(void)
    { return "fxPCI for IRIX (tdfx)"; }

static FxBool pciOutputStringIrix(const char *msg)
    { printf("%s", msg); return FXTRUE; }

static FxBool pciSetPermissionIrix(const unsigned long a, const FxU32 l,
                                   const FxBool w)
    { return FXTRUE; }

static FxBool pciMsrGetIrix(MSRInfo *in, MSRInfo *out) { return FXTRUE; }
static FxBool pciMsrSetIrix(MSRInfo *in, MSRInfo *out) { return FXTRUE; }

static FxBool pciSetPassThroughBaseIrix(FxU32 *b, FxU32 l) { return FXTRUE; }

/* ------------------------------------------------------------------ */
/* MTRR stubs — x86 write-combining, no equivalent on MIPS/IRIX       */
/* sst1InitCachingOn() calls these; they must link but do nothing.     */
/* ------------------------------------------------------------------ */
FxBool pciFindMTRRMatch(FxU32 b, FxU32 s, int t, FxU32 *n)
    { (void)b; (void)s; (void)t; (void)n; return FXFALSE; }
FxBool pciFindFreeMTRR(FxU32 *n)
    { (void)n; return FXFALSE; }
FxBool pciSetMTRR(FxU32 no, FxU32 b, FxU32 s, int t)
    { (void)no; (void)b; (void)s; (void)t; return FXFALSE; }
FxBool pciSetMTRRAmdK6(FxU32 no, FxU32 b, FxU32 s, int t)
    { (void)no; (void)b; (void)s; (void)t; return FXFALSE; }

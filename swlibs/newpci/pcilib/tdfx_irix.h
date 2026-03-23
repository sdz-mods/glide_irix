/*
 * tdfx_irix.h - Shared definitions between the IRIX 3Dfx kernel driver
 *               (tdfx_irix.c) and the Glide2x userspace OS layer
 *               (sst1_irix.c / cvg_irix.c).
 *
 * Covers both Voodoo1 (SST-1, device 0x0001) and Voodoo2 (CVG, device 0x0002).
 *
 * Glide's OS layer should:
 *   1. #include "tdfx_irix.h"
 *   2. Call tdfx_map_card() to get mapped register and FB pointers.
 *   3. Query TDFX_GET_BOARDINFO to retrieve device_id and physical bases.
 *   4. Branch on device_id == SST1_DEVICE_ID vs CVG_DEVICE_ID to select
 *      the appropriate sst1init / cvginit hardware path.
 *   5. Call tdfx_unmap_card() on GrGlideShutdown().
 */

#ifndef _TDFX_IRIX_H
#define _TDFX_IRIX_H

#include <sys/types.h>
#include <sys/ioctl.h>
#ifndef _KERNEL
#  include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* PCI identities                                                       */
/* ------------------------------------------------------------------ */
#define TDFX_PCI_VENDOR     0x121a
#define SST1_DEVICE_ID      0x0001      /* Voodoo1 */
#define CVG_DEVICE_ID       0x0002      /* Voodoo2 */

/* ------------------------------------------------------------------ */
/* Device nodes                                                         */
/* ------------------------------------------------------------------ */
#define TDFX_DEVICE_NODE        "/hw/tdfx0"     /* hwgraph: first card  */
#define TDFX_DEVICE_NODE_1      "/hw/tdfx1"     /* hwgraph: second card */

/* ------------------------------------------------------------------ */
/* mmap offset layout                                                   */
/*                                                                      */
/* Glide uses two separate mmap() calls with these byte offsets.        */
/* The boundary between registers and framebuffer equals reg_size,      */
/* which differs by card type — use TDFX_GET_REGSIZE to query it.      */
/*                                                                      */
/* Voodoo1 (SST-1):  16MB BAR0 layout:                                 */
/*   reg=4MB at 0x000000, lfb=4MB at 0x400000, tex=8MB at 0x800000    */
/* Voodoo2 (CVG):    reg=4MB BAR0 at 0x000000, fb=4MB BAR1 separate   */
/* ------------------------------------------------------------------ */
#define TDFX_MMAP_REG_OFFSET    0x00000000UL   /* always starts at 0      */
#define SST1_MMAP_FB_OFFSET     0x00400000UL   /* Voodoo1 lfb at 4MB mark */
#define CVG_MMAP_FB_OFFSET      0x00400000UL   /* Voodoo2 fb at 4MB mark  */
#define SST1_REGION_SIZE        0x01000000UL   /* 16 MB total on Voodoo1  */
#define CVG_REGION_SIZE         0x00400000UL   /* 4 MB each on Voodoo2    */

/* ------------------------------------------------------------------ */
/* ioctl interface                                                       */
/*                                                                       */
/* Defined as plain integers to match the kernel driver (which cannot   */
/* use _IOR/_IOWR in the kernel compilation environment on IRIX).       */
/* Encoding: 0x3300 + sequence  ('3' << 8 == 0x3300)                   */
/* ------------------------------------------------------------------ */
#define TDFX_GET_REGBASE        0x3300
#define TDFX_GET_FBBASE         0x3301
#define TDFX_GET_REGSIZE        0x3302
#define TDFX_GET_FBSIZE         0x3303
#define TDFX_GET_NUMCARDS       0x3304
#define TDFX_GET_BOARDINFO      0x3305
#define TDFX_PCI_CFG_RD         0x3306  /* read  PCI config space register  */
#define TDFX_PCI_CFG_WR         0x3307  /* write PCI config space register  */

/*
 * TDFX_PCI_CFG_RD / TDFX_PCI_CFG_WR:
 *   Read or write a single PCI configuration space register.
 *   For RD: caller fills offset+size; driver fills value.
 *   For WR: caller fills offset+size+value; driver writes it.
 *   size must be 1, 2, or 4 bytes.
 */
typedef struct {
    uint32_t    offset;     /* IN:  PCI config space byte offset (e.g. 0x40) */
    uint32_t    size;       /* IN:  access width in bytes: 1, 2, or 4        */
    uint32_t    value;      /* I/O: data to write (WR) or value read (RD)    */
} tdfx_pci_cfg_t;

/*
 * TDFX_GET_BOARDINFO: caller fills board_index; driver fills rest.
 * device_id lets Glide distinguish Voodoo1 from Voodoo2 at runtime.
 */
typedef struct {
    int             board_index;    /* IN  */
    uint16_t        device_id;      /* OUT: SST1_DEVICE_ID or CVG_DEVICE_ID */
    unsigned long   reg_base;       /* OUT: physical addr, BAR0 */
    unsigned long   fb_base;        /* OUT: physical addr, BAR1 */
    unsigned long   reg_size;       /* OUT */
    unsigned long   fb_size;        /* OUT */
} tdfx_board_info_t;

/* ------------------------------------------------------------------ */
/* Userspace helpers (not compiled into the kernel)                     */
/* ------------------------------------------------------------------ */
#ifndef _KERNEL
#include <sys/mman.h>
#include <fcntl.h>
#include <stdio.h>

/*
 * tdfx_map_card() - open /dev/tdfx (or /dev/tdfx1) and mmap both
 * hardware regions into the calling process's address space.
 *
 * On success returns 0 and sets *fd_out, *reg_out, *fb_out.
 * The caller must call tdfx_unmap_card() when done.
 *
 * In Glide's sst1_irix.c this replaces the Linux open("/dev/3dfx") +
 * mmap() sequence.
 */
static int
tdfx_map_card(int card_index, int *fd_out,
              void **reg_out, void **fb_out)
{
    const char *devnode = (card_index == 0)
                          ? TDFX_DEVICE_NODE
                          : TDFX_DEVICE_NODE_1;
    int     fd;
    void   *reg_ptr, *fb_ptr;
    unsigned long reg_size;

    fd = open(devnode, O_RDWR);
    if (fd < 0) {
        perror("tdfx_map_card: open");
        return -1;
    }

    /* Query the register region size — differs between SST-1 and CVG */
    if (ioctl(fd, TDFX_GET_REGSIZE, &reg_size) < 0) {
        perror("tdfx_map_card: TDFX_GET_REGSIZE");
        close(fd);
        return -1;
    }

    reg_ptr = mmap(NULL, reg_size,
                   PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, TDFX_MMAP_REG_OFFSET);
    if (reg_ptr == MAP_FAILED) {
        perror("tdfx_map_card: mmap registers");
        close(fd);
        return -1;
    }

    /*
     * Framebuffer starts immediately after the register region.
     * Offset = reg_size (4MB for SST-1, 4MB for CVG).
     * For SST-1, this maps the 4MB LFB region (0x400000-0x7FFFFF).
     * The texture area (0x800000-0xFFFFFF) is not separately mapped here;
     * use a single 16MB mmap at offset 0 if full access is needed.
     */
    fb_ptr = mmap(NULL, reg_size,   /* 4MB LFB for both card types */
                  PROT_READ | PROT_WRITE, MAP_SHARED,
                  fd, (off_t)reg_size);
    if (fb_ptr == MAP_FAILED) {
        perror("tdfx_map_card: mmap framebuffer");
        munmap(reg_ptr, reg_size);
        close(fd);
        return -1;
    }

    *fd_out  = fd;
    *reg_out = reg_ptr;
    *fb_out  = fb_ptr;
    return 0;
}

/*
 * tdfx_query_board() - retrieve hardware info for a specific card.
 * Returns the populated tdfx_board_info_t, or -1 on error.
 * Use device_id to decide between Voodoo1 and Voodoo2 init paths.
 */
static int
tdfx_query_board(int fd, int card_index, tdfx_board_info_t *info)
{
    info->board_index = card_index;
    if (ioctl(fd, TDFX_GET_BOARDINFO, info) < 0) {
        perror("tdfx_query_board: ioctl");
        return -1;
    }
    return 0;
}

static void
tdfx_unmap_card(int fd, void *reg_ptr, void *fb_ptr, unsigned long reg_size)
{
    if (fb_ptr  && fb_ptr  != MAP_FAILED) munmap(fb_ptr,  reg_size);
    if (reg_ptr && reg_ptr != MAP_FAILED) munmap(reg_ptr, reg_size);
    if (fd >= 0) close(fd);
}

#endif /* !_KERNEL */

#endif /* _TDFX_IRIX_H */

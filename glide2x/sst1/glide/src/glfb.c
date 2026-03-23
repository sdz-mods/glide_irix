/*
** THIS SOFTWARE IS SUBJECT TO COPYRIGHT PROTECTION AND IS OFFERED ONLY
** PURSUANT TO THE 3DFX GLIDE GENERAL PUBLIC LICENSE. THERE IS NO RIGHT
** TO USE THE GLIDE TRADEMARK WITHOUT PRIOR WRITTEN PERMISSION OF 3DFX
** INTERACTIVE, INC. A COPY OF THIS LICENSE MAY BE OBTAINED FROM THE 
** DISTRIBUTOR OR BY CONTACTING 3DFX INTERACTIVE INC(info@3dfx.com). 
** THIS PROGRAM IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND, EITHER 
** EXPRESSED OR IMPLIED. SEE THE 3DFX GLIDE GENERAL PUBLIC LICENSE FOR A
** FULL TEXT OF THE NON-WARRANTY PROVISIONS.  
** 
** USE, DUPLICATION OR DISCLOSURE BY THE GOVERNMENT IS SUBJECT TO
** RESTRICTIONS AS SET FORTH IN SUBDIVISION (C)(1)(II) OF THE RIGHTS IN
** TECHNICAL DATA AND COMPUTER SOFTWARE CLAUSE AT DFARS 252.227-7013,
** AND/OR IN SIMILAR OR SUCCESSOR CLAUSES IN THE FAR, DOD OR NASA FAR
** SUPPLEMENT. UNPUBLISHED RIGHTS RESERVED UNDER THE COPYRIGHT LAWS OF
** THE UNITED STATES.  
** 
** COPYRIGHT 3DFX INTERACTIVE, INC. 1999, ALL RIGHTS RESERVED
 * 
 * 23    6/30/97 10:51a Dow
 * Changed size check in grLfbLock to warning
 * 
 * 22    6/03/97 3:50p Pgj
 * fix bugs 580 and 581
 * 
 * 21    5/05/97 2:25p Hanson
 * Fixed unlock bug with grLfbUnlock. On Rush it would always return
 * false.
 * 
 * 20    5/02/97 9:43a Dow
 * Changed indentation to match rest of file, made grLfbLock idle on Jr.
 * 
 * 19    4/08/97 12:47p Jdt
 * Bug in grLfbUnlock()
 * 
 * 18    4/02/97 3:24a Hanson
 * Fixed bug with locking read of Auxbuffer on SST1
 * 
 * 17    3/27/97 9:30p Hanson
 * Fixed my mistake of masking out the no idle bit to late in the
 * grLfbUnlock code.
 * 
 * 16    3/26/97 5:04p Hanson
 * 
 * 15    3/19/97 1:37a Jdt
 * Added real stride computation.
 * 
 * 14    3/18/97 9:07p Dow
 * Got rid of #$#%#$ // comments
 * 
 * 13    3/13/97 1:18a Jdt
 * Re-wrote grLfbWriteRegion to be faster and also be nice to the fifo.
 * Added grLfbReadRegion for sli boards.  Need an sli setup on whihc to
 * test.
 * 
 * 12    3/12/97 2:54p Jdt
 * Fixes for vg96
 * 
 * 11    3/12/97 11:11a Jdt
 * Fixed format bug.  Fixed yorigin problem. Fixed locking bug.
 * 
 * 10    3/06/97 9:03p Jdt
 * Removed Watcom Warning.  
 * 
 * 9     3/05/97 9:35p Jdt
 * Added grLfbWriteRegion
 * 
 * 8     3/04/97 9:08p Dow
 * Neutered multiplatform multiheaded monster
 * 
 * 7     2/26/97 11:56a Jdt
 * changed lfb api
 * 
 * 6     12/23/96 1:37p Dow
 * chagnes for multiplatform glide
**
*/
#include <string.h>
#include <3dfx.h>
#define FX_DLL_DEFINITION
#include <fxdll.h>
#include <glide.h>
#include "fxglide.h"

#if defined(__sgi__) || defined(IRIX)
#include <stdlib.h>

/*
 * IRIX/MACE shadow-buffer state for grLfbLock/grLfbUnlock WRITE_ONLY.
 *
 * MACE silently swaps adjacent pixel pairs on 16-bit PCI writes (XOR^2
 * address routing).  To fix this transparently for callers that write
 * directly through the locked info->lfbPtr, we redirect them to a
 * shadow buffer and flush it to the real LFB using 32-bit stores
 * (two pixels packed per store) in grLfbUnlock.
 *
 * The shadow buffer is allocated once on first use and reused across
 * frames to avoid per-frame malloc/free overhead.
 *
 * On flush we scan backward from the last row to find the highest row
 * the caller actually wrote, and flush only up to that row.  This
 * avoids issuing unnecessary PCI writes for rows the caller left empty.
 * grLfbWriteRegion bypasses this mechanism entirely and manages its own
 * 32-bit packing.
 */
static void            *_irix_shadow_buf     = NULL;
static FxU32            _irix_shadow_cap     = 0; /* allocated bytes */
static void            *_irix_real_lfbptr    = NULL;
static FxU32            _irix_shadow_stride  = 0;
static FxU32            _irix_shadow_height  = 0;
static GrLfbWriteMode_t _irix_shadow_wmode   = GR_LFBWRITEMODE_565;

/* Return the index of the last row that contains any non-zero word,
 * or 0 if only row 0 (or nothing) was written.  Scanning cached RAM
 * is cheap; it avoids flushing empty rows over the PCI bus. */
static FxU32 _irix_find_last_dirty_row(void)
{
  FxU32 words_per_row = _irix_shadow_stride >> 2;
  FxU32 y;
  for (y = _irix_shadow_height; y > 0; y--) {
    const FxU32 *row = (const FxU32 *)
      ((const char *)_irix_shadow_buf + (y - 1) * _irix_shadow_stride);
    FxU32 x;
    for (x = 0; x < words_per_row; x++) {
      if (row[x]) return y; /* row y-1 has data; flush up to row y */
    }
  }
  return 0;
}

static void _irix_flush_shadow_to_lfb(void)
{
  FxU32 flush_rows = _irix_find_last_dirty_row();
  FxU32 words_per_row;
  FxU32 y;

  if (flush_rows == 0) return;

  switch (_irix_shadow_wmode) {
  case GR_LFBWRITEMODE_888:
  case GR_LFBWRITEMODE_8888:
  case GR_LFBWRITEMODE_565_DEPTH:
  case GR_LFBWRITEMODE_555_DEPTH:
  case GR_LFBWRITEMODE_1555_DEPTH:
    /* 32-bit pixel format: one pixel per word, no packing needed */
    words_per_row = _irix_shadow_stride >> 2;
    for (y = 0; y < flush_rows; y++) {
      const FxU32    *src = (const FxU32 *)
        ((const char *)_irix_shadow_buf + y * _irix_shadow_stride);
      volatile FxU32 *dst = (volatile FxU32 *)
        ((char *)_irix_real_lfbptr      + y * _irix_shadow_stride);
      FxU32 x;
      for (x = 0; x < words_per_row; x++)
        dst[x] = src[x];
    }
    break;

  default:
    /*
     * 16-bit pixel formats (565, 555, 1555, ZA16):
     * Pack two adjacent pixels into one 32-bit store so that MACE's
     * word-swap window places them correctly on the Voodoo bus.
     * MACE full 4-byte endian reversal: word written as (c1<<16)|c0
     * arrives at Voodoo as col_even=c0, col_odd=c1.
     */
    words_per_row = _irix_shadow_stride >> 2; /* pairs of FxU16 */
    for (y = 0; y < flush_rows; y++) {
      const FxU16    *src = (const FxU16 *)
        ((const char *)_irix_shadow_buf + y * _irix_shadow_stride);
      volatile FxU32 *dst = (volatile FxU32 *)
        ((char *)_irix_real_lfbptr      + y * _irix_shadow_stride);
      FxU32 x;
      for (x = 0; x < words_per_row; x++)
        dst[x] = ((FxU32)src[x*2+1] << 16) | (FxU32)src[x*2];
    }
    break;
  }
}

/*
 * Called by grBufferSwap to commit any pending shadow-buffer LFB writes
 * to hardware before the buffer flip, without releasing the lock.  This
 * supports callers that hold grLfbLock open across multiple frames
 * (e.g. qatest01) and never call grLfbUnlock mid-loop.
 * After flushing, the shadow buffer is zeroed so the dirty-row scan is
 * accurate for the next frame's writes.
 */
void _irix_lfb_presync_flush(void)
{
  FxU32 bufSize;
  if (_irix_real_lfbptr == NULL) return;
  _irix_flush_shadow_to_lfb();
  bufSize = _irix_shadow_stride * _irix_shadow_height;
  memset(_irix_shadow_buf, 0, bufSize);
}
#endif /* IRIX */

/*---------------------------------------------------------------------------
** grLfbConstantAlpha
*/
GR_ENTRY(grLfbConstantAlpha, void, ( GrAlpha_t alpha ))
{
  GR_BEGIN_NOFIFOCHECK("grLfbConstantAlpha",82);
  GDBG_INFO_MORE((gc->myLevel,"(0x%x)\n",alpha));

  gc->state.lfb_constant_alpha = alpha;
  GR_END();
} /* grLfbConstantAlpha */

/*---------------------------------------------------------------------------
** grLfbConstantDepth
*/
GR_ENTRY(grLfbConstantDepth, void, ( FxU16 depth ))
{
  GR_BEGIN_NOFIFOCHECK("grLfbConstantDepth",82);
  GDBG_INFO_MORE((gc->myLevel,"(0x%x)\n",depth));

  gc->state.lfb_constant_depth = depth;
  GR_END();
} /* grLfbConstantDepth */


/*-------------------------------------------------------------------
  Function: grLfbLock
  Date: 2/19
  Implementor(s): jdt
  Library: Glide
  Description:
  Secures a pointer to a requested frame buffer and guarantees ordered
  access to that buffer.

  Arguments:
  type - type of lock, one of:
            GR_LOCK_WRITE_ONLY
            GR_LOCK_READ_ONLY
  buffer - which buffer to lock, one of:
            GR_BUFFER_FRONTBUFFER
            GR_BUFFER_BACKBUFFER
            GR_BUFFER_AUXBUFFER
  writeMode - desired destination color format
  origin - desired lfb origin
  pixelPipeline - flag whether to process through pixpipe
  info   - pointer to info structure
  Return:
  FXTRUE  - lock is successful
  FXFALSE - lock fails
  -------------------------------------------------------------------*/

GR_ENTRY(grLfbLock, FxBool,( GrLock_t type, GrBuffer_t buffer,  GrLfbWriteMode_t writeMode, GrOriginLocation_t origin,  FxBool pixelPipeline, GrLfbInfo_t *info ) ) 
{
  FxBool rv = FXTRUE;
  FxU32  lockIdle;
  
  GR_BEGIN_NOFIFOCHECK("grLfbLock",82);
  GDBG_INFO_MORE((gc->myLevel,"(%d,%d,0x%x)\n", type, buffer, info));
  
  GR_CHECK_F( myName, !info,
             "Null info structure passed." );

  /* changed from GR_CHECK_F to match the docuementation */

  GR_CHECK_W(myName, (info->size != sizeof( GrLfbInfo_t )),
             "Invalid size sent to grLfbLock()");
  
  lockIdle = !(type & GR_LFB_NOIDLE);
  type = type & ~(GR_LFB_NOIDLE);
  
#if ( GLIDE_PLATFORM & GLIDE_HW_SST1 )
  {
    switch( type ) {
    case GR_LFB_READ_ONLY:
      {
        FxU32 lfbMode;

        if ( gc->lockPtrs[type] != (FxU32)-1 ) {
          GDBG_INFO(( 83, "Read lock failure due to existing lock" ));
          rv = FXFALSE;
        } else {
          lfbMode = gc->state.fbi_config.lfbMode;
          lfbMode &= ~( SST_LFB_READBUFSELECT | 
                       SST_LFB_YORIGIN );
                    
          GR_SET_EXPECTED_SIZE( 4 );

          /* You may not read-lock an SLI board */
          if ( (gc->scanline_interleaved == FXTRUE) &&
              (gc->lfbSliOk==1) ) {
            GDBG_INFO((83, 
                       "Read lock failure: read lock"
                       " disallowed for SLI, use "
                       "grLfbReadRegion()\n" ));
            rv = FXFALSE;
          }

          switch( buffer ) {
          case GR_BUFFER_FRONTBUFFER:
            lfbMode |= SST_LFB_READFRONTBUFFER;
            break;
          case GR_BUFFER_BACKBUFFER:
            lfbMode |= SST_LFB_READBACKBUFFER;
            break;
          case GR_BUFFER_AUXBUFFER:
            lfbMode |= SST_LFB_READAUXBUFFER;
            break;
          default:
            GR_CHECK_F( myName, 
                       1, 
                       "illegal buffer parameter passed" );
            rv = FXFALSE;
            break;
          }

          lfbMode |= ( origin )?SST_LFB_YORIGIN:0;

          GR_SET( hw->lfbMode, lfbMode );
          gc->state.fbi_config.lfbMode = lfbMode;

          info->lfbPtr        = gc->lfb_ptr;
          info->strideInBytes = gc->fbStride * 2;
          info->origin        = origin;

          if ( rv == FXTRUE )
            gc->lockPtrs[type] = buffer;
        }
      }
      break;
    case GR_LFB_WRITE_ONLY:
      {
        FxU32 lfbMode, zaColor, fbzMode;

                
        if ( gc->lockPtrs[type] != (FxU32)-1 ) {
          GDBG_INFO(( 83, "Write failure due to existing lock" ));
          rv = FXFALSE;
        } else {
          GR_SET_EXPECTED_SIZE( 12 );
                    
          /*
           ** Set up the constant depth register because it may have been
           ** trashed by a call to grDepthBiasLevel (depthbiaslevel and
           ** constant depth use the same register)
           */
          zaColor = gc->state.fbi_config.zaColor;
          zaColor = ((( FxU32 ) gc->state.lfb_constant_depth ) << SST_ZACOLOR_DEPTH_SHIFT );
          zaColor |= (gc->state.lfb_constant_alpha << SST_ZACOLOR_ALPHA_SHIFT);
          GR_SET( hw->zaColor, zaColor );
                    
          /*
           ** disable depth biasing
           */
          fbzMode = gc->state.fbi_config.fbzMode;
          fbzMode &= ~( SST_ENZBIAS );
                    
          lfbMode = gc->state.fbi_config.lfbMode;
          lfbMode &= ~( SST_LFB_WRITEBUFSELECT |
                       SST_LFB_YORIGIN        |
                       SST_LFB_FORMAT         |
                       SST_LFB_ENPIXPIPE );
                    
          switch( writeMode ) {
          case GR_LFBWRITEMODE_RESERVED1:
          case GR_LFBWRITEMODE_RESERVED2:
          case GR_LFBWRITEMODE_RESERVED3:
          case GR_LFBWRITEMODE_RESERVED4:
          case GR_LFBWRITEMODE_RESERVED5:
          case GR_LFBWRITEMODE_RESERVED6:
          case GR_LFBWRITEMODE_RESERVED7:
            rv = FXFALSE;
          }

          switch( buffer ) {
          case GR_BUFFER_FRONTBUFFER:
            if (writeMode == GR_LFBWRITEMODE_ANY) {
                writeMode = GR_LFBWRITEMODE_565;
            } else if ( writeMode == GR_LFBWRITEMODE_ZA16 ) {
              rv = FXFALSE;
            }
            lfbMode |= SST_LFB_WRITEFRONTBUFFER;
            break;
          case GR_BUFFER_BACKBUFFER:
            if (writeMode == GR_LFBWRITEMODE_ANY) {
                writeMode = GR_LFBWRITEMODE_565;
            } else if ( writeMode == GR_LFBWRITEMODE_ZA16 ) {
              rv = FXFALSE;
            }
            lfbMode |= SST_LFB_WRITEBACKBUFFER;
            break;
          case GR_BUFFER_AUXBUFFER:
            if (writeMode == GR_LFBWRITEMODE_ANY) {
                writeMode = GR_LFBWRITEMODE_ZA16;
            } else if ( writeMode != GR_LFBWRITEMODE_ZA16 ) {
              rv = FXFALSE;
            }
            lfbMode |= SST_LFB_WRITEFRONTBUFFER;
            break;
          default:
            GR_CHECK_F( myName, 
                       1, 
                       "illegal buffer parameter passed" );
            break;
          } /* end switch(buffer) */

          lfbMode |= ( writeMode << SST_LFB_FORMAT_SHIFT );
          lfbMode |= ( origin )?SST_LFB_YORIGIN:0;

          if ( pixelPipeline ) {
            lfbMode |= SST_LFB_ENPIXPIPE;
            fbzMode &= ~SST_YORIGIN;
            fbzMode |= (origin)?SST_YORIGIN:0;
          }

          GR_SET( hw->lfbMode, lfbMode );
          gc->state.fbi_config.lfbMode = lfbMode;

          GR_SET( hw->fbzMode, fbzMode );
          P6FENCE;              /* This is required to flush the write
                                   buffers before the actual LFB writes */

          /* GMT: very low-risk race condition is probable here
             if a direct LFB read is done aftewards, the lfbMode register might
             not have settled in yet, and the pixel may be read in the wrong
             mode.  However, the 2 write above help reduce the chance of this.
             The proper thing to do is to call grSstIdle() here, but that's 
             too slow.
             */

          info->lfbPtr    = gc->lfb_ptr;
          info->origin    = origin;
          info->writeMode = writeMode;

          switch( lfbMode & SST_LFB_FORMAT ) {
          case SST_LFB_565:
          case SST_LFB_555:
          case SST_LFB_1555:
          case SST_LFB_ZZ:
            info->strideInBytes = gc->fbStride * 2;
            break;
          case SST_LFB_888:
          case SST_LFB_8888:
          case SST_LFB_Z565:
          case SST_LFB_Z555:
          case SST_LFB_Z1555:
            info->strideInBytes = gc->fbStride * 4;
            break;
          }

#if defined(__sgi__) || defined(IRIX)
          /* Redirect caller to a shadow buffer; flushed via 32-bit stores
           * in grLfbUnlock to work around MACE 16-bit XOR^2 write routing.
           * See _irix_flush_shadow_to_lfb() for details.
           * grLfbWriteRegion bypasses this and manages its own packing. */
          if ( rv == FXTRUE ) {
            FxU32 bufSize = info->strideInBytes * gc->state.screen_height;
            if ( _irix_shadow_cap < bufSize ) {
              /* Allocate or grow the persistent shadow buffer. */
              void *nb = malloc(bufSize);
              if ( nb ) {
                free(_irix_shadow_buf);
                _irix_shadow_buf = nb;
                _irix_shadow_cap = bufSize;
              }
            }
            if ( _irix_shadow_buf ) {
              memset(_irix_shadow_buf, 0, bufSize);
              _irix_real_lfbptr    = gc->lfb_ptr;
              _irix_shadow_stride  = info->strideInBytes;
              _irix_shadow_height  = gc->state.screen_height;
              _irix_shadow_wmode   = writeMode;
              info->lfbPtr         = _irix_shadow_buf;
            }
            /* If no buffer (malloc failed): fall back to direct writes
             * (pixels may swap, but better than nothing). */
          }
#endif /* IRIX */

          if ( rv == FXTRUE )
            gc->lockPtrs[type] = buffer;
        }
      }
      break;
    default:
      rv = FXFALSE;
      GDBG_INFO((gc->myLevel,  "Lock failed because of invalid lock type." ));
      break;
    }
  }
  
#elif ( GLIDE_PLATFORM & GLIDE_HW_SST96 )
  switch( type ) {
  case GR_LFB_READ_ONLY:
    if ( gc->lockPtrs[type]!= (FxU32)-1 ) {
      GDBG_INFO(( 83, "Read lock failure due to existing lock" ));
      rv = FXFALSE;
    } else {
      info->lfbPtr = initGetBufferPtr( buffer, &info->strideInBytes );
      if ( origin != GR_ORIGIN_UPPER_LEFT ) {
        GDBG_INFO((gc->myLevel,  
                   "Lock failed because of unsu"
                   "pported origin." ));
        rv = FXFALSE;
      }
      info->origin = GR_ORIGIN_UPPER_LEFT;
      if ( rv == FXTRUE ) 
        gc->lockPtrs[type] = buffer;
    }
    break;
  case GR_LFB_WRITE_ONLY:
    if ( gc->lockPtrs[type]!= (FxU32)-1 ) {
      GDBG_INFO(( 83, "Read lock failure due to existing lock" ));
      rv = FXFALSE;
    } else {
      info->lfbPtr = initGetBufferPtr( buffer, &info->strideInBytes );
      if ( !info->lfbPtr ) {
        GDBG_INFO((gc->myLevel,  
                   "Lock failed because buffer doesn't exist" ));
        rv = FXFALSE;
      }
      if ( origin != GR_ORIGIN_UPPER_LEFT ) {
        GDBG_INFO((gc->myLevel,  
                   "Lock failed because of unsupported origin." ));
        rv = FXFALSE;
      }

      /* Voodoo Rush lfb writes don't go through the pixel pipeline */
      switch( buffer ) {
      case GR_BUFFER_FRONTBUFFER:
        if (writeMode == GR_LFBWRITEMODE_ANY) {
          writeMode = GR_LFBWRITEMODE_565;
        } else if ( writeMode != GR_LFBWRITEMODE_565 ) {
          rv = FXFALSE;
        }
        break;
      case GR_BUFFER_BACKBUFFER:
        if (writeMode == GR_LFBWRITEMODE_ANY) {
          writeMode = GR_LFBWRITEMODE_565;
        } else if ( writeMode != GR_LFBWRITEMODE_565 ) {
          rv = FXFALSE;
        }
        break;
      case GR_BUFFER_AUXBUFFER:
        if (writeMode == GR_LFBWRITEMODE_ANY) {
          writeMode = GR_LFBWRITEMODE_ZA16;
        } else if ( writeMode != GR_LFBWRITEMODE_ZA16 ) {
          rv = FXFALSE;
        }
        break;
      default:
        GR_CHECK_F( myName, 1, "illegal buffer parameter passed" );
        break;
      } /* end switch(buffer) */

      if ( pixelPipeline ) {
        GDBG_INFO((gc->myLevel,  
                   "Lock failed because pixelpipeline is not available on VG96." ));
        rv = FXFALSE;
      }
      info->writeMode = writeMode;
      info->origin = GR_ORIGIN_UPPER_LEFT;
      if ( rv == FXTRUE ) 
        gc->lockPtrs[type] = buffer;
      GDBG_INFO(( gc->myLevel, 
                 "Write Lock Successful: ptr=0x%.08x stride=%.04d\n",
                 info->lfbPtr,
                 info->strideInBytes ));
    }
    break;
  } /* end switch(type) */

#else
#  error "No lfb implementation for this platform"
#endif
  GR_CHECK_SIZE();
  /* We HAVE to idle on Jr., as out-of-order LFBs are unacceptable */
  if (lockIdle || (_GlideRoot.hwConfig.SSTs[_GlideRoot.current_sst].type == GR_SSTTYPE_SST96))
    grSstIdle();
  GR_RETURN( rv );
} /* grLfbLock */
    
/*-------------------------------------------------------------------
  Function: grLfbUnlock
  Date: 2/21
  Implementor(s): jdt
  Library: Glide
  Description:
  Unlock a previously locked buffer
  Arguments:
  type - type of lock ( read only/write only )
  buffer - which buffer to unlock 
  Return:
  FXTRUE  - success
  FXFALSE - failure
  -------------------------------------------------------------------*/

GR_ENTRY(grLfbUnlock, FxBool, ( GrLock_t type, GrBuffer_t buffer ) )
{
  FxBool rval = FXFALSE;
  
  GR_BEGIN_NOFIFOCHECK("grLfbUnLock",83 );
  GDBG_INFO_MORE((gc->myLevel,"(%d, %d)\n", type, buffer ));
  
  type = type & ~(GR_LFB_NOIDLE);
  
  GR_CHECK_F( myName, 
             type != GR_LFB_WRITE_ONLY &&
             type != GR_LFB_READ_ONLY,
             "Bad type" );
  
  GR_CHECK_F( myName, 
             buffer != GR_BUFFER_FRONTBUFFER &&
             buffer != GR_BUFFER_BACKBUFFER  &&
             buffer != GR_BUFFER_AUXBUFFER,
             "Bad buffer" );
  
#if ( GLIDE_PLATFORM & GLIDE_HW_SST1 )
  if ( gc->lockPtrs[type] == (FxU32)buffer ) {
    rval = FXTRUE;

#if defined(__sgi__) || defined(IRIX)
    /* Flush shadow buffer to real LFB using 32-bit stores before
     * restoring hardware registers (grSstIdle at the end of
     * grLfbLock already serialised the FIFO, so the lfbMode
     * register is stable). */
    if ( type == GR_LFB_WRITE_ONLY && _irix_real_lfbptr != NULL ) {
      _irix_flush_shadow_to_lfb();
      _irix_real_lfbptr = NULL; /* shadow buf itself is kept for reuse */
    }
#endif /* IRIX */

    if ( gc->scanline_interleaved == FXTRUE ) {
      GR_SET_EXPECTED_SIZE( 12 );
    } else {
      GR_SET_EXPECTED_SIZE( 8 );
    }

    /* Restore depth bias level */
    GR_SET( hw->zaColor, gc->state.fbi_config.zaColor );

    /* turn back on depth biasing */
    GR_SET( hw->fbzMode, gc->state.fbi_config.fbzMode );

    if ( gc->scanline_interleaved == FXTRUE )
      GR_SET( hw->nopCMD, 0x0 );

    gc->lockPtrs[type] = (FxU32)-1;
  } else {
    GDBG_INFO((gc->myLevel, "Unlock failed because there was no matching lock" ));
    rval = FXFALSE;
  }
#elif ( GLIDE_PLATFORM & GLIDE_HW_SST96 )
  if ( gc->lockPtrs[type] == (FxU32)buffer ) {
    rval = FXTRUE;

    gc->lockPtrs[type] = (FxU32)-1;
  } else {
    GDBG_INFO((gc->myLevel, "Unlock failed because there was no matching lock" ));
    rval = FXFALSE;
  }
#endif
  GR_RETURN( rval );
} /* grLfbUnlock */


/*---------------------------------------------------------------------------
** grLfbWriteColorFormat
**
*/

GR_ENTRY(grLfbWriteColorFormat, void, (GrColorFormat_t colorFormat))
{
#if !(GLIDE_PLATFORM & GLIDE_HW_SST96)
  FxU32 lfbMode;

  GR_BEGIN("grLfbWriteColorFormat",82,8);
  GDBG_INFO_MORE((gc->myLevel,"(%d)\n",colorFormat));

  GR_CHECK_F(myName,
              colorFormat < 0 || colorFormat > 0x3,
              "invalid color format");

  lfbMode = gc->state.fbi_config.lfbMode;
  lfbMode &= ~SST_LFB_RGBALANES;
  lfbMode |= (colorFormat << SST_LFB_RGBALANES_SHIFT);

  GR_SET( hw->lfbMode, lfbMode );
  gc->state.fbi_config.lfbMode = lfbMode;
  if ( gc->scanline_interleaved == FXTRUE ) 
    GR_SET( hw-> nopCMD, 0x0 );
  GR_END_SLOPPY();
#else
  return;
#endif
} /* grLfbWriteColorFormat */


/*---------------------------------------------------------------------------
**  grLfbWriteColorSwizzle  - set up SST for byte swizzling and word swapping
**
**  Registers/Bits Affected:
**  lfbMode:  bit(11), bit(12)
**
**  WARNING:  GMT: SST_LFB_WRITE_SWAP16 changes pixel addressing!!!
*/

GR_ENTRY(grLfbWriteColorSwizzle, void, (FxBool swizzleBytes, FxBool swapWords))
{
#if !(GLIDE_PLATFORM & GLIDE_HW_SST96)
  FxU32 lfbMode;

  GR_BEGIN("grLfbWriteColorSwizzle",82,8);
  GDBG_INFO_MORE((gc->myLevel,"(%d,%d)\n",swizzleBytes,swapWords));

  /* Clear out the bits we'll set back if appropriate */
  lfbMode = gc->state.fbi_config.lfbMode;
  lfbMode &= ~(SST_LFB_WRITE_SWAP16 | SST_LFB_WRITE_BYTESWAP);

  if (swizzleBytes)
    lfbMode |= SST_LFB_WRITE_BYTESWAP;

  if (swapWords)
    lfbMode |= SST_LFB_WRITE_SWAP16;

  GR_SET( hw->lfbMode, lfbMode );
  gc->state.fbi_config.lfbMode = lfbMode;
  if ( gc->scanline_interleaved == FXTRUE ) 
    GR_SET( hw-> nopCMD, 0x0 );
  GR_END_SLOPPY();
#else
  return;
#endif
} /* grLfbWriteColorSwizzle */

/*-------------------------------------------------------------------
  Function: grLfbWriteRegion
  Date: 3/5
  Implementor(s): jdt
  Library: Glide
  Description:
  Write a pixel rectangle to the frame buffer as efficiently as possible
  Arguments:
  dst_buffer  - buffer to which to copy data
  dst_x,dst_y - destination image start coordinates
  src_format  - data format of source image
  src_width, src_height 
              - dimensions of source image
  src_stride  - stride of source image in bytes, not meaningful
                for RLE images
  src_data    - pointer to source data memory           
  Return:
  FXTRUE  succeed
  FXFALSE fail
  -------------------------------------------------------------------*/
GR_ENTRY(grLfbWriteRegion, FxBool, ( GrBuffer_t dst_buffer, 
                                     FxU32 dst_x, FxU32 dst_y, 
                                     GrLfbSrcFmt_t src_format, 
                                     FxU32 src_width, FxU32 src_height, 
                                     FxI32 src_stride, void *src_data ))
{
  FxBool           rv = FXTRUE;
  GrLfbInfo_t      info;
  GrLfbWriteMode_t writeMode;
  
  GR_BEGIN_NOFIFOCHECK("grLfbWriteRegion",82);
  GDBG_INFO_MORE((gc->myLevel,
                  "(0x%x,%d,%d,%d,%d,%d,%d,0x%x)\n", 
                  dst_buffer, dst_x, dst_y, 
                  src_format, src_width, src_height,
                  src_stride, src_data ));
  
#if ( GLIDE_PLATFORM & GLIDE_HW_SST1 )
  if ( src_format == GR_LFB_SRC_FMT_RLE16 ) 
    writeMode = GR_LFBWRITEMODE_565;
  else 
    writeMode = src_format;
  
  info.size = sizeof( info );
  
  if ( grLfbLock( GR_LFB_WRITE_ONLY | GR_LFB_NOIDLE,
                 dst_buffer,
                 writeMode,
                 GR_ORIGIN_UPPER_LEFT,
                 FXFALSE,
                 &info ) ) {
    FxU32 *srcData;             /* Tracking Source Pointer */
    FxU32 *dstData;             /* Tracking Destination Pointer */
    FxU32 *end;                 /* Demarks End of each Scanline */
    FxI32 srcJump;              /* bytes to next scanline */
    FxU32 dstJump;              /* bytes to next scanline */
    FxU32 length;               /* bytes to copy in scanline */
    FxU32 scanline;             /* scanline number */
    int   aligned;              /* word aligned? */

#if defined(__sgi__) || defined(IRIX)
    /* grLfbWriteRegion handles its own 32-bit packing for IRIX.
     * Restore the real LFB pointer and clear real_lfbptr so that
     * grLfbUnlock skips the shadow-buffer flush. */
    if ( _irix_real_lfbptr ) {
      info.lfbPtr       = _irix_real_lfbptr;
      _irix_real_lfbptr = NULL;
    }
#endif

    srcData = ( FxU32 * ) src_data;
    dstData = ( FxU32 * ) ( ((char*)info.lfbPtr)+
                           (dst_y*info.strideInBytes) );
    scanline = src_height;
    
    switch( src_format ) {
      /* 16-bit aligned */
    case GR_LFB_SRC_FMT_565:
    case GR_LFB_SRC_FMT_555:
    case GR_LFB_SRC_FMT_1555:
    case GR_LFB_SRC_FMT_ZA16:
      dstData = (FxU32*)(((FxU16*)dstData) + dst_x);
      length  = src_width * 2;
      srcJump = src_stride - length;
      dstJump = info.strideInBytes - length;
#if defined(__sgi__) || defined(IRIX)
      /*
       * MIPS/IRIX: Use 32-bit stores, NOT 16-bit.
       *
       * MACE (O2 PCI bridge) uses XOR^2 routing for 16-bit PCI writes:
       * a 16-bit store to in-word offset 0 arrives at PCI bytes 2-3 (the
       * ODD pixel slot), and offset 2 arrives at bytes 0-1 (the EVEN slot).
       * This silently swaps every adjacent pixel pair in Voodoo DRAM.
       *
       * For 32-bit stores MACE does a full 4-byte endian reversal.  To
       * place P_even at column 2k and P_odd at column 2k+1, store:
       *   V = (P_odd << 16) | P_even
       *
       * Odd dst_x: the first pixel occupies the ODD slot of a 32-bit word.
       * Read the existing EVEN slot via byte reads (XOR^3 addressing,
       * unaffected by 16-bit routing), merge, and write back.
       */
      {
        volatile FxU32 *base32;
        const FxU16    *src16;
        FxU32           sl;

        base32 = (volatile FxU32 *)dstData;
        src16  = (const FxU16 *)srcData;
        for (sl = scanline; sl > 0; sl--) {
          volatile FxU32 *wp = base32;
          FxU32           px = 0;
          GR_SET_EXPECTED_SIZE(length);

          if ( (FxU32)wp & 2 ) {
            /* Leading odd pixel: read existing even pixel via byte reads,
             * then merge into a single 32-bit store. */
            volatile FxU32     *wptr = (volatile FxU32 *)((char *)wp - 2);
            volatile const FxU8 *p8  = (volatile const FxU8 *)wptr;
            FxU16 existing_even = ((FxU16)p8[2] << 8) | (FxU16)p8[3];
            *wptr = ((FxU32)src16[0] << 16) | existing_even;
            wp    = (volatile FxU32 *)((char *)wp + 2);
            px    = 1;
          }

          for ( ; px + 1 < src_width; px += 2 ) {
            *wp++ = ((FxU32)src16[px + 1] << 16) | src16[px];
          }

          if ( px < src_width ) {
            /* Trailing odd pixel: write even slot, zero the adjacent odd col. */
            *wp = (FxU32)src16[px];
          }

          base32 = (volatile FxU32 *)((char *)base32 + info.strideInBytes);
          src16  = (const FxU16 *)((const char *)src16 + src_stride);
          GR_CHECK_SIZE_SLOPPY();
        }
      }
#else
      aligned = !((int)dstData&0x2);
      if ( aligned ) {
        while( scanline-- ) {
          GR_SET_EXPECTED_SIZE(length);
          end = (FxU32*)((char*)srcData + length - 2);
          while( srcData < end ) {
            GR_SET( dstData[0], srcData[0] );
            dstData++;
            srcData++;
          }
          if ( ((int)length) & 0x2 ) {
            GR_SET16( (*(FxU16*)&(dstData[0])),
                     (*(FxU16*)&(srcData[0])) );
            dstData = (FxU32*)(((FxU16*)dstData) + 1 );
            srcData = (FxU32*)(((FxU16*)srcData) + 1 );
          }
          dstData = (FxU32*)(((char*)dstData)+dstJump);
          srcData = (FxU32*)(((char*)srcData)+srcJump);
          GR_CHECK_SIZE_SLOPPY();
        }
      } else {
        while( scanline-- ) {
          GR_SET_EXPECTED_SIZE(length);
          end = (FxU32*)((char*)srcData + length - 2);
          GR_SET16( (*(FxU16*)&(dstData[0])),
                   (*(FxU16*)&(srcData[0])) );
          dstData = (FxU32*)(((FxU16*)dstData) + 1 );
          srcData = (FxU32*)(((FxU16*)srcData) + 1 );
          while( srcData < end ) {
            GR_SET( dstData[0], srcData[0] );
            dstData++;
            srcData++;
          }
          if ( !(length & 0x2) ) {
            GR_SET16( (*(FxU16*)&(dstData[0])),
                     (*(FxU16*)&(srcData[0])) );
            dstData = (FxU32*)(((FxU16*)dstData) + 1 );
            srcData = (FxU32*)(((FxU16*)srcData) + 1 );
          }
          dstData = (FxU32*)(((char*)dstData)+dstJump);
          srcData = (FxU32*)(((char*)srcData)+srcJump);
          GR_CHECK_SIZE_SLOPPY();
        }
      }
#endif
      break;
      /* 32-bit aligned */
    case GR_LFB_SRC_FMT_888:
    case GR_LFB_SRC_FMT_8888:
    case GR_LFB_SRC_FMT_565_DEPTH:
    case GR_LFB_SRC_FMT_555_DEPTH:
    case GR_LFB_SRC_FMT_1555_DEPTH:
      dstData = ((FxU32*)dstData) + dst_x;
      length  = src_width * 4;
      srcJump = src_stride - length;
      dstJump = info.strideInBytes - length;
#if defined(__sgi__) || defined(IRIX)
      while ( scanline-- ) {
        GR_SET_EXPECTED_SIZE(length);
        memcpy( dstData, srcData, length );
        dstData = (FxU32*)((char*)dstData + info.strideInBytes);
        srcData = (FxU32*)((char*)srcData + src_stride);
        GR_CHECK_SIZE_SLOPPY();
      }
#else
      while( scanline-- ) {
        GR_SET_EXPECTED_SIZE(length);
        end = (FxU32*)((char*)srcData + length);
        while( srcData < end ) {
          GR_SET( dstData[0], srcData[0] );
          dstData++;
          srcData++;
        }
        dstData = (FxU32*)(((char*)dstData)+dstJump);
        srcData = (FxU32*)(((char*)srcData)+srcJump);
        GR_CHECK_SIZE_SLOPPY();
      }
#endif
      break;
    case GR_LFB_SRC_FMT_RLE16:
      /* needs to be implemented */
      rv = FXFALSE;
      break;
    }
    grLfbUnlock( GR_LFB_WRITE_ONLY, dst_buffer );
  } else {
    rv = FXFALSE;
  }
#elif ( GLIDE_PLATFORM & GLIDE_HW_SST96 )
  if ( src_format == GR_LFB_SRC_FMT_RLE16 ) {
    rv = FXFALSE;
    GR_RETURN( rv );
  } else 
    writeMode = src_format;
  
  info.size = sizeof( info );
  
  if ( grLfbLock( GR_LFB_WRITE_ONLY,
                 dst_buffer, 
                 writeMode,
                 GR_ORIGIN_UPPER_LEFT,
                 FXFALSE,
                 &info ) ) {
    FxU32 *srcData;             /* Tracking Source Pointer */
    FxU32 *dstData;             /* Tracking Destination Pointer */
    FxU32 *end;                 /* Demarks End of each Scanline */
    FxI32 srcJump;              /* bytes to next scanline */
    FxU32 dstJump;              /* bytes to next scanline */
    FxU32 length;               /* bytes to copy in scanline */
    FxU32 scanline;             /* scanline number */
    int   aligned;              /* word aligned? */
    
    
    srcData = ( FxU32 * ) src_data;
    dstData = ( FxU32 * ) ( ((char*)info.lfbPtr)+
                           (dst_y*info.strideInBytes)+
                           (dst_x<<1));
    scanline = src_height;
    length  = src_width * 2;
    aligned = !((int)dstData&0x2);
    srcJump = src_stride - length;
    dstJump = info.strideInBytes - length;
    if ( aligned ) {
      while( scanline-- ) {
        end = (FxU32*)((char*)srcData + length - 2);
        while( srcData < end ) 
          *dstData++ = *srcData++;
                
        if ( ((int)length) & 0x2 ) {
          *(FxU16*)dstData = *(FxU16*)srcData;
          dstData = (FxU32*)(((FxU16*)dstData) + 1 );
          srcData = (FxU32*)(((FxU16*)srcData) + 1 );
        }
                
        dstData = (FxU32*)(((char*)dstData)+dstJump);
        srcData = (FxU32*)(((char*)srcData)+srcJump);
      }
    } else {
      while( scanline-- ) {
        end = (FxU32*)((char*)srcData + length - 2);
                
        *(FxU16*)dstData = *(FxU16*)srcData;
        dstData = (FxU32*)(((FxU16*)dstData) + 1 );
        srcData = (FxU32*)(((FxU16*)srcData) + 1 );
                
        while( srcData < end ) 
          *dstData++ = *srcData++;
                
        if ( !(((int)length) & 0x2) ) {
          *(FxU16*)dstData = *(FxU16*)srcData;
          dstData = (FxU32*)(((FxU16*)dstData) + 1 );
          srcData = (FxU32*)(((FxU16*)srcData) + 1 );
        }
                
        dstData = (FxU32*)(((char*)dstData)+dstJump);
        srcData = (FxU32*)(((char*)srcData)+srcJump);
      }
    }
    grLfbUnlock( GR_LFB_WRITE_ONLY, dst_buffer );
  } else {
    rv = FXFALSE;
  }
#else 
#  error "No grLfbWriteRegion Implementation for this platform"     
#endif
  GR_RETURN( rv );
} /* grLfbWriteRegion */


/*-------------------------------------------------------------------
  Function: grLfbReadRegion
  Date: 3/12
  Implementor(s): jdt
  Library: Glide
  Description:
  Grab a rectangle from the frame buffer into user supplied memory
  Arguments:
  src_buffer - buffer to read from 
  src_x      - x coordinate of upper left corner rectangle to read
  src_y      - y coordinate of upper left corner of rectangle to read
  src_width  - width of rectangle to read
  src_height - height of rectangle to read
  dst_stride - distance between scanlines in destination buffer
  dst_data   - pointer to user memory in which to place image
  Return:
  FXTRUE - success
  FXFALSE - failure
  -------------------------------------------------------------------*/
GR_ENTRY(grLfbReadRegion, FxBool, ( GrBuffer_t src_buffer, 
                                    FxU32 src_x, FxU32 src_y, 
                                    FxU32 src_width, FxU32 src_height, 
                                    FxU32 dst_stride, void *dst_data ))
{
  FxBool rv = FXTRUE;
  GrLfbInfo_t info;
  
  GR_BEGIN_NOFIFOCHECK("grLfbReadRegion",82);
  GDBG_INFO_MORE((gc->myLevel,
                  "(0x%x,%d,%d,%d,%d,%d,0x%x)\n",
                  src_buffer, src_x, src_y,
                  src_width, src_height, dst_stride, dst_data ));
  
#if ( GLIDE_PLATFORM & GLIDE_HW_SST1 )
  gc->lfbSliOk = 1;
  info.size = sizeof( info );
  if ( grLfbLock( GR_LFB_READ_ONLY,
                 src_buffer, 
                 GR_LFBWRITEMODE_ANY,
                 GR_ORIGIN_UPPER_LEFT,
                 FXFALSE,
                 &info ) ) {
    FxU32 *srcData;             /* Tracking Source Pointer */
    FxU32 *dstData;             /* Tracking Destination Pointer */
    FxU32 *end;                 /* Demarks End of each Scanline */
    FxU32 srcJump;              /* bytes to next scanline */
    FxU32 dstJump;              /* bytes to next scanline */
    FxU32 length;               /* bytes to copy in scanline */
    FxU32 scanline;             /* scanline number */
    int   aligned;              /* word aligned? */
    FxU32 odd;                  /* is src_y odd? ( for sli ) */
    
    dstData = ( FxU32 * ) dst_data;
    srcData = ( FxU32 * ) ( ((char*)info.lfbPtr)+
                           (src_y*info.strideInBytes) +
                           (src_x<<1) );
    scanline = src_height;
    length   = src_width * 2;
    dstJump  = dst_stride - length;
    srcJump  = info.strideInBytes - length;
    odd      = (src_y+src_height) & 0x1;

#if defined(__sgi__) || defined(IRIX)
    /*
     * MIPS/IRIX: Voodoo 1 LFB read path.
     *
     * MACE performs a full 4-byte endian reversal on 32-bit PCI reads.
     * A 32-bit read of two adjacent 16-bit pixels therefore yields:
     *   word & 0xFFFF = col_even
     *   word >> 16    = col_odd
     *
     * Using volatile FxU32 reads (one word at a time) is essential.
     * Raw 16-bit FxU16 reads return byte-swapped pixel values because
     * MACE only swaps within the halfword for 16-bit transactions, not
     * the full 4-byte reversal needed to reconstruct the correct value.
     * grLfbReadRegion passes readback tests; callers that read directly
     * via a FxU16 pointer obtained from grLfbLock READ_ONLY will get
     * incorrect values on IRIX for this reason.
     *
     * If src_x is odd, the first pixel occupies the ODD slot of the
     * preceding aligned word (word >> 16).
     */
    {
      const FxU16       *src16 = (const FxU16 *)srcData;
      FxU16             *dst16 = (FxU16 *)dstData;
      while ( scanline-- ) {
        volatile const FxU32 *src32;
        FxU32 px = 0;
        if ( (int)src16 & 2 ) {
          /* start not 4-byte aligned: pixel at src_x is the ODD slot */
          FxU32 word = *(volatile const FxU32 *)((const char *)src16 - 2);
          dst16[0] = (FxU16)(word >> 16);
          px = 1;
          src32 = (volatile const FxU32 *)(src16 + 1);
        } else {
          src32 = (volatile const FxU32 *)src16;
        }
        for ( ; px + 1 < src_width; px += 2 ) {
          FxU32 word = *src32++;
          dst16[px]     = (FxU16)(word & 0xFFFF); /* col_even */
          dst16[px + 1] = (FxU16)(word >> 16);    /* col_odd  */
        }
        if ( px < src_width ) {
          dst16[px] = (FxU16)(*src32 & 0xFFFF);   /* trailing col_even */
        }
        src16 = (const FxU16 *)((const char *)src16 + info.strideInBytes);
        dst16 = (FxU16 *)((char *)dst16 + dst_stride);
      }
    }
#else
    aligned  = !((int)srcData&0x2);
    if ( aligned ) {
      while( scanline-- ) {
        end = (FxU32*)((char*)srcData + length - 2);

        if(gc->scanline_interleaved == FXTRUE) {
          if((scanline+odd) & 0x1)
            sst1InitSliPciOwner(gc->base_ptr, SST_SLI_MASTER_OWNPCI);
          else
            sst1InitSliPciOwner(gc->base_ptr, SST_SLI_SLAVE_OWNPCI);
        }

        while( srcData < end )
          *dstData++ = *srcData++;

        if ( ((int)length) & 0x2 ) {
          (*(FxU16*)dstData) = (*(FxU16*)srcData);
          dstData = (FxU32*)(((FxU16*)dstData) + 1 );
          srcData = (FxU32*)(((FxU16*)srcData) + 1 );
        }

        dstData = (FxU32*)(((char*)dstData)+dstJump);
        srcData = (FxU32*)(((char*)srcData)+srcJump);
      }
    } else {
      while( scanline-- ) {
        end = (FxU32*)((char*)srcData + length - 2);

        if(gc->scanline_interleaved == FXTRUE) {
          if((scanline+odd) & 0x1)
            sst1InitSliPciOwner(gc->base_ptr, SST_SLI_MASTER_OWNPCI);
          else
            sst1InitSliPciOwner(gc->base_ptr, SST_SLI_SLAVE_OWNPCI);
        }

        (*(FxU16*)dstData) = (*(FxU16*)srcData);
        dstData = (FxU32*)(((FxU16*)dstData) + 1 );
        srcData = (FxU32*)(((FxU16*)srcData) + 1 );

        while( srcData < end )
          *dstData++ = *srcData++;

        if ( !(((int)length) & 0x2) ) {
          (*(FxU16*)dstData) = (*(FxU16*)srcData);
          dstData = (FxU32*)(((FxU16*)dstData) + 1 );
          srcData = (FxU32*)(((FxU16*)srcData) + 1 );
        }

        dstData = (FxU32*)(((char*)dstData)+dstJump);
        srcData = (FxU32*)(((char*)srcData)+srcJump);
      }
    }
#endif
    grLfbUnlock( GR_LFB_READ_ONLY, src_buffer );
    if ( gc->scanline_interleaved )
      sst1InitSliPciOwner( gc->base_ptr, SST_SLI_MASTER_OWNPCI );
  } else {
    rv = FXFALSE;
  }
#elif ( GLIDE_PLATFORM & GLIDE_HW_SST96 )
  info.size = sizeof( info );
  if ( grLfbLock( GR_LFB_READ_ONLY,
                 src_buffer, 
                 GR_LFBWRITEMODE_ANY,
                 GR_ORIGIN_UPPER_LEFT,
                 FXFALSE,
                 &info ) ) {
    FxU32 *srcData;             /* Tracking Source Pointer */
    FxU32 *dstData;             /* Tracking Destination Pointer */
    FxU32 *end;                 /* Demarks End of each Scanline */
    FxU32 srcJump;              /* bytes to next scanline */
    FxU32 dstJump;              /* bytes to next scanline */
    FxU32 length;               /* bytes to copy in scanline */
    FxU32 scanline;             /* scanline number */
    int   aligned;              /* word aligned? */
    
    dstData = ( FxU32 * ) dst_data;
    srcData = ( FxU32 * ) ( ((char*)info.lfbPtr)+
                           (src_y*info.strideInBytes) +
                           (src_x<<1) );
    scanline = src_height;
    length   = src_width * 2;
    dstJump  = dst_stride - length;
    srcJump  = info.strideInBytes - length;
    aligned  = !((int)srcData&0x2);
    
    if ( aligned ) {
      while( scanline-- ) {
        end = (FxU32*)((char*)srcData + length - 2);

        while( srcData < end ) 
          *dstData++ = *srcData++;
                
        if ( ((int)length) & 0x2 ) {
          (*(FxU16*)dstData) = (*(FxU16*)srcData);
          dstData = (FxU32*)(((FxU16*)dstData) + 1 );
          srcData = (FxU32*)(((FxU16*)srcData) + 1 );
        }
                
        dstData = (FxU32*)(((char*)dstData)+dstJump);
        srcData = (FxU32*)(((char*)srcData)+srcJump);
      }
    } else {
      while( scanline-- ) {
        end = (FxU32*)((char*)srcData + length - 2);
                
        (*(FxU16*)dstData) = (*(FxU16*)srcData);
        dstData = (FxU32*)(((FxU16*)dstData) + 1 );
        srcData = (FxU32*)(((FxU16*)srcData) + 1 );

        while( srcData < end )
          *dstData++ = *srcData++;

        if ( !(((int)length) & 0x2) ) {
          (*(FxU16*)dstData) = (*(FxU16*)srcData);
          dstData = (FxU32*)(((FxU16*)dstData) + 1 );
          srcData = (FxU32*)(((FxU16*)srcData) + 1 );
        }
                
        dstData = (FxU32*)(((char*)dstData)+dstJump);
        srcData = (FxU32*)(((char*)srcData)+srcJump);
      }
    }
    grLfbUnlock( GR_LFB_READ_ONLY, src_buffer );
  } else {
    rv = FXFALSE;
  }
#else 
#  error "No grLfbReadRegion Implementation for this platform"     
#endif
  GR_RETURN( rv );
}/* grLfbReadRegion */


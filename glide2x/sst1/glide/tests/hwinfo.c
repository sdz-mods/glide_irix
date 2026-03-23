/*
** hwinfo.c - Print 3Dfx hardware information via the Glide API.
**
** Queries FBI revision, FBI memory, TMU count, TMU revision, and
** TMU memory for all detected 3Dfx boards without opening a display
** window.
**
** Build: part of Makefile.tests_irix
** Usage: ./hwinfo
*/

#include <stdio.h>
#include <stdlib.h>
#include <glide.h>

static const char *
sst_type_name(GrSstType type)
{
    switch (type) {
    case GR_SSTTYPE_VOODOO:  return "Voodoo 1 (SST-1)";
    case GR_SSTTYPE_Voodoo2: return "Voodoo 2";
    case GR_SSTTYPE_SST96:   return "SST-96 (Rush)";
    case GR_SSTTYPE_AT3D:    return "AT3D";
    default:                 return "Unknown";
    }
}

int
main(void)
{
    GrHwConfiguration hw;
    int i;
    int j;

    grGlideInit();

    if ( !grSstQueryHardware( &hw ) ) {
        fprintf( stderr, "hwinfo: grSstQueryHardware failed\n" );
        grGlideShutdown();
        return 1;
    }

    printf( "3Dfx boards detected: %d\n", hw.num_sst );

    for ( i = 0; i < hw.num_sst; i++ ) {
        GrVoodooConfig_t *v = &hw.SSTs[i].sstBoard.VoodooConfig;

        printf( "\n" );
        printf( "Board %d\n", i );
        printf( "  Type        : %s\n", sst_type_name( hw.SSTs[i].type ) );

        if ( hw.SSTs[i].type != GR_SSTTYPE_VOODOO &&
             hw.SSTs[i].type != GR_SSTTYPE_Voodoo2 ) {
            printf( "  (no detailed info for this board type)\n" );
            continue;
        }

        printf( "  FBI rev     : %d\n",    v->fbiRev );
        printf( "  FBI RAM     : %d MB\n", v->fbRam  );
        printf( "  TMU count   : %d\n",    v->nTexelfx );

        for ( j = 0; j < v->nTexelfx; j++ ) {
            printf( "  TMU %d rev   : %d\n",    j, v->tmuConfig[j].tmuRev );
            printf( "  TMU %d RAM   : %d MB\n", j, v->tmuConfig[j].tmuRam );
        }
    }

    printf( "\n" );
    grGlideShutdown();
    return 0;
}

/*
 * irix_sync.s - MIPS SYNC instruction wrapper for MIPSPro (n32 ABI)
 *
 * MIPSPro does not support inline assembly on MIPS targets.  This file
 * provides irix_sync(), which issues the SYNC instruction to drain the
 * CPU write buffer before the caller issues a hardware command register
 * write.  Required for correct Voodoo1 register write ordering via the
 * MACE PCI bridge on SGI O2.
 *
 * .option pic0 disables PIC constraints so the assembler does not
 * mis-classify "jr $31" (= return) as a jump to an external symbol.
 */

        .text
        .option pic0
        .globl  irix_sync
        .ent    irix_sync
irix_sync:
        .set noreorder
        sync
        jr      $31
        nop
        .set reorder
        .end    irix_sync

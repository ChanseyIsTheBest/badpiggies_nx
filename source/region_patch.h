/* region_patch.h -- Bad Piggies libunity.so 256MB->64MB memory-region granularity
 * patch table (Unity 2020.3.39f1, arm64-v8a).  [DERIVED + VERIFIED, 20 sites]
 *
 * Auto-derived by tools/re/region_derive.py, which sweeps the WHOLE allocator address range
 * (0x1a0ce8-0x1a7530, LocalLowLevelAllocator .. TLSAllocator) and classifies every granularity-
 * encoding instruction by EXACT value -- not a hand-picked function list. Cross-checked against
 * laytonbmr_nx (Unity 6, 18 sites) and zookeeperdx_nx (2022.3, 21 sites). All 20 `from` words
 * verified against the shipping binary; all `to` words disassemble to the 64MB form.
 *
 * WHY: Unity's MemoryManager indexes a TWO-LEVEL block table at 256MB (2^28) granularity. On a
 * 4GB Switch the boot reservation set does not fit; re-tiling to 64MB (2^26) lets ~4x more
 * regions fit. Harmless on 8GB.
 *
 * THREE COHERENT GROUPS -- every member of each MUST move together, or you get silent,
 * ASLR-dependent corruption rather than a clean failure:
 *
 *  (1) BLOCK-TABLE INDEX (page = ptr>>GRAN; lvl-2 = ubfx #GRAN,#12; lvl-1 = page>>12):
 *      MarkMemoryBlocks (the WRITER), GetMemoryBlockFromPointer, GetBlockInfoFromPointer,
 *      and MemoryManager::GetAllocatorContainingPtr. Miss the last one and a freed pointer
 *      resolves to the WRONG allocator.
 *      This build derives lvl-1 by shifting the PAGE INDEX (asr #12), NOT a hard-coded
 *      `lsr #40`. Verified exhaustively: ZERO immr==40 bitfield insns exist in all 3,207,765
 *      .text instructions, so the `>>40 -> >>38` fix that 2022.3 needs does NOT apply here.
 *
 *  (2) ALIGN-UP PAIRS  (size + (GRAN-1)) & ~(GRAN-1):  the addend AND the mask.
 *      LocalLowLevel::ReserveMemoryBlock, BucketAllocator::ctor, DHA::RequestLargeAllocMemory.
 *      Patching only the mask leaves (size + 256MB-1) & ~(64MB-1) -- still 64MB-aligned, but it
 *      adds up to 255MB of slack to EVERY reservation, over-allocating exactly where a 4GB
 *      Switch has no room. That silently defeats the whole patch.
 *
 *  (3) REGION-SIZE CONSTANTS (what actually gets reserved):
 *      VirtAlloc::ReserveMemoryBlock, DynamicHeapAllocator::ctor.
 *
 * KNOWN NON-SITE (do not "fix"): libunity+0x1a3880 `mov w0,#0x10000000` inside
 * AtomicPageAllocator::AllocatePage is an argument to FormatBytes(long) building an
 * error-message string on the page-exhaustion path -- cosmetic, not region math.
 * 2020.3's TLSAllocator::ThreadInitialize carries no 256MB const at all (unlike Unity 6),
 * so it contributes no site -- correct for this binary, not a miss.
 *
 * SELF-VERIFYING + all-or-nothing (main.c nx_patch_unity_regions): checks EVERY `from` word and
 * patches NOTHING on any mismatch, so a stock/updated/wrong build is left intact, never mixed.
 */
#ifndef REGION_PATCH_H
#define REGION_PATCH_H
#include <stdint.h>

typedef struct { uint32_t off, from, to; } region_patch_t;

/* {off, from(256MB), to(64MB)}. VADDR = load_virtbase + off. */
static const region_patch_t BP_REGION_PATCH[] = {
  {0x1a0d84, 0x32006fe9, 0x12bf8009},  /* LocalLowLevel::ReserveMemoryBlock    MOV gran-1       mov w9, #0xfffffff */
  {0x1a0d8c, 0x92648d36, 0x92669536},  /* LocalLowLevel::ReserveMemoryBlock    LOGIMM ~(gran-1) and x22, x9, #0xfffffffff0000000 */
  {0x1a1a78, 0xd35cfc33, 0xd35afc33},  /* VirtAlloc::MarkMemoryBlocks          BITF             lsr x19, x1, #0x1c */
  {0x1a1a7c, 0xd35cfd15, 0xd35afd15},  /* VirtAlloc::MarkMemoryBlocks          BITF             lsr x21, x8, #0x1c */
  {0x1a1af4, 0x320403e8, 0x52a08008},  /* VirtAlloc::ReserveMemoryBlock        MOV gran         mov w8, #0x10000000 */
  {0x1a1cec, 0xd35cfc2c, 0xd35afc2c},  /* VirtAlloc::GetMemoryBlockFromPointer BITF             lsr x12, x1, #0x1c */
  {0x1a1cfc, 0x92648c28, 0x92669428},  /* VirtAlloc::GetMemoryBlockFromPointer LOGIMM ~(gran-1) and x8, x1, #0xfffffffff0000000 */
  {0x1a1d04, 0xd35c9c2a, 0xd35a942a},  /* VirtAlloc::GetMemoryBlockFromPointer BITF             ubfx x10, x1, #0x1c, #0xc */
  {0x1a1d18, 0xb25c6feb, 0xb25e77eb},  /* VirtAlloc::GetMemoryBlockFromPointer MOV span         mov x11, #-0x1000000000 */
  {0x1a1d1c, 0xf2a2000b, 0xf2a0800b},  /* VirtAlloc::GetMemoryBlockFromPointer MOVZ/K gran      movk x11, #0x1000, lsl #16 */
  {0x1a1d58, 0xcb0a7108, 0xcb0a6908},  /* VirtAlloc::GetMemoryBlockFromPointer SHREG            sub x8, x8, x10, lsl #28 */
  {0x1a1d70, 0xd35cfc28, 0xd35afc28},  /* VirtAlloc::GetBlockInfoFromPointer   BITF             lsr x8, x1, #0x1c */
  {0x1a1d84, 0xd35c9c29, 0xd35a9429},  /* VirtAlloc::GetBlockInfoFromPointer   BITF             ubfx x9, x1, #0x1c, #0xc */
  {0x1a3404, 0xd35cfc28, 0xd35afc28},  /* MemMgr::GetAllocatorContainingPtr    BITF             lsr x8, x1, #0x1c */
  {0x1a3420, 0xd35c9e89, 0xd35a9689},  /* MemMgr::GetAllocatorContainingPtr    BITF             ubfx x9, x20, #0x1c, #0xc */
  {0x1a3d78, 0x32006fea, 0x12bf800a},  /* BucketAllocator::ctor                MOV gran-1       mov w10, #0xfffffff */
  {0x1a3d88, 0x92648d01, 0x92669501},  /* BucketAllocator::ctor                LOGIMM ~(gran-1) and x1, x8, #0xfffffffff0000000 */
  {0x1a5c00, 0x320403e9, 0x52a08009},  /* DynamicHeapAllocator::ctor           MOV gran         mov w9, #0x10000000 */
  {0x1a6070, 0x32006fea, 0x12bf800a},  /* DHA::RequestLargeAllocMemory         MOV gran-1       mov w10, #0xfffffff */
  {0x1a6078, 0x92648d36, 0x92669536},  /* DHA::RequestLargeAllocMemory         LOGIMM ~(gran-1) and x22, x9, #0xfffffffff0000000 */
};
#define BP_REGION_PATCH_N ((int)(sizeof(BP_REGION_PATCH)/sizeof(BP_REGION_PATCH[0])))

#endif /* REGION_PATCH_H */

/** @file
*  File managing the MMU for ARMv7 architecture
*
*  Copyright (c) 2011-2013, ARM Limited. All rights reserved.
*  Copyright (c) Microsoft Corporation. All rights reserved.
*
*  This program and the accompanying materials
*  are licensed and made available under the terms and conditions of the BSD License
*  which accompanies this distribution.  The full text of the license may be found at
*  http://opensource.org/licenses/bsd-license.php
*
*  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
*  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
*
**/

#include <Uefi.h>
#include <Chipset/ArmV7.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include "ArmV7Lib.h"
#include "ArmLibPrivate.h"

//
// BCM2711 (Raspberry Pi 4) bring-up switch.
//
// Set to 1 to enable the instruction cache on top of the MMU, which is what the
// generic ArmLib does. On Cortex-A72 running in AArch32 that stalls the core the
// moment SCTLR.I is set (see the comment in ArmConfigureMmu()), so it is left
// disabled for now: running with I=0 is functionally correct, only slower.
//
#define ENABLE_INSTRUCTION_CACHE   0

//
// Set to 1 to enable the data cache on top of the MMU.
//
// Bisecting on a Pi 4 gave:
//   DRAM mapped Write-Back  -> the contents of read-only strings start coming
//                              back modified a few debug prints into the boot,
//                              i.e. memory is really clobbered (making the
//                              mappings Inner Shareable changed nothing)
//   DRAM mapped Write-Through -> no corruption at all, the dcache self test
//                              below passes and the boot reaches PrePi's
//                              ArmPlatformGetBootMode, but then stalls (see the
//                              comment further down)
// The data cache is therefore left off for now so that console work runs on a
// known-good base; the Write-Through configuration is kept in Pi2Board.h.
//
// Later bisect with both caches enabled got much further - DecompressFirstFv
// (LZMA) returned success and the boot stalled inside LoadPeCoffImage, at the
// PeCoffLoaderLoadImage header copy into the freshly allocated image buffer
// (SizeOfHeaders was a sane 0x240, the copy simply never returned). See
// README.caches.md for the full chain, including what was ruled out.
//
#define ENABLE_DATA_CACHE          0

UINT32
ConvertSectionAttributesToPageAttributes (
  IN UINT32   SectionAttributes,
  IN BOOLEAN  IsLargePage
  )
{
  UINT32 PageAttributes;

  PageAttributes = 0;
  PageAttributes |= TT_DESCRIPTOR_CONVERT_TO_PAGE_CACHE_POLICY (SectionAttributes, IsLargePage);
  PageAttributes |= TT_DESCRIPTOR_CONVERT_TO_PAGE_AP (SectionAttributes);
  PageAttributes |= TT_DESCRIPTOR_CONVERT_TO_PAGE_XN (SectionAttributes, IsLargePage);
  PageAttributes |= TT_DESCRIPTOR_CONVERT_TO_PAGE_NG (SectionAttributes);
  PageAttributes |= TT_DESCRIPTOR_CONVERT_TO_PAGE_S (SectionAttributes);

  return PageAttributes;
}

STATIC
VOID
PopulateLevel2PageTable (
  IN UINT32                         *SectionEntry,
  IN UINT32                         PhysicalBase,
  IN UINT32                         RemainLength,
  IN ARM_MEMORY_REGION_ATTRIBUTES   Attributes
  )
{
  UINT32* PageEntry;
  UINT32  Pages;
  UINT32  Index;
  UINT32  PageAttributes;
  UINT32  SectionDescriptor;
  UINT32  TranslationTable;
  UINT32  BaseSectionAddress;

  switch (Attributes) {
    case ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK:
    case ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_WRITE_BACK:
      PageAttributes = TT_DESCRIPTOR_PAGE_WRITE_BACK;
      break;
    case ARM_MEMORY_REGION_ATTRIBUTE_WRITE_THROUGH:
    case ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_WRITE_THROUGH:
      PageAttributes = TT_DESCRIPTOR_PAGE_WRITE_THROUGH;
      break;
    case ARM_MEMORY_REGION_ATTRIBUTE_DEVICE:
    case ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_DEVICE:
      PageAttributes = TT_DESCRIPTOR_PAGE_DEVICE;
      break;
    case ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED:
    case ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_UNCACHED_UNBUFFERED:
      PageAttributes = TT_DESCRIPTOR_PAGE_UNCACHED;
      break;
    default:
      PageAttributes = TT_DESCRIPTOR_PAGE_UNCACHED;
      break;
  }

  // Check if the Section Entry has already been populated. Otherwise attach a
  // Level 2 Translation Table to it
  if (*SectionEntry != 0) {
    // The entry must be a page table. Otherwise it exists an overlapping in the memory map
    if (TT_DESCRIPTOR_SECTION_TYPE_IS_PAGE_TABLE(*SectionEntry)) {
      TranslationTable = *SectionEntry & TT_DESCRIPTOR_SECTION_PAGETABLE_ADDRESS_MASK;
    } else if ((*SectionEntry & TT_DESCRIPTOR_SECTION_TYPE_MASK) == TT_DESCRIPTOR_SECTION_TYPE_SECTION) {
      // Case where a virtual memory map descriptor overlapped a section entry

      // Allocate a Level2 Page Table for this Section
      TranslationTable = (UINTN)AllocatePages(EFI_SIZE_TO_PAGES(TRANSLATION_TABLE_PAGE_SIZE + TRANSLATION_TABLE_PAGE_ALIGNMENT));
      TranslationTable = ((UINTN)TranslationTable + TRANSLATION_TABLE_PAGE_ALIGNMENT_MASK) & ~TRANSLATION_TABLE_PAGE_ALIGNMENT_MASK;

      // Translate the Section Descriptor into Page Descriptor
      SectionDescriptor = TT_DESCRIPTOR_PAGE_TYPE_PAGE | ConvertSectionAttributesToPageAttributes (*SectionEntry, FALSE);

      BaseSectionAddress = TT_DESCRIPTOR_SECTION_BASE_ADDRESS(*SectionEntry);

      // Populate the new Level2 Page Table for the section
      PageEntry = (UINT32*)TranslationTable;
      for (Index = 0; Index < TRANSLATION_TABLE_PAGE_COUNT; Index++) {
        PageEntry[Index] = TT_DESCRIPTOR_PAGE_BASE_ADDRESS(BaseSectionAddress + (Index << 12)) | SectionDescriptor;
      }

      // Overwrite the section entry to point to the new Level2 Translation Table
      *SectionEntry = (TranslationTable & TT_DESCRIPTOR_SECTION_PAGETABLE_ADDRESS_MASK) |
          (IS_ARM_MEMORY_REGION_ATTRIBUTES_SECURE(Attributes) ? (1 << 3) : 0) |
          TT_DESCRIPTOR_SECTION_TYPE_PAGE_TABLE;
    } else {
      // We do not support the other section type (16MB Section)
      ASSERT(0);
      return;
    }
  } else {
    TranslationTable = (UINTN)AllocatePages(EFI_SIZE_TO_PAGES(TRANSLATION_TABLE_PAGE_SIZE + TRANSLATION_TABLE_PAGE_ALIGNMENT));
    TranslationTable = ((UINTN)TranslationTable + TRANSLATION_TABLE_PAGE_ALIGNMENT_MASK) & ~TRANSLATION_TABLE_PAGE_ALIGNMENT_MASK;

    ZeroMem ((VOID *)TranslationTable, TRANSLATION_TABLE_PAGE_SIZE);

    *SectionEntry = (TranslationTable & TT_DESCRIPTOR_SECTION_PAGETABLE_ADDRESS_MASK) |
        (IS_ARM_MEMORY_REGION_ATTRIBUTES_SECURE(Attributes) ? (1 << 3) : 0) |
        TT_DESCRIPTOR_SECTION_TYPE_PAGE_TABLE;
  }

  PageEntry = ((UINT32 *)(TranslationTable) + ((PhysicalBase & TT_DESCRIPTOR_PAGE_INDEX_MASK) >> TT_DESCRIPTOR_PAGE_BASE_SHIFT));
  Pages     = RemainLength / TT_DESCRIPTOR_PAGE_SIZE;

  for (Index = 0; Index < Pages; Index++) {
    *PageEntry++     =  TT_DESCRIPTOR_PAGE_BASE_ADDRESS(PhysicalBase) | PageAttributes;
    PhysicalBase += TT_DESCRIPTOR_PAGE_SIZE;
  }

}

STATIC
VOID
FillTranslationTable (
  IN  UINT32                        *TranslationTable,
  IN  ARM_MEMORY_REGION_DESCRIPTOR  *MemoryRegion
  )
{
  UINT32  *SectionEntry;
  UINT32  Attributes;
  UINT32  PhysicalBase = MemoryRegion->PhysicalBase;
  UINT32  RemainLength = MemoryRegion->Length;

  ASSERT(MemoryRegion->Length > 0);

  switch (MemoryRegion->Attributes) {
    case ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK:
      Attributes = TT_DESCRIPTOR_SECTION_WRITE_BACK(0);
      break;
    case ARM_MEMORY_REGION_ATTRIBUTE_WRITE_THROUGH:
      Attributes = TT_DESCRIPTOR_SECTION_WRITE_THROUGH(0);
      break;
    case ARM_MEMORY_REGION_ATTRIBUTE_DEVICE:
      Attributes = TT_DESCRIPTOR_SECTION_DEVICE(0);
      break;
    case ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED:
      Attributes = TT_DESCRIPTOR_SECTION_UNCACHED(0);
      break;
    case ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_WRITE_BACK:
      Attributes = TT_DESCRIPTOR_SECTION_WRITE_BACK(1);
      break;
    case ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_WRITE_THROUGH:
      Attributes = TT_DESCRIPTOR_SECTION_WRITE_THROUGH(1);
      break;
    case ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_DEVICE:
      Attributes = TT_DESCRIPTOR_SECTION_DEVICE(1);
      break;
    case ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_UNCACHED_UNBUFFERED:
      Attributes = TT_DESCRIPTOR_SECTION_UNCACHED(1);
      break;
    default:
      Attributes = TT_DESCRIPTOR_SECTION_UNCACHED(0);
      break;
  }

  // Get the first section entry for this mapping
  SectionEntry    = TRANSLATION_TABLE_ENTRY_FOR_VIRTUAL_ADDRESS(TranslationTable, MemoryRegion->VirtualBase);

  while (RemainLength != 0) {
    if (PhysicalBase % TT_DESCRIPTOR_SECTION_SIZE == 0) {
      if (RemainLength >= TT_DESCRIPTOR_SECTION_SIZE) {
        // Case: Physical address aligned on the Section Size (1MB) && the length is greater than the Section Size
        *SectionEntry++ = TT_DESCRIPTOR_SECTION_BASE_ADDRESS(PhysicalBase) | Attributes;
        PhysicalBase += TT_DESCRIPTOR_SECTION_SIZE;
      } else {
        // Case: Physical address aligned on the Section Size (1MB) && the length does not fill a section
        PopulateLevel2PageTable (SectionEntry++, PhysicalBase, RemainLength, MemoryRegion->Attributes);

        // It must be the last entry
        break;
      }
    } else {
      // Case: Physical address NOT aligned on the Section Size (1MB)
      PopulateLevel2PageTable (SectionEntry++, PhysicalBase, RemainLength, MemoryRegion->Attributes);
      // Aligned the address
      PhysicalBase = (PhysicalBase + TT_DESCRIPTOR_SECTION_SIZE) & ~(TT_DESCRIPTOR_SECTION_SIZE-1);

      // If it is the last entry
      if (RemainLength < TT_DESCRIPTOR_SECTION_SIZE) {
        break;
      }
    }
    RemainLength -= TT_DESCRIPTOR_SECTION_SIZE;
  }
}

RETURN_STATUS
EFIAPI
ArmConfigureMmu (
  IN  ARM_MEMORY_REGION_DESCRIPTOR  *MemoryTable,
  OUT VOID                         **TranslationTableBase OPTIONAL,
  OUT UINTN                         *TranslationTableSize OPTIONAL
  )
{
  VOID*                         TranslationTable;
  ARM_MEMORY_REGION_ATTRIBUTES  TranslationTableAttribute;
  UINT32                        TTBRAttributes;

  // Allocate pages for translation table.
  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: AllocatePages(%d) for translation table\n",
          (UINT32)EFI_SIZE_TO_PAGES (TRANSLATION_TABLE_SECTION_SIZE + TRANSLATION_TABLE_SECTION_ALIGNMENT)));
  TranslationTable = AllocatePages (EFI_SIZE_TO_PAGES (TRANSLATION_TABLE_SECTION_SIZE + TRANSLATION_TABLE_SECTION_ALIGNMENT));
  if (TranslationTable == NULL) {
    DEBUG ((DEBUG_INIT, "ArmConfigureMmu: AllocatePages FAILED\n"));
    return RETURN_OUT_OF_RESOURCES;
  }
  TranslationTable = (VOID*)(((UINTN)TranslationTable + TRANSLATION_TABLE_SECTION_ALIGNMENT_MASK) & ~TRANSLATION_TABLE_SECTION_ALIGNMENT_MASK);
  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: TranslationTable=0x%08x\n", (UINT32)(UINTN)TranslationTable));

  if (TranslationTableBase != NULL) {
    *TranslationTableBase = TranslationTable;
  }

  if (TranslationTableSize != NULL) {
    *TranslationTableSize = TRANSLATION_TABLE_SECTION_SIZE;
  }

  ZeroMem (TranslationTable, TRANSLATION_TABLE_SECTION_SIZE);

  // By default, mark the translation table as belonging to a uncached region
  TranslationTableAttribute = ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED;
  while (MemoryTable->Length != 0) {
    DEBUG ((DEBUG_INIT, "ArmConfigureMmu: region phys=0x%08x virt=0x%08x len=0x%08x attr=0x%x\n",
            (UINT32)MemoryTable->PhysicalBase, (UINT32)MemoryTable->VirtualBase,
            (UINT32)MemoryTable->Length, (UINT32)MemoryTable->Attributes));
    // Find the memory attribute for the Translation Table
    if (((UINTN)TranslationTable >= MemoryTable->PhysicalBase) && ((UINTN)TranslationTable <= MemoryTable->PhysicalBase - 1 + MemoryTable->Length)) {
      TranslationTableAttribute = MemoryTable->Attributes;
    }

    FillTranslationTable (TranslationTable, MemoryTable);
    MemoryTable++;
  }

  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: translation table filled\n"));

  // Translate the Memory Attributes into Translation Table Register Attributes
  if ((TranslationTableAttribute == ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED) ||
      (TranslationTableAttribute == ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_UNCACHED_UNBUFFERED)) {
    TTBRAttributes = TTBR_NON_CACHEABLE;
  } else if ((TranslationTableAttribute == ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK) ||
      (TranslationTableAttribute == ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_WRITE_BACK)) {
    TTBRAttributes = TTBR_WRITE_BACK_ALLOC;
  } else if ((TranslationTableAttribute == ARM_MEMORY_REGION_ATTRIBUTE_WRITE_THROUGH) ||
      (TranslationTableAttribute == ARM_MEMORY_REGION_ATTRIBUTE_NONSECURE_WRITE_THROUGH)) {
    TTBRAttributes = TTBR_WRITE_THROUGH_NO_ALLOC;
  } else {
    ASSERT (0); // No support has been found for the attributes of the memory region that the translation table belongs to.
    return RETURN_UNSUPPORTED;
  }

  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: TTBR attributes=0x%x, clean+invalidate caches\n", TTBRAttributes));
  ArmCleanInvalidateDataCache ();
  ArmInvalidateInstructionCache ();
  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: caches cleaned, disabling caches+MMU\n"));

  ArmDisableDataCache ();
  ArmDisableInstructionCache();
  // TLBs are also invalidated when calling ArmDisableMmu()
  ArmDisableMmu ();
  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: MMU disabled\n"));

  // Make sure nothing sneaked into the cache
  ArmCleanInvalidateDataCache ();
  ArmInvalidateInstructionCache ();
  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: caches cleaned again, setting TTBR0\n"));

  ArmSetTTBR0 ((VOID *)(UINTN)(((UINTN)TranslationTable & ~TRANSLATION_TABLE_SECTION_ALIGNMENT_MASK) | (TTBRAttributes & 0x7F)));

  //
  // Initialize TTBCR to set 32-bit translation system with the Short-descriptor translation table format,
  // and perform translation table walks using TTBR0
  //
  ArmSetTTBCR (0);

  ArmSetDomainAccessControl (DOMAIN_ACCESS_CONTROL_NONE(15) |
                             DOMAIN_ACCESS_CONTROL_NONE(14) |
                             DOMAIN_ACCESS_CONTROL_NONE(13) |
                             DOMAIN_ACCESS_CONTROL_NONE(12) |
                             DOMAIN_ACCESS_CONTROL_NONE(11) |
                             DOMAIN_ACCESS_CONTROL_NONE(10) |
                             DOMAIN_ACCESS_CONTROL_NONE( 9) |
                             DOMAIN_ACCESS_CONTROL_NONE( 8) |
                             DOMAIN_ACCESS_CONTROL_NONE( 7) |
                             DOMAIN_ACCESS_CONTROL_NONE( 6) |
                             DOMAIN_ACCESS_CONTROL_NONE( 5) |
                             DOMAIN_ACCESS_CONTROL_NONE( 4) |
                             DOMAIN_ACCESS_CONTROL_NONE( 3) |
                             DOMAIN_ACCESS_CONTROL_NONE( 2) |
                             DOMAIN_ACCESS_CONTROL_NONE( 1) |
                             DOMAIN_ACCESS_CONTROL_MANAGER(0));
  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: TTBR0/DACR set, TTBR0 now=0x%08x\n",
          (UINT32)(UINTN)ArmGetTTBR0BaseAddress ()));

  // Dump a few section descriptors so a fault right after the MMU is enabled can
  // be attributed to a specific mapping: [0] boot region, [4] system memory,
  // [0x3B3] GPU memory, [0xFE0] SoC peripherals.
  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: desc 0x000=0x%08x 0x004=0x%08x 0x3B3=0x%08x 0xFE0=0x%08x\n",
          ((UINT32*)TranslationTable)[0x000], ((UINT32*)TranslationTable)[0x004],
          ((UINT32*)TranslationTable)[0x3B3], ((UINT32*)TranslationTable)[0xFE0]));

  //
  // Enable the MMU first and the caches afterwards. This keeps every step
  // separately observable on the serial console: if the platform stops right
  // after "enabling MMU" then translation itself is broken, if it stops after
  // the instruction or data cache has been turned on then only the cache
  // attributes of the mappings are wrong.
  //
  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: SCTLR=0x%08x DACR=0x%08x TTBR0=0x%08x PRRR=0x%08x NMRR=0x%08x CLIDR=0x%08x\n",
          ArmReadSctlr (), ArmReadDacr (), ArmReadTtbr0Raw (), ArmReadPrrr (), ArmReadNmrr (), ArmReadClidr ()));

  //
  // Enable the MMU first, then the data cache, then the instruction cache, so
  // that the first cacheable access is issued at a known point and the step that
  // stalls is visible on the serial console.
  //
  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: enabling MMU (caches still off)\n"));
  ArmEnableMmu();
  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: MMU on, SCTLR=0x%08x\n", ArmReadSctlr ()));

#if ENABLE_DATA_CACHE
  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: enabling data cache\n"));
  ArmEnableDataCache();
  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: data cache on, SCTLR=0x%08x\n", ArmReadSctlr ()));

  //
  // Sanity check the newly enabled data cache: fill a buffer and read it back,
  // and print a literal so that a corrupted read-only string shows up right
  // here instead of several prints later.
  //
  {
    volatile UINT32  Scratch[64];
    UINTN            Index;
    UINT32           Failures;

    Failures = 0;
    for (Index = 0; Index < 64; Index++) {
      Scratch[Index] = 0xA5A50000 + (UINT32)Index;
    }
    for (Index = 0; Index < 64; Index++) {
      if (Scratch[Index] != (0xA5A50000 + (UINT32)Index)) {
        Failures++;
      }
    }
    DEBUG ((DEBUG_INIT, "ArmConfigureMmu: dcache test %d/64 words wrong, rodata: %a\n",
            Failures, "if this whole line is readable the cache is currently fine"));
  }
#else
  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: data cache left disabled (BCM2711 workaround)\n"));
#endif

#if ENABLE_INSTRUCTION_CACHE
  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: enabling instruction cache\n"));
  ArmEnableInstructionCache();
  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: instruction cache on, SCTLR=0x%08x\n", ArmReadSctlr ()));
#else
  //
  // BCM2711 workaround: do not enable the instruction cache.
  //
  // Measured on a Pi 4, with everything else verified correct
  // (SCTLR=0x00C50838 -> M=0 C=0 I=0 B=0 A=0 TRE=0 AFE=0 before enabling,
  //  CLIDR=0x0A200023 -> L1 I/D split + unified L2, TTBR0=0x3AEEC048,
  //  DACR=0x3 = domain 0 manager, descriptors normal WB-WA / shareable device):
  //
  //   I=1 with M=0 .......... works
  //   M=1 with I=0 .......... works (prints right after)
  //   M=1 with I=0, C=1 ..... works (prints right after)
  //   M=1 with I=1 .......... setting SCTLR.I makes the very next instruction
  //                           fetch never complete: no further output and no
  //                           exception dump
  //
  // So the MMU and the data cache are both fine and only the instruction cache
  // trips the core over. Running with I=0 is architecturally correct, it just
  // costs performance, so the port proceeds that way until the cause is found.
  //
  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: instruction cache left disabled (BCM2711 workaround)\n"));
#endif

  DEBUG ((DEBUG_INIT, "ArmConfigureMmu: done, MMU enabled=%d, UART alive after MMU\n", (UINT32)ArmMmuEnabled ()));
  return RETURN_SUCCESS;
}

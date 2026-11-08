/** @file
*
*  Copyright (c), Microsoft Corporation. All rights reserved.
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

#include <Library/ArmPlatformLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/IoLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PcdLib.h>
#include <Library/HobLib.h>

#include <Pi2Board.h>
#include <BcmMailbox.h>

#define MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS  6

volatile MAILBOX_GET_MEMORY MbGetMemory __attribute__((aligned(16)));

/**
  Return the Virtual Memory Map of your platform

  This Virtual Memory Map is used by MemoryInitPei Module to initialize the MMU on your platform.

  @param[out]   VirtualMemoryMap    Array of ARM_MEMORY_REGION_DESCRIPTOR describing a Physical-to-
                                    Virtual Memory mapping. This array must be ended by a zero-filled
                                    entry

**/
VOID
ArmPlatformGetVirtualMemoryMap (
  IN ARM_MEMORY_REGION_DESCRIPTOR** VirtualMemoryMap
  )
{
  ARM_MEMORY_REGION_ATTRIBUTES  CacheAttributes;
  UINTN                         Index = 0;
  ARM_MEMORY_REGION_DESCRIPTOR  *VirtualMemoryTable;

  ASSERT(VirtualMemoryMap != NULL);

  DEBUG((DEBUG_INIT, "Enter: ArmPlatformGetVirtualMemoryMap\n"));

  VirtualMemoryTable = (ARM_MEMORY_REGION_DESCRIPTOR*)AllocatePages(EFI_SIZE_TO_PAGES (sizeof(ARM_MEMORY_REGION_DESCRIPTOR) * MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS));
  if (VirtualMemoryTable == NULL) {
    return;
  }

  if (FeaturePcdGet(PcdCacheEnable) == TRUE) {
    CacheAttributes = DDR_ATTRIBUTES_CACHED;
  } else {
    CacheAttributes = DDR_ATTRIBUTES_UNCACHED;
  }

  DEBUG((DEBUG_INIT, "CacheAttributes=0x%d\n", CacheAttributes));

  ASSERT(FixedPcdGet32(PcdBootRegionSize) != 0);

  // Boot DRAM region (UEFI binary image & boot working DRAM)
  VirtualMemoryTable[Index].PhysicalBase   = FixedPcdGet32(PcdBootRegionBase);
  VirtualMemoryTable[Index].VirtualBase    = FixedPcdGet32(PcdBootRegionBase);
  VirtualMemoryTable[Index].Length         = FixedPcdGet32(PcdBootRegionSize);
  VirtualMemoryTable[Index].Attributes     = BOOT_IMAGE_ATTRIBUTES;

  // Base SDRAM mapped as is.
  VirtualMemoryTable[++Index].PhysicalBase = PcdGet64(PcdSystemMemoryBase);
  VirtualMemoryTable[Index].VirtualBase    = PcdGet64(PcdSystemMemoryBase);
  VirtualMemoryTable[Index].Length         = PcdGet64(PcdSystemMemorySize);
  VirtualMemoryTable[Index].Attributes     = CacheAttributes;

  //
  // PCIe root complex.
  //
  // The device tree has pcie@7d500000 as a sibling of the soc node rather than a
  // child of it, so it is *not* subject to the soc ranges translation of
  // 0x7e000000 to 0xfe000000. Its CPU physical address is 0x7D500000 itself, and
  // the 0xFD500000 used at first does not exist: every access there timed out on
  // the bus and read back zero, which looked exactly like an unclocked block.
  // Map 0x7D000000..0x7DFFFFFF as device memory, before the SoC entry because
  // regions are filled in ascending order.
  //
  VirtualMemoryTable[++Index].PhysicalBase = 0xFC000000;
  VirtualMemoryTable[Index].VirtualBase    = 0xFC000000;
  VirtualMemoryTable[Index].Length         = 0x02000000;
  VirtualMemoryTable[Index].Attributes     = SOC_REGISTERS_ATTRIBUTES;

  // SOC peripheral registers.
  VirtualMemoryTable[++Index].PhysicalBase = SOC_REGISTERS_PHYSICAL_BASE;
  VirtualMemoryTable[Index].VirtualBase    = SOC_REGISTERS_PHYSICAL_BASE;
  VirtualMemoryTable[Index].Length         = SOC_REGISTERS_PHYSICAL_LENGTH;
  VirtualMemoryTable[Index].Attributes     = SOC_REGISTERS_ATTRIBUTES;

  // ARM Core peripheral registers.
  VirtualMemoryTable[++Index].PhysicalBase = ARM_REGISTERS_PHYSICAL_BASE;
  VirtualMemoryTable[Index].VirtualBase    = ARM_REGISTERS_PHYSICAL_BASE;
  VirtualMemoryTable[Index].Length         = ARM_REGISTERS_PHYSICAL_LENGTH;
  VirtualMemoryTable[Index].Attributes     = ARM_REGISTERS_ATTRIBUTES;

  // GPU Shared memory: starts at the top of system memory
  VirtualMemoryTable[++Index].PhysicalBase = PcdGet64 (PcdGpuMemoryBase);
  VirtualMemoryTable[Index].VirtualBase    = PcdGet64 (PcdGpuMemoryBase);
  VirtualMemoryTable[Index].Length         = PcdGet64 (PcdGpuMemorySize);
  VirtualMemoryTable[Index].Attributes     = GPU_SHARED_MEMORY_ATTRIBUTES;

  // End of Table
  VirtualMemoryTable[++Index].PhysicalBase = 0;
  VirtualMemoryTable[Index].VirtualBase    = 0;
  VirtualMemoryTable[Index].Length         = 0;
  VirtualMemoryTable[Index].Attributes     = (ARM_MEMORY_REGION_ATTRIBUTES)0;

  ASSERT((Index + 1) <= MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS);

  *VirtualMemoryMap = VirtualMemoryTable;
}

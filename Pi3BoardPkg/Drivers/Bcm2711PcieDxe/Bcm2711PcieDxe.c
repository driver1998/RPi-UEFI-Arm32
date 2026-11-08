/** @file
  Bring up the BCM2711 PCIe root complex.

  The Pi 4 USB ports hang off the VL805 xHCI controller, which sits behind PCIe,
  and the boot firmware powers PCIe down again before handing control to the ARM
  (a probe showed every root complex register reading zero, with each access
  taking on the order of ten seconds, which is the AXI timeout of an unclocked
  block). So this port has to perform the initialisation itself.

  This driver is the first stage of that: enable SCB access and take the SERDES
  out of IDDQ, then report whether the block responds. Register offsets follow
  the Broadcom STB PCIe core, as used by the Linux pcie-brcmstb driver.

  Copyright (c) 2024, the EDK2 Raspberry Pi 4 port authors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <BcmMailbox.h>

//
// BCM2711 PCIe root complex: pcie@7d500000, which maps directly to 0x7D500000 (it is a sibling of the soc node, so it is not subject to the 0x7e000000 to 0xfe000000 translation).
//
#define PCIE_RC_BASE                        0xFD500000UL

#define PCIE_MISC_MISC_CTRL                 0x4008
#define  MISC_CTRL_SCB_ACCESS_EN            (1U << 12)
#define  MISC_CTRL_SCB0_SIZE_MASK           0xF8000000

#define PCIE_MISC_PCIE_STATUS               0x4068
#define  PCIE_STATUS_RDLINK_UP              (1U << 4)
#define  PCIE_STATUS_LINK_UP                (1U << 5)
#define  PCIE_STATUS_PHYLINK_UP             (1U << 6)
#define  PCIE_STATUS_DL_ACTIVE              (1U << 7)

#define PCIE_MISC_HARD_PCIE_HARD_DEBUG      0x4204
#define  HARD_DEBUG_SERDES_IDDQ             0x08000000

//
// Reset control register.
//
// bcm2711_cfg selects the generic helpers and both of them operate on this one
// register, whose offset comes from pcie_offsets[] in pcie-brcmstb.c:
//
//   [RGR1_SW_INIT_1]                 = 0x9210
//   RGR1_SW_INIT_1_INIT_GENERIC_MASK = 0x2, shift 1   (bridge software init)
//   RGR1_SW_INIT_1_PERST_MASK        = 0x1            (PERST#)
//
#define PCIE_RGR1_SW_INIT_1                 0x9210
#define  RGR1_INIT_MASK                     0x2
#define  RGR1_INIT_SHIFT                    1
#define  RGR1_PERST_MASK                    0x1

/**
  Read one root complex register.
**/
STATIC
UINT32
PcieRead (
  IN UINT32  Offset
  )
{
  return MmioRead32 (PCIE_RC_BASE + Offset);
}

/**
  Write one root complex register and read it back.
**/
STATIC
UINT32
PcieWrite (
  IN UINT32  Offset,
  IN UINT32  Value
  )
{
  MmioWrite32 (PCIE_RC_BASE + Offset, Value);
  return MmioRead32 (PCIE_RC_BASE + Offset);
}

/**
  Decode and print PCIE_STATUS.
**/
STATIC
VOID
PrintLinkStatus (
  IN CONST CHAR8  *When,
  IN UINT32       Status
  )
{
  DEBUG ((
    DEBUG_INIT,
    "Bcm2711Pcie: link (%a): rdl=%d link=%d phy=%d dl_active=%d, raw=0x%08x\n",
    When,
    (Status & PCIE_STATUS_RDLINK_UP)  ? 1 : 0,
    (Status & PCIE_STATUS_LINK_UP)    ? 1 : 0,
    (Status & PCIE_STATUS_PHYLINK_UP) ? 1 : 0,
    (Status & PCIE_STATUS_DL_ACTIVE)  ? 1 : 0,
    Status));
}

/**
  Driver entry point.

  Deliberately minimal: touch only the two registers that gate access to the rest
  of the block, then report what came back. If the block is still powered down
  every access here also takes the AXI timeout, which is itself the signal that
  the next step is to get the PCIe clock enabled from the firmware side.

**/
EFI_STATUS
EFIAPI
Bcm2711PcieInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32  HardDebug;
  UINT32  MiscCtrl;
  UINT32  Status;
  UINT32  Reset;

  DEBUG ((DEBUG_INIT, "Bcm2711Pcie: bring-up starting at 0x%08x\n", (UINT32)PCIE_RC_BASE));

  HardDebug = PcieRead (PCIE_MISC_HARD_PCIE_HARD_DEBUG);
  MiscCtrl  = PcieRead (PCIE_MISC_MISC_CTRL);
  Status    = PcieRead (PCIE_MISC_PCIE_STATUS);
  PrintLinkStatus ("before", Status);
  DEBUG ((
    DEBUG_INIT,
    "Bcm2711Pcie: before: hard_debug=0x%08x misc_ctrl=0x%08x\n",
    HardDebug, MiscCtrl));

  if ((HardDebug == 0) && (MiscCtrl == 0) && (Status == 0)) {
    //
    // Note it and keep going. Writes to a block in this state may time out, and
    // that is acceptable here: releasing the bridge reset and PERST# is exactly
    // what might bring it out of this state, so bailing out first would make the
    // reset sequence unreachable.
    //
    DEBUG ((
      DEBUG_WARN,
      "Bcm2711Pcie: all three registers read zero, trying the reset sequence anyway\n"));
  }

  //
  // Mirror the start of brcm_pcie_setup(): hold the bridge in reset, assert
  // PERST#, wait, release the bridge, then take the SERDES out of IDDQ.
  //
  // Both reset controls live in PCIE_RGR1_SW_INIT_1: bit 1 is the bridge
  // software init and bit 0 is PERST#.
  //
  Reset = PcieRead (PCIE_RGR1_SW_INIT_1);
  DEBUG ((DEBUG_INIT, "Bcm2711Pcie: rgr1 before=0x%08x\n", Reset));

  Reset &= ~RGR1_INIT_MASK;
  Reset |= (1U << RGR1_INIT_SHIFT) & RGR1_INIT_MASK;     // hold bridge in reset
  Reset = PcieWrite (PCIE_RGR1_SW_INIT_1, Reset);

  Reset |= RGR1_PERST_MASK;                              // assert PERST#
  Reset = PcieWrite (PCIE_RGR1_SW_INIT_1, Reset);
  gBS->Stall (200);

  Reset &= ~RGR1_INIT_MASK;                              // release the bridge
  Reset = PcieWrite (PCIE_RGR1_SW_INIT_1, Reset);
  DEBUG ((DEBUG_INIT, "Bcm2711Pcie: rgr1 after=0x%08x (perst asserted, bridge released)\n", Reset));

  //
  // Take the SERDES out of IDDQ so the PHY can start its clocks.
  //
  HardDebug &= ~HARD_DEBUG_SERDES_IDDQ;
  HardDebug = PcieWrite (PCIE_MISC_HARD_PCIE_HARD_DEBUG, HardDebug);
  gBS->Stall (200);

  //
  // Enable SCB access, which is what makes the config space windows usable, and
  // report the size field so the next stage knows what it is working with.
  //
  MiscCtrl |= MISC_CTRL_SCB_ACCESS_EN;
  MiscCtrl = PcieWrite (PCIE_MISC_MISC_CTRL, MiscCtrl);

  DEBUG ((
    DEBUG_INIT,
    "Bcm2711Pcie: after: hard_debug=0x%08x misc_ctrl=0x%08x (scb_en=%d, scb0_size=0x%x)\n",
    HardDebug, MiscCtrl,
    (MiscCtrl & MISC_CTRL_SCB_ACCESS_EN) ? 1 : 0,
    (MiscCtrl & MISC_CTRL_SCB0_SIZE_MASK) >> 27));

  Status = PcieRead (PCIE_MISC_PCIE_STATUS);
  PrintLinkStatus ("after", Status);

  //
  // All ones is not a link, it is an undecoded address: an interconnect answers a
  // read from an address that no device claims with 0xFFFFFFFF, and it does so
  // quickly. A root complex that exists but is powered down answers with zeros
  // after the AXI timeout instead. Check this explicitly, otherwise every bit of
  // PCIE_STATUS reads as set and the driver would claim the data link is active.
  //
  if (Status == 0xFFFFFFFF) {
    DEBUG ((
      DEBUG_ERROR,
      "Bcm2711Pcie: PCIE_STATUS reads all ones, so no device is decoded at "
      "0x%08x; the base address is wrong\n",
      (UINT32)PCIE_RC_BASE));
    return EFI_NO_MAPPING;
  }

  if ((Status & PCIE_STATUS_DL_ACTIVE) != 0) {
    DEBUG ((DEBUG_INIT, "Bcm2711Pcie: data link is active\n"));
    return EFI_SUCCESS;
  }

  if ((HardDebug == 0) && (MiscCtrl == 0)) {
    DEBUG ((DEBUG_ERROR, "Bcm2711Pcie: writes did not stick, the block is still unclocked\n"));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((DEBUG_WARN, "Bcm2711Pcie: registers respond but the link is not trained yet\n"));
  return EFI_NOT_READY;
}

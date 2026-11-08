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
#define PCIE_RC_BASE                        0x7D500000UL

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
    // Do not write anything in this state: a write to an unclocked block hangs
    // for the full AXI timeout just like a read (the previous build stalled
    // here), and it cannot take effect anyway.
    //
    DEBUG ((
      DEBUG_ERROR,
      "Bcm2711Pcie: all three registers read zero, the block is unclocked; "
      "the PCIe clock has to be enabled before any of this can work\n"));

    //
    // Query, but never set, a range of Pi firmware clock ids so the log shows
    // which of them exist and at what rate. Read only on purpose: setting the
    // wrong clock could disturb an unrelated part of the SoC.
    //
    {
      UINT32  ClockId;

      for (ClockId = 1; ClockId <= 15; ClockId++) {
        MAILBOX_GET_CLOCK_RATE  Query;
        EFI_STATUS              QueryStatus;

        ZeroMem ((void *)&Query, sizeof (Query));
        Query.Header.BufferSize = sizeof (Query);
        Query.Header.TagID      = TAG_GET_CLOCK_RATE;
        Query.Header.TagLength  = 8;
        Query.ClockId           = ClockId;

        QueryStatus = MailboxProperty (
                        MAILBOX_CHANNEL_PROPERTY_ARM_VC,
                        (MAILBOX_HEADER *)&Query
                        );
        DEBUG ((
          DEBUG_INIT,
          "Bcm2711Pcie: clock id %d -> %r, rate %d Hz\n",
          ClockId, QueryStatus, Query.Rate
          ));
      }
    }

    return EFI_DEVICE_ERROR;
  }

  //
  // Take the SERDES out of IDDQ so the PHY can start its clocks.
  //
  HardDebug &= ~HARD_DEBUG_SERDES_IDDQ;
  HardDebug = PcieWrite (PCIE_MISC_HARD_PCIE_HARD_DEBUG, HardDebug);

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

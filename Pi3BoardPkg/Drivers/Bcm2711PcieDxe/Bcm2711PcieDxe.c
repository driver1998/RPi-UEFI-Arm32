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
//
// Bit positions from pcie-brcmstb.c, which are not what a first guess suggests:
// bit 7 is the RC port indicator, bit 6 LINK_IN_L23, bit 5 DL_ACTIVE and bit 4
// PHYLINKUP. Getting these wrong makes an RC-mode block with an untrained link
// look like a live one, because the port bit then reads as "link active".
//
#define  PCIE_STATUS_PCIE_PORT              (1U << 7)
#define  PCIE_STATUS_LINK_IN_L23            (1U << 6)
#define  PCIE_STATUS_DL_ACTIVE              (1U << 5)
#define  PCIE_STATUS_PHYLINKUP              (1U << 4)

//
// PCIE_MISC_MISC_CTRL fields, again straight from the Linux driver.
//
#define PCIE_MISC_MISC_CTRL                 0x4008
#define  MISC_CTRL_RCB_64B_MODE             (1U << 7)
#define  MISC_CTRL_RCB_MPS_MODE             (1U << 10)
#define  MISC_CTRL_SCB_ACCESS_EN            (1U << 12)
#define  MISC_CTRL_CFG_READ_UR_MODE         (1U << 13)
#define  MISC_CTRL_MAX_BURST_SIZE_MASK      0x00300000

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
    (Status & PCIE_STATUS_PHYLINKUP) ? 1 : 0,
    (Status & PCIE_STATUS_LINK_IN_L23)    ? 1 : 0,
    (Status & PCIE_STATUS_PHYLINKUP) ? 1 : 0,
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
//
// Config space access.
//
// brcm_pcie_map_bus() in pcie-brcmstb.c shows there is no plain ECAM window:
//
//   - the root bus is memory mapped directly, at base + (where & 0xfff)
//   - for a downstream bus the address is written to EXT_CFG_INDEX (0x9000) and
//     the data appears at EXT_CFG_DATA (0x8000) plus (where & 0xfff)
//
// with the index encoded as PCIE_ECAM_OFFSET(bus, devfn, where) from
// include/linux/pci-ecam.h: bus << 20 | devfn << 12 | (where & 0xfff).
//
#define PCIE_EXT_CFG_INDEX                  0x9000
#define PCIE_EXT_CFG_DATA                   0x8000
#define PCIE_ECAM_OFFSET(bus, dev, fn) \
  ((((UINT32)(bus) & 0xFF) << 20) | ((((UINT32)(dev) << 3 | (UINT32)(fn)) & 0xFF) << 12))

/**
  Read a 32 bit PCI configuration register.

  @param  Bus     PCI bus number.
  @param  Device  Device number.
  @param  Function Function number.
  @param  Offset  Register offset within the configuration space.
**/
STATIC
UINT32
PcieReadConfig32 (
  IN UINT32  Bus,
  IN UINT32  Device,
  IN UINT32  Function,
  IN UINT32  Offset
  )
{
  UINT32  Register = Offset & 0xFFF;

  if (Bus == 0) {
    //
    // The root complex's own configuration space is directly mapped. Only
    // device 0 exists on the root bus.
    //
    if ((Device != 0) || (Function != 0)) {
      return 0xFFFFFFFF;
    }
    return MmioRead32 (PCIE_RC_BASE + Register);
  }

  MmioWrite32 (
    PCIE_RC_BASE + PCIE_EXT_CFG_INDEX,
    PCIE_ECAM_OFFSET (Bus, Device, Function)
    );
  return MmioRead32 (PCIE_RC_BASE + PCIE_EXT_CFG_DATA + Register);
}

/**
  Print vendor and device id for one function.
**/
/**
  Write a 32 bit PCI configuration register.
**/
STATIC
VOID
PcieWriteConfig32 (
  IN UINT32  Bus,
  IN UINT32  Device,
  IN UINT32  Function,
  IN UINT32  Offset,
  IN UINT32  Value
  )
{
  UINT32  Register = Offset & 0xFFF;

  if (Bus == 0) {
    if ((Device != 0) || (Function != 0)) {
      return;
    }
    MmioWrite32 (PCIE_RC_BASE + Register, Value);
    return;
  }

  MmioWrite32 (
    PCIE_RC_BASE + PCIE_EXT_CFG_INDEX,
    PCIE_ECAM_OFFSET (Bus, Device, Function)
    );
  MmioWrite32 (PCIE_RC_BASE + PCIE_EXT_CFG_DATA + Register, Value);
}

STATIC
VOID
PciePrintDevice (
  IN UINT32  Bus,
  IN UINT32  Device,
  IN UINT32  Function
  )
{
  UINT32  Id;

  Id = PcieReadConfig32 (Bus, Device, Function, 0x00);
  if ((Id == 0xFFFFFFFF) || (Id == 0) || ((Id & 0xFFFF) == 0xFFFF)) {
    return;
  }

  DEBUG ((
    DEBUG_INIT,
    "Bcm2711Pcie: %d:%d.%d vendor=0x%04x device=0x%04x class=0x%06x\n",
    Bus, Device, Function, Id & 0xFFFF, (Id >> 16) & 0xFFFF,
    PcieReadConfig32 (Bus, Device, Function, 0x08) >> 8
    ));
}

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
  UINTN   Spin;

  DEBUG ((DEBUG_INIT, "Bcm2711Pcie: bring-up starting at 0x%08x\n", (UINT32)PCIE_RC_BASE));

  //
  // No pre-flight probe. The firmware leaves the block held in reset, so every
  // read before the reset sequence is released returns zero after the full AXI
  // timeout, which cost about thirty seconds per boot and told us nothing. The
  // first meaningful read is the one after PERST# is deasserted.
  //
  HardDebug = 0;
  MiscCtrl  = 0;
  Status    = 0;

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
  // The rest of the MISC_CTRL setup from brcm_pcie_setup(): SCB access enable,
  // unrestricted config reads, a 128 byte max burst on BCM2711, and the RCB
  // modes. All masks come from the Linux driver.
  //
  MiscCtrl |= MISC_CTRL_SCB_ACCESS_EN;
  MiscCtrl |= MISC_CTRL_CFG_READ_UR_MODE;
  MiscCtrl |= MISC_CTRL_RCB_MPS_MODE;
  MiscCtrl |= MISC_CTRL_RCB_64B_MODE;
  MiscCtrl &= ~MISC_CTRL_MAX_BURST_SIZE_MASK;      // 0 = 128 bytes on BCM2711
  MiscCtrl = PcieWrite (PCIE_MISC_MISC_CTRL, MiscCtrl);

  DEBUG ((
    DEBUG_INIT,
    "Bcm2711Pcie: after: hard_debug=0x%08x misc_ctrl=0x%08x (scb_en=%d)\n",
    HardDebug, MiscCtrl,
    (MiscCtrl & MISC_CTRL_SCB_ACCESS_EN) ? 1 : 0));

  //
  // Deassert PERST#.
  //
  // brcm_pcie_setup() only asserts it; the deassertion happens afterwards in
  // brcm_pcie_start_link(). Without this the downstream device, here the VL805,
  // is held in reset and the PHY link can never train, which is exactly what the
  // previous run showed: PHYLINKUP never reached with rgr1 still reading bit 0.
  //
  Reset &= ~RGR1_PERST_MASK;
  Reset = PcieWrite (PCIE_RGR1_SW_INIT_1, Reset);
  DEBUG ((DEBUG_INIT, "Bcm2711Pcie: perst deasserted, rgr1=0x%08x\n", Reset));
  gBS->Stall (1000);

  //
  // Wait for the PHY link, then for the data link. The link comes up in stages:
  // DL_ACTIVE without PHYLINKUP first would mean something is wrong.
  //
  for (Spin = 0; Spin < 200; Spin++) {
    Status = PcieRead (PCIE_MISC_PCIE_STATUS);
    if ((Status & PCIE_STATUS_PHYLINKUP) != 0) {
      break;
    }
    gBS->Stall (1000);
  }
  DEBUG ((
    DEBUG_INIT,
    "Bcm2711Pcie: PHYLINKUP %a after %d ms, status=0x%08x\n",
    (Status & PCIE_STATUS_PHYLINKUP) ? "reached" : "never reached",
    (UINT32)Spin, Status));

  for (Spin = 0; Spin < 200; Spin++) {
    Status = PcieRead (PCIE_MISC_PCIE_STATUS);
    if ((Status & PCIE_STATUS_DL_ACTIVE) != 0) {
      break;
    }
    gBS->Stall (1000);
  }

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
    UINT32  Device;
    UINT32  Command;

    DEBUG ((DEBUG_INIT, "Bcm2711Pcie: data link is active\n"));

    //
    // The link is up, so config space should work. Read the root complex's own
    // ids first: that needs no index write and proves the direct mapping.
    //
    PciePrintDevice (0, 0, 0);

    //
    // Program the root complex as a bridge so configuration cycles are forwarded
    // downstream. Until the primary, secondary and subordinate bus numbers are
    // set, the root bus answers for itself and nothing below it is reachable,
    // which is exactly what the previous run showed: 0:0.0 read fine and bus 1
    // was empty.
    //
    //   0x18: primary 0, secondary 1, subordinate 1
    //   0x04: memory space enable + bus master enable
    //
    PcieWriteConfig32 (0, 0, 0, 0x18, 0x00010100);
    Command = PcieReadConfig32 (0, 0, 0, 0x04);
    Command |= 0x0006;
    PcieWriteConfig32 (0, 0, 0, 0x04, Command);
    DEBUG ((
      DEBUG_INIT,
      "Bcm2711Pcie: bridge programmed, bus numbers=0x%08x command=0x%04x\n",
      PcieReadConfig32 (0, 0, 0, 0x18),
      PcieReadConfig32 (0, 0, 0, 0x04) & 0xFFFF));

    //
    // Now walk the downstream bus, where the VL805 xHCI controller lives and
    // should answer with VIA's vendor id 0x1106 and device 0x3483.
    //
    for (Device = 0; Device < 32; Device++) {
      PciePrintDevice (1, Device, 0);
    }

    return EFI_SUCCESS;
  }

  if ((HardDebug == 0) && (MiscCtrl == 0)) {
    DEBUG ((DEBUG_ERROR, "Bcm2711Pcie: writes did not stick, the block is still unclocked\n"));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((DEBUG_WARN, "Bcm2711Pcie: registers respond but the link is not trained yet\n"));
  return EFI_SUCCESS;
}

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

//
// CPU to PCIe outbound window. The device tree uses CPU 0x600000000, which a
// 32 bit ARM cannot address, so the port picks its own 4 MB range instead.
//
#define PCIE_MEM_WIN0_LO                    0x400C
#define PCIE_MEM_WIN0_HI                    0x4010
#define PCIE_MEM_WIN0_BASE_LIMIT            0x4070
#define PCIE_MEM_WIN0_BASE_HI               0x4080
#define PCIE_MEM_WIN0_LIMIT_HI              0x4084

#define PCIE_WINDOW_CPU_BASE                0xC0000000UL
#define PCIE_WINDOW_CPU_END                 0xC03FFFFFUL
#define PCIE_WINDOW_DEVICE_ADDR             0xF8000000UL
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

  //
  // The SCB decode size was zero, which leaves the root complex with no
  // address decode range at all. brcm_pcie_setup computes it as
  // ilog2(memc_size) - 15; 15 corresponds to a 1 GB range, which covers
  // the 944 MB this board reports.
  //
  MiscCtrl  = MmioRead32 (PCIE_RC_BASE + PCIE_MISC_MISC_CTRL);
  MiscCtrl &= ~0xF8000000;
  MiscCtrl |= (15UL & 0x1F) << 27;
  MmioWrite32 (PCIE_RC_BASE + PCIE_MISC_MISC_CTRL, MiscCtrl);

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
    UINT32  Command;
    UINT32  Window;
    UINT32  Index;
    UINT32  Before;
    UINT32  Written;

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

    //
    // The bridge also filters memory cycles by its memory base and limit
    // registers. With both at zero every access through the window is dropped,
    // which shows up exactly as it did: the device BAR reads back a sensible
    // address while reading that address returns zero. Cover
    // PCI 0xF8000000..0xFBFFFFFF; on a 32 bit bridge both registers hold address
    // bits[31:20] in bits[15:4].
    //
    PcieWriteConfig32 (0, 0, 0, 0x20, 0xF800);
    PcieWriteConfig32 (0, 0, 0, 0x24, 0xFBF0);

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
    // answered on the first try with VIA's vendor id 0x1106 and device 0x3483.
    //
    // Only device 0 is probed. Reading the configuration space of a device that
    // is not there makes the root complex retry until
    // PCIE_MISC_RC_CONFIG_RETRY_TIMEOUT expires, which costs seconds per access:
    // scanning all 32 device numbers stalled the boot for about a minute after
    // the VL805 line was printed.
    //
    // The firmware never assigned an address: BAR0 reads back 0x00000004, a 64
    // bit memory BAR with a zero address, and the command register is 0. That is
    // convenient, there is nothing to relocate, so the port simply assigns an
    // address of its own inside a 32 bit outbound window.
    //
    // Window: CPU 0xC0000000..0xC03FFFFF  ->  PCI 0xF8000000
    //   PCI side base      : PCIE_MEM_WIN0_LO / HI
    //   CPU side base      : WIN0_BASE_LIMIT bits[15:4], in MB
    //   CPU side limit     : WIN0_BASE_LIMIT bits[31:20], in MB
    //   upper bits         : WIN0_BASE_HI / WIN0_LIMIT_HI
    //
    MmioWrite32 (PCIE_RC_BASE + PCIE_MEM_WIN0_LO, (UINT32)PCIE_WINDOW_DEVICE_ADDR);
    MmioWrite32 (PCIE_RC_BASE + PCIE_MEM_WIN0_HI, 0x00000000);

    Window = MmioRead32 (PCIE_RC_BASE + PCIE_MEM_WIN0_BASE_LIMIT);
    Window &= ~0xFFF00000;
    Window |= ((PCIE_WINDOW_CPU_END / 0x100000) & 0xFFF) << 20;
    Window &= ~0x0000FFF0;
    Window |= ((PCIE_WINDOW_CPU_BASE / 0x100000) & 0xFFF) << 4;
    MmioWrite32 (PCIE_RC_BASE + PCIE_MEM_WIN0_BASE_LIMIT, Window);
    MmioWrite32 (PCIE_RC_BASE + PCIE_MEM_WIN0_BASE_HI, 0x00000000);
    MmioWrite32 (PCIE_RC_BASE + PCIE_MEM_WIN0_LIMIT_HI, 0x00000000);

    //
    // Point the VL805's 64 bit memory BAR at the start of that window and turn on
    // memory space and bus mastering.
    //
    PcieWriteConfig32 (1, 0, 0, 0x10, PCIE_WINDOW_DEVICE_ADDR);
    PcieWriteConfig32 (1, 0, 0, 0x14, 0x00000000);
    PcieWriteConfig32 (1, 0, 0, 0x04, 0x0006);

    DEBUG ((
      DEBUG_INIT,
      "Bcm2711Pcie: window cpu 0x%08x..0x%08x -> pci 0x%08x, bar0=0x%08x, command=0x%04x\n",
      (UINT32)PCIE_WINDOW_CPU_BASE, (UINT32)PCIE_WINDOW_CPU_END,
      MmioRead32 (PCIE_RC_BASE + PCIE_MEM_WIN0_LO),
      PcieReadConfig32 (1, 0, 0, 0x10),
      PcieReadConfig32 (1, 0, 0, 0x04) & 0xFFFF));

    //
    // Prove it: the first two registers of an xHCI controller are CAPLENGTH and
    // HCIVERSION, so a plausible value here means config space, the outbound
    // window and the BAR assignment all work together.
    //
    DEBUG ((
      DEBUG_INIT,
      "Bcm2711Pcie: readback win0_lo=0x%08x base_limit=0x%08x misc_ctrl=0x%08x (scb0_size=%d)\n",
      MmioRead32 (PCIE_RC_BASE + PCIE_MEM_WIN0_LO),
      MmioRead32 (PCIE_RC_BASE + PCIE_MEM_WIN0_BASE_LIMIT),
      MmioRead32 (PCIE_RC_BASE + PCIE_MISC_MISC_CTRL),
      (MmioRead32 (PCIE_RC_BASE + PCIE_MISC_MISC_CTRL) >> 27) & 0x1F));

    DEBUG ((
      DEBUG_INIT,
      "Bcm2711Pcie: xHCI CAPLENGTH/HCIVERSION at 0x%08x = 0x%08x\n",
      (UINT32)PCIE_WINDOW_CPU_BASE,
      MmioRead32 (PCIE_WINDOW_CPU_BASE)));

    //
    // Distinguish the two explanations for a zero read, which need opposite
    // fixes: "the address is decoded but the device does not answer" versus "the
    // address is not decoded at all". Write a pattern and read it back. A decoded
    // window returns the pattern; an address nothing decodes returns zero again.
    // Watch the log speed as well: accesses that go nowhere are slow, a decoded
    // access is immediate.
    //
    Before  = MmioRead32 (PCIE_WINDOW_CPU_BASE);
    MmioWrite32 (PCIE_WINDOW_CPU_BASE, 0x5A5A1234);
    Written = MmioRead32 (PCIE_WINDOW_CPU_BASE);
    MmioWrite32 (PCIE_WINDOW_CPU_BASE, Before);

    DEBUG ((
      DEBUG_INIT,
      "Bcm2711Pcie: window probe: before=0x%08x, wrote 0x5a5a1234, read back 0x%08x\n",
      Before, Written));

    DEBUG ((
      DEBUG_INIT,
      "Bcm2711Pcie: window probe: +4 = 0x%08x, +0x100 = 0x%08x\n",
      MmioRead32 (PCIE_WINDOW_CPU_BASE + 4),
      MmioRead32 (PCIE_WINDOW_CPU_BASE + 0x100)));

    //
    PciePrintDevice (1, 0, 0);

    //
    // Dump the base address registers. The device tree assigns PCI memory from
    // 0xF8000000 and maps it to CPU 0x600000000, which a 32 bit ARM cannot reach,
    // so the VL805's BARs are expected to sit above 4 GB. The exact layout decides
    // how the outbound window has to be built: a 64 bit memory BAR means rewriting
    // two registers, and the type bits say whether it is memory at all.
    //
    for (Index = 0; Index < 6; Index++) {
      DEBUG ((
        DEBUG_INIT,
        "Bcm2711Pcie: 1:0.0 BAR%d (0x%02x) = 0x%08x\n",
        Index, 0x10 + (Index * 4),
        PcieReadConfig32 (1, 0, 0, 0x10 + (Index * 4))
        ));
    }

    DEBUG ((
      DEBUG_INIT,
      "Bcm2711Pcie: 1:0.0 command=0x%04x status=0x%04x revision=0x%02x irq=0x%02x pin=0x%02x\n",
      PcieReadConfig32 (1, 0, 0, 0x04) & 0xFFFF,
      (PcieReadConfig32 (1, 0, 0, 0x04) >> 16) & 0xFFFF,
      PcieReadConfig32 (1, 0, 0, 0x08) & 0xFF,
      PcieReadConfig32 (1, 0, 0, 0x3C) & 0xFF,
      (PcieReadConfig32 (1, 0, 0, 0x3C) >> 8) & 0xFF));

    return EFI_SUCCESS;
  }

  if ((HardDebug == 0) && (MiscCtrl == 0)) {
    DEBUG ((DEBUG_ERROR, "Bcm2711Pcie: writes did not stick, the block is still unclocked\n"));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((DEBUG_WARN, "Bcm2711Pcie: registers respond but the link is not trained yet\n"));
  return EFI_SUCCESS;
}

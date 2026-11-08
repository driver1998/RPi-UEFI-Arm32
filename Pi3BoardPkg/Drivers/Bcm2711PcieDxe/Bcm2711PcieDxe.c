/** @file
  Dump the BCM2711 PCIe root complex state left behind by the Pi boot firmware.

  This driver deliberately does nothing but read registers. The Pi 4 boot
  firmware has to bring up PCIe itself to load the VL805 USB controller's
  firmware from EEPROM, so by the time the ARM starts the root complex may
  already be trained and its outbound windows programmed. If that is the case the
  port can reuse that configuration instead of reimplementing link training and
  window setup, which is a large amount of BCM2711 specific work.

  Register offsets follow the Broadcom STB PCIe core as used by the Linux
  pcie-brcmstb driver.

  Copyright (c) 2024, the EDK2 Raspberry Pi 4 port authors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Uefi.h>

#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

//
// BCM2711 PCIe root complex. Device tree node is pcie@7d500000, which is
// 0xFD500000 in the ARM physical (high peripheral) window.
//
#define PCIE_RC_BASE                  0xFD500000UL

#define PCIE_MISC_MISC_CTRL           0x4008
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO   0x400C
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI   0x4010
#define PCIE_MISC_RC_BAR1_CONFIG_LO   0x402C
#define PCIE_MISC_RC_BAR1_CONFIG_HI   0x4030
#define PCIE_MISC_RC_BAR2_CONFIG_LO   0x4034
#define PCIE_MISC_RC_BAR2_CONFIG_HI   0x4038
#define PCIE_MISC_RC_BAR3_CONFIG_LO   0x403C
#define PCIE_MISC_RC_BAR3_CONFIG_HI   0x4040
#define PCIE_MISC_PCIE_STATUS         0x4068
#define PCIE_MISC_UBUS_BAR1_REMAP     0x406C
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT 0x4070
#define PCIE_MISC_UBUS_BAR2_REMAP     0x4074
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI    0x4080
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI   0x4084
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG 0x4204

//
// PCIE_MISC_PCIE_STATUS bits (Broadcom STB PCIe core)
//
#define PCIE_STATUS_RDLINK_UP         (1U << 4)
#define PCIE_STATUS_LINK_UP           (1U << 5)
#define PCIE_STATUS_PHYLINK_UP        (1U << 6)
#define PCIE_STATUS_DL_ACTIVE         (1U << 7)

/**
  Print one register with its name.
**/
STATIC
VOID
DumpReg (
  IN CONST CHAR8  *Name,
  IN UINT32       Offset
  )
{
  DEBUG ((
    DEBUG_INIT,
    "Bcm2711Pcie: %-34a [0x%04x] = 0x%08x\n",
    Name, Offset, MmioRead32 (PCIE_RC_BASE + Offset)));
}

/**
  Driver entry point: report the root complex state and whether the link is up.
**/
EFI_STATUS
EFIAPI
Bcm2711PcieInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32  Status;
  UINT32  MiscCtrl;
  UINT32  Win0Lo;

  DEBUG ((DEBUG_INIT, "Bcm2711Pcie: probing the PCIe root complex at 0x%08x\n", (UINT32)PCIE_RC_BASE));

  DumpReg ("HARD_PCIE_HARD_DEBUG", PCIE_MISC_HARD_PCIE_HARD_DEBUG);
  DumpReg ("MISC_CTRL", PCIE_MISC_MISC_CTRL);
  DumpReg ("PCIE_STATUS", PCIE_MISC_PCIE_STATUS);
  DumpReg ("RC_BAR1_CONFIG_LO", PCIE_MISC_RC_BAR1_CONFIG_LO);
  DumpReg ("RC_BAR1_CONFIG_HI", PCIE_MISC_RC_BAR1_CONFIG_HI);
  DumpReg ("RC_BAR2_CONFIG_LO", PCIE_MISC_RC_BAR2_CONFIG_LO);
  DumpReg ("RC_BAR2_CONFIG_HI", PCIE_MISC_RC_BAR2_CONFIG_HI);
  DumpReg ("RC_BAR3_CONFIG_LO", PCIE_MISC_RC_BAR3_CONFIG_LO);
  DumpReg ("RC_BAR3_CONFIG_HI", PCIE_MISC_RC_BAR3_CONFIG_HI);
  DumpReg ("UBUS_BAR1_REMAP", PCIE_MISC_UBUS_BAR1_REMAP);
  DumpReg ("UBUS_BAR2_REMAP", PCIE_MISC_UBUS_BAR2_REMAP);
  DumpReg ("CPU_2_PCIE_WIN0_LO", PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO);
  DumpReg ("CPU_2_PCIE_WIN0_HI", PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI);
  DumpReg ("WIN0_BASE_LIMIT", PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT);
  DumpReg ("WIN0_BASE_HI", PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI);
  DumpReg ("WIN0_LIMIT_HI", PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI);

  Status  = MmioRead32 (PCIE_RC_BASE + PCIE_MISC_PCIE_STATUS);
  MiscCtrl = MmioRead32 (PCIE_RC_BASE + PCIE_MISC_MISC_CTRL);
  Win0Lo  = MmioRead32 (PCIE_RC_BASE + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO);

  DEBUG ((
    DEBUG_INIT,
    "Bcm2711Pcie: link: rdl=%d link=%d phy=%d dl_active=%d, scb_access_en=%d, win0_lo=0x%08x\n",
    (Status & PCIE_STATUS_RDLINK_UP)  ? 1 : 0,
    (Status & PCIE_STATUS_LINK_UP)    ? 1 : 0,
    (Status & PCIE_STATUS_PHYLINK_UP) ? 1 : 0,
    (Status & PCIE_STATUS_DL_ACTIVE)  ? 1 : 0,
    (MiscCtrl & 0x1) ? 1 : 0,
    Win0Lo));

  if ((Status & PCIE_STATUS_DL_ACTIVE) == 0) {
    DEBUG ((DEBUG_ERROR, "Bcm2711Pcie: link is not up, the firmware did not leave PCIe running\n"));
    return EFI_NOT_FOUND;
  }

  DEBUG ((DEBUG_INIT, "Bcm2711Pcie: link is up, the firmware configuration can be reused\n"));
  return EFI_SUCCESS;
}

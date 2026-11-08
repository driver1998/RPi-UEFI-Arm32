# PCIe bring-up on BCM2711 (Raspberry Pi 4), ARM32 EDK2 port

Status: **blocked on enabling the PCIe clock.** Everything below is what has been
established on hardware, so the next attempt starts from facts rather than
assumptions.

The goal is USB: the four USB-A ports are behind the VL805 xHCI controller, which
is on PCIe, so keyboard input and USB storage both need the root complex running.

## Facts established on the board

1. The register block lives at `0xFD500000` (`pcie@7d500000` in the DT, high
   peripheral alias). It was not mapped by this port at all; the very first read
   took a translation fault. `Pi2BoardMem.c` now maps
   `0xFC000000..0xFDFFFFFF` as device memory, before the SoC region because the
   short descriptor fill walks the table in order.

2. With the mapping in place, every root complex register reads `0x00000000` and
   **each access costs about ten seconds**. That is the AXI timeout of an
   unclocked block, and it applies to writes as well: a build that tried to clear
   `SERDES_IDDQ` stalled in the write.

3. Writes to an unclocked block cannot take effect, so the driver now skips them
   entirely when the three probe registers read zero.

4. **There is no PCIe entry in the Pi firmware mailbox clock list.** Querying clock
   ids 1..15 with the property interface returns:

   | id | rate | clock |
   |----|------|-------|
   | 1 | 250 MHz | EMMC |
   | 2 | 48 MHz | UART |
   | 3 | 1.8 GHz | ARM |
   | 4 | 500 MHz | CORE |
   | 5 | 500 MHz | V3D |
   | 6 | 250 MHz | H264 |
   | 7 | 250 MHz | ISP |
   | 8 | 400 MHz | SDRAM |
   | 9, 10, 13, 15 | 0 | unused |
   | 11 | 250 MHz | HEVC |
   | 12 | 100 MHz | EMMC2 |
   | 14 | 75 MHz | GENET |

   So the PCIe clock cannot be requested the way the UART and eMMC2 clocks were.

## Consequence and next step

The BCM2711 clock manager sits at `0xFE101000` and is directly accessible from the
ARM, so the PCIe clock has to be enabled by writing the CM registers rather than
going through the mailbox.

**Do not guess the `CM_PCIE` register offset.** Writing the wrong clock register
can disturb an unrelated part of the SoC. Get the offset from the BCM2711 ARM
Peripherals datasheet clock manager section, or from the Linux `clk-bcm2835`
driver's clock table for BCM2711, before touching it.

Once the clock is on, the probe should complete instantly with non-zero values
instead of taking ten seconds per read; that is the signal to watch.

## Remaining work after the clock

1. SERDES out of IDDQ, then the MDIO write table that configures the SERDES and
   PLL. This is a fixed table in `brcm_pcie_setup()` of the Linux
   `pcie-brcmstb` driver and must be ported exactly.
2. Wait for `PCIE_STATUS.PHYLINK_UP`, then `.DL_ACTIVE`.
3. Configure the RC BARs and the UBUS remaps that provide config space, and the
   CPU to PCIe outbound window.
4. **32 bit specific:** the VL805 BAR defaults to `0x6_00000000`, which a 32 bit
   ARM cannot address. The root bridge must reprogram the BAR into a 32 bit
   window.
5. Implement `EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL` for this root complex. Note that
   this tree has no producer of that protocol at all: `PciBusDxe` and `XhciDxe`
   exist but nothing feeds them, so the root bridge is new code, not
   configuration.
6. Then the existing drivers: `PciBusDxe`, `XhciDxe`, `UsbBusDxe`, `UsbKbDxe`.

Also worth testing, because it is free: `dtparam=pciex1` in `config.txt`. If the
firmware's own PCIe power policy depends on it, it may keep the block clocked and
make the whole power-up path unnecessary.

## Files

- `Pi3BoardPkg/Drivers/Bcm2711PcieDxe/` - the probe and the start of bring-up
- `Pi2BoardPkg/Library/Pi2BoardLib/Pi2BoardMem.c` - the PCIe mapping

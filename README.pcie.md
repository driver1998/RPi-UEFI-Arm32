# PCIe bring-up on BCM2711 (Raspberry Pi 4), ARM32 EDK2 port

Status: **root complex and link work, the device is visible on the bus, but its
memory BAR cannot be read through the outbound window yet.**

The goal is USB: the four USB-A ports are behind the VL805 xHCI controller, which
is on PCIe, so keyboard input and USB storage both need this to work.

## What works, verified on hardware

    rgr1 before             = 0x00000003   PERST# and bridge init both asserted
    perst deasserted        rgr1 = 0x00000000
    PHYLINKUP reached after 7 ms, PCIE_STATUS = 0x000000B0
    PCIE_STATUS bits: 0x80 PCIE_PORT, 0x20 DL_ACTIVE, 0x10 PHYLINKUP
    0:0.0 vendor=0x14E4 device=0x2711 class=0x060400   root complex
    1:0.0 vendor=0x1106 device=0x3483 class=0x0C0330   VL805 xHCI controller

So: address mapping, reset release, link training, config space access and
downstream bridge forwarding are all confirmed working. The VL805 answers
configuration reads.

## What does not work

Reading the VL805 register block through the outbound window still returns zero.
Every register involved reads back the intended value:

    win0_lo    = 0xC0000000    PCI side base of the window
    base_limit = 0xC030C000    CPU side 0xC00..0xC03 MB, a 4 MB window
    misc_ctrl  = 0x78003480    SCB access enable, 128 B burst, SCB0 size 15 (1 GB)
    BAR0       = 0xC0000004    64 bit memory BAR, address 0xC0000000
    command    = 0x0006        memory space and bus master enabled

    bridge memory base/limit = 0xC000 / 0xC030, covering the same range

The three ranges that have to agree, the window's PCI base, the device BAR and the
bridge memory window, now all hold 0xC0000000, and the SCB decode size is set, so
the obvious mismatches are gone.

## The next experiment, and why it is the right one

The read returns 0 without faulting, which is ambiguous: it is the value both of a
decoded address whose target does not answer and of an address nothing decodes.
Distinguish the two before changing anything else, because they lead in opposite
directions:

1. **Write a pattern to 0xC0000000 and read it back**, and time the access. A fast
   read that returns the pattern means the address is decoded by the root complex
   and the problem is downstream at the device. A read that returns zero slowly, or
   returns the pattern quickly, means the access is not reaching the root complex
   and the CPU side of the window is not being honoured.
2. Read a known-undecoded address in the same session for comparison. Earlier
   observations: 0x7D500000 returned 0xFFFFFFFF immediately (undecoded) and
   0xFD500000 returned zero after about ten seconds (decoded, target silent). Any
   third signature would be informative.

If the access does reach the root complex, the remaining candidates are the window
index (try window 1), the vendor specific endian register
(PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1, the last step of brcm_pcie_setup), and
whether the memory controller windows need to be programmed for non-prefetchable
memory in the prefetchable base/limit registers instead of 0x20 and 0x24.

## History: three failures with the same shape

All three were silent, returning zero with no fault, no ASSERT and no log line,
which is why the debug prints matter more than the changes do:

| failure | symptom |
|---|---|
| window PCI base did not match the BAR address | translation ended where no device lives |
| bridge memory base/limit did not contain the window | the bridge dropped the cycle |
| SCB decode size was zero | the root complex decoded nothing |

The lesson worth keeping: print the values read back from hardware, never the
value that was written, and never a literal in the format string. One build printed
`-> pci 0xF8000000` from a string literal while the register held 0xC0000000, and
that cost a round trip.

## Reference material

The Linux tree the numbers came from is at `linux/` in the workspace:
`drivers/pci/controller/pcie-brcmstb.c` for the register offsets, the reset
sequence and the outbound window encoding, and `include/linux/pci-ecam.h` for the
config space index format.

Key BCM2711 values:

- root complex at **0xFD500000**: `scb` in bcm2711.dtsi maps 0x7c000000 to
  0xfc000000, so pcie@7d500000 lands there
- `PCIE_RGR1_SW_INIT_1` = 0x9210, bridge init mask 0x2 shift 1, PERST# mask 0x1
- `PCIE_MISC_MISC_CTRL` = 0x4008, `HARD_DEBUG` = 0x4204 (SERDES IDDQ 0x08000000)
- `PCIE_MEM_WIN0_LO/HI` = 0x400C / 0x4010, `WIN0_BASE_LIMIT` = 0x4070 with the CPU
  base in bits 15:4 and limit in bits 31:20, both in megabytes
- `EXT_CFG_INDEX` = 0x9000, `EXT_CFG_DATA` = 0x8000, index is
  `bus << 20 | devfn << 12 | (where & 0xfff)`

## Files

- `Pi3BoardPkg/Drivers/Bcm2711PcieDxe/` - bring-up, config space, window and BAR
  assignment
- `Pi2BoardPkg/Library/Pi2BoardLib/Pi2BoardMem.c` - maps 0xFC000000 (root complex)
  and 0xC0000000 (outbound window) as device memory

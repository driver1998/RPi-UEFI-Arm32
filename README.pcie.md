# PCIe bring-up on BCM2711 (Raspberry Pi 4), ARM32 EDK2 port

Status: **base address confirmed, reset sequence identified, not yet implemented.**

The goal is USB: the four USB-A ports are behind the VL805 xHCI controller, which
is on PCIe, so keyboard input and USB storage both need the root complex running.

## Addressing: 0xFD500000, and how the three read signatures differ

`bcm2711.dtsi` wraps the peripheral nodes in an `scb` node:

```
scb {
        ranges = <0x0 0x7c000000  0x0 0xfc000000  0x03800000>,
                 <0x6 0x00000000  0x6 0x00000000  0x40000000>;

        pcie0: pcie@7d500000 { reg = <0x0 0x7d500000 0x9310>; ... };
};
```

`pcie@7d500000` is inside `scb`, so it is translated by the first range:

```
0x7c000000 -> 0xfc000000          (offset +0x80000000)
0x7d500000 -> 0xfc000000 + (0x7d500000 - 0x7c000000) = 0xFD500000
```

**The root complex is at 0xFD500000.** Reading 0x7D500000 directly reaches nothing.

Two failure signatures, which must not be confused:

| reading | meaning |
|---|---|
| `0x00000000` after about ten seconds | address is decoded, the target does not answer: AXI timeout |
| `0xFFFFFFFF` immediately | nothing decoded at that address, interconnect returns all ones |

So the observed `0` + 10 s at 0xFD500000 is the real signature and means the block
is decoded but not running. An all-ones status must never be treated as a live
link, which is why the driver now returns `EFI_NO_MAPPING` for it.

The `ranges` of the pcie node itself also matter for the 32 bit port:

```
ranges     = <0x02000000 0x0 0xf8000000  0x6 0x00000000  0x0 0x04000000>;
dma-ranges = <0x02000000 0x0 0x00000000  0x0 0x00000000  0x0 0xc0000000>;
```

PCI memory 0xF8000000..0xFBFFFFFF maps to CPU **0x600000000**, which a 32 bit ARM
cannot address. The port must choose its own 32 bit CPU side address for the
outbound window. DMA to the first 3 GB of DRAM is fine as is.

## The reset and power sequence Linux uses on BCM2711

From `bcm2711_cfg` in `pcie-brcmstb.c`, BCM2711 uses the **generic** helpers:

```
.perst_set          = brcm_pcie_perst_set_generic,
.bridge_sw_init_set = brcm_pcie_bridge_sw_init_set_generic,
.num_inbound_wins   = 3,
```

and both helpers operate on the same register, `PCIE_RGR1_SW_INIT_1`, whose offset
comes from `pcie_offsets[RGR1_SW_INIT_1]`:

```
#define RGR1_SW_INIT_1_INIT_GENERIC_MASK   0x2
#define RGR1_SW_INIT_1_INIT_GENERIC_SHIFT  0x1
```

- `bridge_sw_init_set(val)`: clear mask `0x2`, insert `val << 1`
- `perst_set(val)`: insert `val` at `PCIE_RGR1_SW_INIT_1_PERST_MASK`

`brcm_pcie_setup()` then runs, in order:

1. `bridge_sw_init_set(1)` - hold the bridge in reset
2. `perst_set(1)` - assert PERST# (BCM2711 only)
3. sleep 100..200 us
4. `bridge_sw_init_set(0)` - release the bridge
5. read `HARD_DEBUG`, clear `SERDES_IDDQ` (0x08000000), write back
6. sleep 100..200 us
7. `MISC_CTRL`: SCB_ACCESS_EN=1, CFG_READ_UR_MODE=1, MAX_BURST_SIZE=0 (128 B for
   BCM2711), RCB_MPS_MODE=1, RCB_64B_MODE=1
8. inbound windows (3 of them), RC mode check, SCB sizes from memory controller size
9. L0s and link width in `PCIE_RC_CFG_PRIV1_LINK_CAPABILITY`
10. class code 0x060400 in `PCIE_RC_CFG_PRIV1_ID_VAL3`
11. outbound windows
12. endian mode in `PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1`

No clock or reset controller is needed: the DT declares none for this node and
`brcm_pcie_probe` only takes them optionally, so the firmware leaves the PCIe clock
running. An earlier conclusion in this branch that the firmware powers PCIe down,
and the mailbox clock hunt that followed, were both wrong and are superseded here.

## Still to look up in the local linux tree

- `pcie_offsets[RGR1_SW_INIT_1]` - the register offset for BCM2711
- `RGR1_SW_INIT_1_PERST_MASK` - the PERST bit inside that register

Both are in `drivers/pci/controller/pcie-brcmstb.c`; grep for `pcie_offsets` and
`RGR1_SW_INIT_1_PERST_MASK`.

## Remaining work after the reset sequence

1. Wait for `PCIE_STATUS.PHYLINK_UP`, then `.DL_ACTIVE` (0x4068).
2. Configure RC BARs and UBUS remaps for config space, plus the outbound window
   with a **32 bit** CPU side address of our choosing.
3. Reprogram the VL805 BAR out of `0x6_00000000` into that window.
4. Implement `EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL`. This tree has no producer of that
   protocol at all, so it is new code, not configuration.
5. Then the existing drivers: `PciBusDxe`, `XhciDxe`, `UsbBusDxe`, `UsbKbDxe`.

## Files

- `Pi3BoardPkg/Drivers/Bcm2711PcieDxe/` - probe, plus the start of bring-up
- `Pi2BoardPkg/Library/Pi2BoardLib/Pi2BoardMem.c` - maps
  `0xFC000000..0xFDFFFFFF` as device memory so the root complex is reachable

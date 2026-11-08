# RPi-UEFI, Raspberry Pi 4 (BCM2711) ARM32 port

An ARM32 UEFI firmware for the Raspberry Pi 4, built from the Microsoft
[ms-iot RPi-UEFI](https://github.com/ms-iot/RPi-UEFI) fork of
[Linaro EDK2 2015.01](https://git.linaro.org/uefi/linaro-edk2.git). That fork
targets Windows 10 IoT Core on the Pi 2 and Pi 3; this branch moves it to
BCM2711 while deliberately keeping the Pi 3 naming, so the build script and the
packages are still called `BuildPi3Board.bat` and `Pi3BoardPkg`. There is no
attempt to stay compatible with Pi 3 hardware.

The original project documentation, including the toolchain setup and the
Windows 10 IoT Core background, is preserved unchanged in
[README.ms-iot.md](README.ms-iot.md).

## Status

The firmware boots to the BDS menu on a Raspberry Pi 4 and is stable there. The
port is a bring-up: several subsystems work, several are known incomplete, and each
incomplete one is documented rather than left as a surprise.

| Area | State |
|---|---|
| SEC, mailbox, memory sizing | working |
| MMU (short descriptor, caches off) | working |
| GIC-400 interrupts, ARM generic timer | working |
| HDMI console, 1920x1080 | working |
| Serial console on the PL011, 115200 8N1 | working |
| BDS menu, DXE dispatch (about 30 drivers) | working, five out of five boots |
| eMMC2 SD controller: card identification, `BlockIo` | working |
| eMMC2 SD controller: block data reads | **incomplete**, see below |
| Serial keyboard input | **incomplete**, see below |
| Data and instruction caches | **disabled**, see [README.caches.md](README.caches.md) |
| PCIe and therefore USB | **incomplete**, see [README.pcie.md](README.pcie.md) |

### Known limitations

- **Caches are off.** Write-back clobbers memory and write-through stalls inside
  the DXE core load. The measured behaviour, what has been ruled out and the next
  experiments are in [README.caches.md](README.caches.md).
- **PCIe and USB do not work.** The root complex is woken up, the link trains, the
  config space works and the VL805 xHCI controller is enumerated, but the outbound
  window needs a CPU address at 4 GB or above, which the short descriptor MMU
  cannot express. This is a mapping capability, not a driver defect. Details, both
  Linux register dumps and the two ways forward are in
  [README.pcie.md](README.pcie.md).
- **The SD card data path is incomplete.** Commands work, the card is identified
  and `BlockIo` installs, but reading a block returns no payload. The remaining
  suspects are electrical, and the investigation notes are in the commit history
  plus [README.pcie.md](README.pcie.md) for the method that was used.
- **Serial input does not work.** The PL011 receive path is proven good at PrePi
  and stops delivering once the MMU is on, while the pin still shows the data
  arriving. Console output is unaffected. The full bisect is recorded in the commit
  history; the summary is in `git log --grep="serial input"`.

Bring-up settings still in place, to be restored once the port is stable:
`PcdDebugPropertyMask` is `0x0f`, with `ASSERT_DEADLOOP` removed so one driver
cannot stop DXE, and `PcdDebugPrintErrorLevel` is `0x800404C7` rather than the
production `0x80000003`.

## Building

The build environment is the one described in [README.ms-iot.md](README.ms-iot.md):
the `RPi-UEFI` and `Toolchains` folders must sit side by side, and the GNU ARM
Embedded 4.8 toolchain is expected at `D:\Toolchains\Arm`.

```
BuildPi3Board.bat
```

The output is `Build\Pi3Board\DEBUG_ARMGCC\kernel.img`, 1,966,080 bytes: the
firmware volume at 0x108000 preceded by a 1 MB pad and the VideoCore startup blob
at 0x8000. Copy it to the boot partition of the SD card as `kernel.img`.

`BuildPi2Board.bat` and `BuildPi2Pi3Board.bat` still exist but target Pi 2 and Pi 3
hardware and are not maintained by this port.

Because these are DEBUG images, expect the serial console to be verbose. The
`README.*.md` files describe what that output means.

## How to connect

- **HDMI** is the primary console, driven by `DisplayDxe` through the mailbox, with
  `GraphicsConsoleDxe` and `TerminalDxe` on top of it.
- **Serial** is the PL011 at `0xFE201000`, GPIO14 and GPIO15 in ALT0, 115200 8N1 on
  a 3.3 V USB-TTL adapter. The driver asks the firmware for a 48 MHz UART clock over
  the mailbox so the baud rate does not depend on `config.txt`.

A `config.txt` that works, and that the port was developed against:

```
arm_64bit=0
arm_boost=1
enable_uart=1
uart_2ndstage=1
enable_gic=1
kernel=kernel.img
disable_commandline_tags=1
disable_overscan=1
device_tree_address=0x3e0000
device_tree_end=0x400000
dtoverlay=miniuart-bt
dtoverlay=upstream-pi4
```

## What the port changed

The interesting parts, in the order they were needed to reach BDS:

- **BCM2711 is not a BCM2837 with a bigger number.** The peripherals live in the
  `0xFE000000` window rather than `0x3F000000`, the ARM local block is at
  `0xFF800000`, and the interrupt controller is a real **GIC-400** at `0xFF841000`
  and `0xFF842000` rather than Broadcom's own controller. The GIC, the timer PPIs
  and the memory map all had to change accordingly.
- **The console moved from the AUX mini UART to the PL011**, whose clock is
  requested explicitly over the mailbox. The mini UART is clocked from the core
  clock, which the firmware varies, and it produced garbled output.
- **`ArmPlatformPkg/Bds/Bds.c`** walked an uninitialised `BootOrder` in
  `StartDefaultBootOnTimeout`, which faults on BCM2711 because the garbage pointer
  lands in the unmapped hole at `0x00200000`. Fixed.
- **The SD controller moved from SDHOST to the Arasan eMMC2** at `0xFE340000`, the
  block the Pi 4 SD slot is actually wired to. `SdHostDxe` gained an
  `EFI_MMC_HOST_PROTOCOL` backend for it, selected by `PcdSdIsArasan`.
- **`ConSplitterDxe` is still disabled**, as it was in the ms-iot fork, with the
  same comment: it needs fixing to recognise valid console input, output and error.
- **The PCIe register block at `0xFD500000` had never been mapped** and now is,
  which is what allowed the root complex work to start at all.

`BuildNotes2.txt` holds the original notes on the build layout.

## Documentation

| File | Contents |
|---|---|
| [README.ms-iot.md](README.ms-iot.md) | the original ms-iot project README, unchanged |
| [README.caches.md](README.caches.md) | cache bring-up: measurements, ruled out causes, next steps |
| [README.pcie.md](README.pcie.md) | PCIe bring-up: what works, the Linux register dumps, why it stops |
| `BuildNotes2.txt` | build layout notes from the original project |

## Licence

The original licence and code of conduct from the ms-iot project apply; see
[LICENSE.txt](LICENSE.txt) and [README.ms-iot.md](README.ms-iot.md).

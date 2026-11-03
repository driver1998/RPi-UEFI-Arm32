# Raspberry Pi 4 SD support (BCM2711 eMMC2)

Design note for adding the Pi 4 SD card path. Written after reaching BDS on
BCM2711 with the SD drivers excluded from the image.

## Why the Pi 3 drivers cannot work on a Pi 4

`MmcDxe` is controller independent: it implements card identification, the MMC
state machine, `BlockIo` and `DiskIo`, and talks to hardware through
`EFI_MMC_HOST_PROTOCOL` (`EmbeddedPkg/Include/Protocol/MmcHost.h`, eight
functions: `IsCardPresent`, `IsReadOnly`, `BuildDevicePath`, `NotifyState`,
`SendCommand`, `ReceiveResponse`, `ReadBlockData`, `WriteBlockData`).

`SdHostDxe` is only one backend for that protocol: it drives the Broadcom
**SDHOST** block at `0xFE202000` (`SDHOST_CDIV`, `SDHOST_TOUT`, ...). On a
Raspberry Pi 2/3 the SD card slot is wired to SDHOST.

On BCM2711 the SD card slot is wired to **eMMC2**, an Arasan SDHCI controller at
**`0xFE340000`**, and SDHOST is not connected to the card at all. Running the
Pi 3 backend there therefore ends in `ASSERT(mRca != 0)` inside SdHostDxe
because no card ever answers, which is what stopped DXE before BDS.

## Approach: a second backend selected by PCD

`PcdSdIsArasan` (`Pi2BoardPkg.dec`, BOOLEAN, default FALSE) selects the backend:

- FALSE: existing SDHOST code path, unchanged (Pi 2/3)
- TRUE: new Arasan SDHCI code path (Pi 4); set in `Pi3BoardPkg.dsc`

`SdHostDxe` keeps a single `EFI_MMC_HOST_PROTOCOL` instance; each protocol
function dispatches on the PCD to either `SdHost...` (existing) or
`ArasanSd...` (new) helpers. This mirrors the way edk2-platforms keeps one
`SdHostDxe` for the RaspberryPi platforms with an `SdIsArasan` PCD instead of
forking the driver.

`Bcm2836MMCHS.h` already defines the Arasan register layout
(`MMCHS1BASE 0xFE300000`, `MMCHS_CMD`, `MMCHS_AC12`, `MMCHS_SYSCTL`, ...) but no
driver in this tree uses it. The new backend should move that base to
`0xFE340000` (a `PcdSdIsArasan`-selected define) and use the SDHCI field
definitions that go with it.

## Pi 4 specifics that differ from the Pi 3 SDHOST path

### Base address and clock

- eMMC2 register block: `0xFE340000`, SDHCI register layout, 0x100 bytes
- The SDHCI capability register's base clock field is **not reliable on
  BCM2711** (Linux carries `SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN` for this
  controller), so the driver must not derive its divider from `CAPABILITIES`.
  Use an explicit base clock constant instead. The Pi 4 firmware leaves the
  eMMC2 clock at a fixed rate; start from **100 MHz** and keep the constant in
  one place so it can be corrected from a measurement.
- Divider: SDHCI `CLOCK_CONTROL` uses `SDCLK_FREQ_SEL` written as
  `(base / (2 * target))`, ten bits wide, and the clock must be disabled before
  the divider is changed.
- Identification runs at 400 kHz, then the card's `TRAN_SPEED` from the CSD
  selects the transfer rate; cap it at 25 MHz for 3.3 V signalling on the first
  bring-up instead of going for high speed straight away.

### Signalling voltage

The Pi 4 SD slot has a 1.8 V regulator for UHS modes. It is not needed for
basic 3.3 V operation, so the first milestone should explicitly stay at 3.3 V
(`POWER_CONTROL` bit 1) and never toggle `HOST_CONTROL1` 1.8 V signalling.

### Interrupts

eMMC2 raises GIC SPI 126. The existing SDHOST backend polls status registers
and never enables interrupts; keep the Arasan backend polled as well for the
first milestone, and only wire the interrupt if polling turns out too slow.
Polled operation also avoids depending on the still-open timer/interrupt
questions on this port.

## Implementation steps

1. Add `ArasanSd.c` next to `SdHostDxe.c`, with:
   - SDHCI register and bit definitions (own header or extend `Bcm2836MMCHS.h`)
   - `ArasanSdInitialize`: soft reset (`SW_RESET_ALL`), read `CAPABILITIES` for
     baseline info, set 400 kHz from the fixed base clock, power on at 3.3 V,
     enable the card clock
   - `ArasanSdSendCommand` / `ArasanSdReceiveResponse`: write `ARGUMENT`,
     `COMMAND` (index, response type, CRC check, data-present), wait on
     `INTERRUPT_STATUS` `COMMAND_COMPLETE` / `TRANSFER_COMPLETE` / errors, read
     `RESPONSE0..3`
   - `ArasanSdReadBlockData` / `WriteBlockData`: `BLOCK_SIZE`,
     `BLOCK_COUNT`, `TRANSFER_MODE`, then PIO through `BUFFER_DATA_PORT`
   - `IsCardPresent` (always TRUE, the slot is soldered),
     `IsReadOnly` (FALSE), `BuildDevicePath` (same node as the SDHOST backend),
     `NotifyState` (no-op or clock change only)
2. Dispatch on `PcdSdIsArasan` inside the eight protocol functions in
   `SdHostDxe.c`.
3. Add `gPi2BoardTokenSpaceGuid.PcdSdIsArasan` to `SdHostDxe.inf`.
4. Set `PcdSdIsArasan|TRUE` in `Pi3BoardPkg.dsc` and put the two SD `INF`s back
   into the DSC and FDF.
5. Restore `PcdDebugPropertyMask` `ASSERT_DEADLOOP` once the driver is known
   good, so a driver assert is fatal again.

## Validation milestones

1. Card answers `CMD0`/`CMD8`/`ACMD41`/`CMD2`/`CMD3` and `CMD9` returns a CSD:
   the driver prints the capacity it decodes from `C_Size`.
2. `MmcDxe` reaches `MmcTransferState` and installs `BlockIo`.
3. `DiskIo`/`PartitionDxe`/`Fat` find the FAT partition and BDS stops printing
   "No recognize filesystem to parse".
4. A file can be read from the card, then boot an EFI application from it.

## References

- SD Host Controller Simplified Specification (SDHCI register layout)
- Linux `sdhci-iproc` / `sdhci-bcm2835` for the BCM2711 eMMC2 quirks, in
  particular `SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN`
- edk2-platforms `Platform/RaspberryPi` for the `SdIsArasan` PCD approach

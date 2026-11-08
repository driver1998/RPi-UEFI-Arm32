/** @file
  Arasan SDHCI backend for EFI_MMC_HOST_PROTOCOL on the BCM2711 eMMC2 controller.

  Copyright (c), Microsoft Corporation. All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

  Only the parts that are controller specific live here. IsCardPresent,
  IsReadOnly and BuildDevicePath are hardware independent and are served by the
  existing SdHostDxe implementations.

**/

#include <Uefi.h>

#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/MmcHost.h>

#include <BcmMailbox.h>

#include "ArasanSd.h"

//
// BCM2711 eMMC2 (Arasan SDHCI). The Pi 3 build uses the BCM2836/2837 SDHOST
// block at 0xFE202000 instead; on a Pi 4 the SD slot is wired to eMMC2 and
// SDHOST is not connected to the card at all.
//
#define ARASAN_SD_BASE              0xFE340000UL

//
// The SDHCI capabilities register reports a base clock that is not reliable on
// BCM2711 (Linux carries SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN for this controller),
// so the divider is computed from this constant instead. Keep it in one place so
// it can be corrected from a measurement.
//
#define ARASAN_SD_BASE_CLOCK_HZ     100000000UL

#define ARASAN_SD_IDENT_CLOCK_HZ    400000UL
#define ARASAN_SD_TRANSFER_CLOCK_HZ 25000000UL

//
// SDHCI register offsets
//
#define SDHCI_DMA_ADDRESS           0x00
#define SDHCI_BLOCK_SIZE_COUNT      0x04
#define SDHCI_ARGUMENT              0x08
#define SDHCI_TRANSFER_MODE         0x0C
#define SDHCI_COMMAND               0x0E
#define SDHCI_RESPONSE_0            0x10
#define SDHCI_BUFFER_DATA_PORT      0x20
#define SDHCI_PRESENT_STATE         0x24
#define SDHCI_HOST_CONTROL          0x28
#define SDHCI_POWER_CONTROL         0x29
#define SDHCI_CLOCK_CONTROL         0x2C
#define SDHCI_TIMEOUT_CONTROL       0x2E
#define SDHCI_SOFTWARE_RESET        0x2F
#define SDHCI_NORMAL_INT_STATUS     0x30
#define SDHCI_ERROR_INT_STATUS      0x32
#define SDHCI_NORMAL_INT_STATUS_ENABLE  0x34
#define SDHCI_ERROR_INT_STATUS_ENABLE   0x36
#define SDHCI_NORMAL_INT_SIGNAL_ENABLE  0x38
#define SDHCI_ERROR_INT_SIGNAL_ENABLE   0x3A
#define SDHCI_CAPABILITIES          0x40
#define SDHCI_HOST_CONTROLLER_VER   0xFE

//
// SDHCI_CLOCK_CONTROL bits. The SDCLK Frequency Select field is 10 bits split as
// bits[7:6] (low two) and bits[15:8] (high eight) on SDHCI 3.0 controllers, so
// the programmed value V is (V >> 2) << 8 | (V & 3) << 6.
//
#define SDHCI_CLOCK_INT_EN          (1U << 0)
#define SDHCI_CLOCK_INT_STABLE      (1U << 1)
#define SDHCI_CLOCK_CARD_EN         (1U << 2)
#define SDHCI_CLOCK_SEL_MASK        0xFFC0
#define SDHCI_CLOCK_MAX_DIVIDER     1023

//
// SDHCI_POWER_CONTROL: bus power on plus the bus voltage (bits[3:1]).
//   111 = 3.3 V, 110 = 3.0 V, 101 = 1.8 V
#define SDHCI_POWER_33V_ON          0x0F
#define SDHCI_POWER_18V_ON          0x0B

//
// SDHCI_HOST_CONTROL2 (16-bit, SDHCI 3.0). Bit 3 enables 1.8 V signalling.
//
#define SDHCI_HOST_CONTROL2         0x3E
#define SDHCI_HOST_CONTROL2_18V     (1U << 3)

//
// SDHCI_HOST_CONTROL bits. Bit 7 selects the card detect test level instead of
// the SDCD# pin and bit 6 is that level; the Pi 4 slot has no card detect pin
// wired to eMMC2, so both are needed before the controller will start a command.
//
#define SDHCI_HOST_CONTROL_CD_TEST_LEVEL    (1U << 6)
#define SDHCI_HOST_CONTROL_CD_TEST_SEL      (1U << 7)

#define SDHCI_SOFTWARE_RESET_ALL    (1U << 0)
#define SDHCI_SOFTWARE_RESET_CMD    (1U << 1)
#define SDHCI_SOFTWARE_RESET_DAT    (1U << 2)

//
// SDHCI normal interrupt status (write one to clear)
//
#define SDHCI_INT_CMD_COMPLETE      (1U << 0)
#define SDHCI_INT_XFER_COMPLETE     (1U << 1)
#define SDHCI_INT_BUF_WRITE_READY   (1U << 4)
#define SDHCI_INT_BUF_READ_READY    (1U << 5)

//
// SDHCI_COMMAND response type select
//
#define SDHCI_CMD_RESP_NONE         0x00
#define SDHCI_CMD_RESP_LONG         0x01
#define SDHCI_CMD_RESP_SHORT        0x02
#define SDHCI_CMD_CRC_CHECK         (1U << 3)
#define SDHCI_CMD_INDEX_CHECK       (1U << 4)
#define SDHCI_CMD_DATA_PRESENT      (1U << 5)

//
// SDHCI_TRANSFER_MODE bits
//
#define SDHCI_TM_DMA_ENABLE         (1U << 0)
#define SDHCI_TM_BLOCK_COUNT_EN     (1U << 1)
#define SDHCI_TM_READ               (1U << 3)
#define SDHCI_TM_MULTI_BLOCK        (1U << 4)

#define SDHCI_BLOCK_BYTE_LENGTH     512
#define SDHCI_BLOCK_BOUNDARY_4K     0x7000

//
// Block count programmed for a multi block transfer. MmcDxe caps its own
// per-command block count and terminates the transfer with CMD12, so this only
// has to be at least as large as the number of blocks it will actually consume.
//
#define ARASAN_SD_MULTI_BLOCK_COUNT 64

#define ARASAN_SD_CMD_POLL_COUNT    1000000UL
#define ARASAN_SD_DATA_POLL_COUNT   1000000UL

static BOOLEAN  mArasanInitialized     = FALSE;
static BOOLEAN  mArasanAtTransferClock = FALSE;

//
// SDMA bounce buffer for the data path.
//
// PIO was tried first and never produced a payload: the controller accepted the
// command, reported the DAT line active, raised no error, and the buffer data
// port returned zeros. The working Linux configuration uses DMA (its
// TRANSFER_MODE has DMA Enable set), so the data path moves to single address
// DMA, which this controller appears to require.
//
// The driver runs with the data cache disabled and the memory map is identity
// mapped, so the buffer's virtual address is also its physical address and no
// cache maintenance is needed.
//
static VOID   *mArasanDmaBuffer     = NULL;
static UINTN   mArasanDmaBlockIndex = 0;

STATIC
EFI_STATUS
ArasanEnsureDmaBuffer (
  VOID
  )
{
    if (mArasanDmaBuffer != NULL) {
        return EFI_SUCCESS;
    }

    mArasanDmaBuffer = AllocatePages (
                         EFI_SIZE_TO_PAGES (
                           (UINTN)SDHCI_BLOCK_BYTE_LENGTH * ARASAN_SD_MULTI_BLOCK_COUNT));
    if (mArasanDmaBuffer == NULL) {
        DEBUG ((DEBUG_ERROR, "ArasanSd: failed to allocate the SDMA buffer\n"));
        return EFI_OUT_OF_RESOURCES;
    }

    DEBUG ((
        DEBUG_INIT,
        "ArasanSd: SDMA buffer at 0x%08x, %d bytes\n",
        (UINT32)(UINTN)mArasanDmaBuffer,
        (UINT32)SDHCI_BLOCK_BYTE_LENGTH * ARASAN_SD_MULTI_BLOCK_COUNT));

    return EFI_SUCCESS;
}

/**
  Reset the command (and data) line after a failed command.

  A command that times out leaves the controller with Command Inhibit asserted in
  PRESENT_STATE. Every later COMMAND write is then ignored, which turns a single
  expected failure (an SD card does not answer CMD1, for instance) into a run of
  bogus timeouts for all following commands. The SDHCI way out is a command line
  reset, which is what Linux does in sdhci_reset_for_error().
**/
STATIC
VOID
ArasanResetCmdLine (
  VOID
  )
{
    UINTN  Spin;

    MmioWrite8 (ARASAN_SD_BASE + SDHCI_SOFTWARE_RESET,
                SDHCI_SOFTWARE_RESET_CMD | SDHCI_SOFTWARE_RESET_DAT);

    for (Spin = 0; Spin < ARASAN_SD_CMD_POLL_COUNT; Spin++) {
        if ((MmioRead8 (ARASAN_SD_BASE + SDHCI_SOFTWARE_RESET) &
             (SDHCI_SOFTWARE_RESET_CMD | SDHCI_SOFTWARE_RESET_DAT)) == 0) {
            break;
        }
    }

    MmioWrite16 (ARASAN_SD_BASE + SDHCI_NORMAL_INT_STATUS, 0xFFFF);
    MmioWrite16 (ARASAN_SD_BASE + SDHCI_ERROR_INT_STATUS, 0xFFFF);
}

/**
  Program the SD clock divider and enable the card clock.

  @param  TargetHz  Requested SD clock in Hz.
**/
STATIC
VOID
ArasanSetSdClock (
  IN UINT32  TargetHz
  )
{
    UINT32  Divider;
    UINT16  Control;
    UINTN   Spin;

    if (TargetHz == 0) {
        return;
    }

    Divider = ARASAN_SD_BASE_CLOCK_HZ / (2 * TargetHz);
    if (Divider == 0) {
        Divider = 1;
    }
    if (Divider > SDHCI_CLOCK_MAX_DIVIDER) {
        Divider = SDHCI_CLOCK_MAX_DIVIDER;
    }

    //
    // The clock must be off while the divider changes.
    //
    Control  = MmioRead16 (ARASAN_SD_BASE + SDHCI_CLOCK_CONTROL);
    Control &= (UINT16)~(SDHCI_CLOCK_CARD_EN | SDHCI_CLOCK_SEL_MASK);
    MmioWrite16 (ARASAN_SD_BASE + SDHCI_CLOCK_CONTROL, Control);

    Control |= (UINT16)(((Divider >> 2) << 8) & 0xFF00);
    Control |= (UINT16)((Divider & 0x3) << 6);
    Control |= SDHCI_CLOCK_INT_EN;
    MmioWrite16 (ARASAN_SD_BASE + SDHCI_CLOCK_CONTROL, Control);

    //
    // Wait for the internal clock to become stable before enabling it on the bus.
    //
    for (Spin = 0; Spin < ARASAN_SD_CMD_POLL_COUNT; Spin++) {
        if ((MmioRead16 (ARASAN_SD_BASE + SDHCI_CLOCK_CONTROL) & SDHCI_CLOCK_INT_STABLE) != 0) {
            break;
        }
    }
    if (Spin == ARASAN_SD_CMD_POLL_COUNT) {
        DEBUG ((DEBUG_ERROR, "ArasanSd: internal clock never became stable\n"));
    }

    Control |= SDHCI_CLOCK_CARD_EN;
    MmioWrite16 (ARASAN_SD_BASE + SDHCI_CLOCK_CONTROL, Control);

    DEBUG ((
        DEBUG_INIT,
        "ArasanSd: target %d Hz, divider %d, clock control=0x%04x (stable after %d polls)\n",
        TargetHz, Divider, MmioRead16 (ARASAN_SD_BASE + SDHCI_CLOCK_CONTROL), (UINT32)Spin));
}

/**
  Bring the controller to a known state: reset, identification clock, 3.3 V power.

  Called once from the driver entry point when PcdSdIsArasan is set, and again on
  the MmcHwInitializationState notification.
**/
EFI_STATUS
ArasanSdInitialize (
  VOID
  )
{
    UINTN   Spin;
    UINT16  Version;
    UINT32  Capabilities;

    Version      = MmioRead16 (ARASAN_SD_BASE + SDHCI_HOST_CONTROLLER_VER);
    Capabilities = MmioRead32 (ARASAN_SD_BASE + SDHCI_CAPABILITIES);
    DEBUG ((
        DEBUG_INIT,
        "ArasanSd: base=0x%08x SDHCI version %d.%02d, capabilities=0x%08x\n",
        (UINT32)ARASAN_SD_BASE, Version >> 8, Version & 0xFF, Capabilities));
    DEBUG ((
        DEBUG_INIT,
        "ArasanSd: using a fixed %d Hz base clock for the divider\n",
        (UINT32)ARASAN_SD_BASE_CLOCK_HZ));

    //
    // Request the eMMC2 module clock explicitly.
    //
    // A gated controller still answers register reads (the APB path is up, which
    // is why the version and capabilities registers read correctly) but it never
    // completes a command: the SDHCI command timeout is counted in SD clock
    // cycles, so with no module clock neither COMMAND_COMPLETE nor a timeout
    // error is ever raised. Ask the firmware for the clock the same way the PL011
    // clock is requested at console init, and report what both the eMMC and the
    // eMMC2 clock nodes currently are so a wrong clock id is visible.
    //
    {
        MAILBOX_GET_CLOCK_RATE  MbGetClock;
        MAILBOX_SET_CLOCK_RATE  MbSetClock;
        EFI_STATUS              Status;
        UINT32                  EmmcRate;
        UINT32                  Emmc2Rate;

        ZeroMem ((void *)&MbGetClock, sizeof (MbGetClock));
        MbGetClock.Header.BufferSize = sizeof (MbGetClock);
        MbGetClock.Header.TagID      = TAG_GET_CLOCK_RATE;
        MbGetClock.Header.TagLength  = 8;
        MbGetClock.ClockId           = CLOCK_ID_EMMC;
        if (!EFI_ERROR (MailboxProperty (MAILBOX_CHANNEL_PROPERTY_ARM_VC,
                                         (MAILBOX_HEADER *)&MbGetClock))) {
            EmmcRate = MbGetClock.Rate;
        } else {
            EmmcRate = 0;
        }

        ZeroMem ((void *)&MbGetClock, sizeof (MbGetClock));
        MbGetClock.Header.BufferSize = sizeof (MbGetClock);
        MbGetClock.Header.TagID      = TAG_GET_CLOCK_RATE;
        MbGetClock.Header.TagLength  = 8;
        MbGetClock.ClockId           = CLOCK_ID_EMMC2;
        if (!EFI_ERROR (MailboxProperty (MAILBOX_CHANNEL_PROPERTY_ARM_VC,
                                         (MAILBOX_HEADER *)&MbGetClock))) {
            Emmc2Rate = MbGetClock.Rate;
        } else {
            Emmc2Rate = 0;
        }

        DEBUG ((DEBUG_INIT, "ArasanSd: eMMC clock %d Hz, eMMC2 clock %d Hz\n", EmmcRate, Emmc2Rate));

        ZeroMem ((void *)&MbSetClock, sizeof (MbSetClock));
        MbSetClock.Header.BufferSize = sizeof (MbSetClock);
        MbSetClock.Header.TagID      = TAG_SET_CLOCK_RATE;
        MbSetClock.Header.TagLength  = 12;
        MbSetClock.ClockId           = CLOCK_ID_EMMC2;
        MbSetClock.Rate              = ARASAN_SD_BASE_CLOCK_HZ;
        MbSetClock.SkipSettingTurbo  = 1;
        Status = MailboxProperty (MAILBOX_CHANNEL_PROPERTY_ARM_VC,
                                  (MAILBOX_HEADER *)&MbSetClock);
        DEBUG ((
            DEBUG_INIT,
            "ArasanSd: set eMMC2 clock to %d Hz -> %r, now %d Hz\n",
            (UINT32)ARASAN_SD_BASE_CLOCK_HZ, Status, MbSetClock.Rate));
    }

    //
    // Soft reset the whole controller and wait for it to clear.
    //
    MmioWrite8 (ARASAN_SD_BASE + SDHCI_SOFTWARE_RESET, SDHCI_SOFTWARE_RESET_ALL);
    for (Spin = 0; Spin < ARASAN_SD_CMD_POLL_COUNT; Spin++) {
        if ((MmioRead8 (ARASAN_SD_BASE + SDHCI_SOFTWARE_RESET) & SDHCI_SOFTWARE_RESET_ALL) == 0) {
            break;
        }
    }
    if (Spin == ARASAN_SD_CMD_POLL_COUNT) {
        DEBUG ((DEBUG_ERROR, "ArasanSd: controller reset did not complete\n"));
        return EFI_DEVICE_ERROR;
    }

    //
    // Longest timeout, no interrupts (this backend polls), 1-bit bus.
    //
    // Both interrupt status registers are 16-bit and ERROR_INT_STATUS lives at
    // offset 0x32, so they must be accessed with MmioWrite16: a 32-bit access
    // there is misaligned and traps with an alignment fault.
    //
    MmioWrite8  (ARASAN_SD_BASE + SDHCI_TIMEOUT_CONTROL, 0x0E);

    //
    // Enable the interrupt status bits, but not the interrupt signals.
    //
    // The specification says the Status Enable registers only gate interrupt
    // generation and that the status bits are set regardless, but this Arasan
    // implementation appears to gate the status bits themselves. With both
    // registers left at zero a command that is running (PRESENT_STATE reports
    // Command Inhibit) never shows COMMAND_COMPLETE and never shows a timeout
    // error either, so the driver reports a timeout forever. Linux programs
    // 0x100b into the normal enable register and 0x03ff into the error one; all
    // ones is fine here because this backend polls and leaves SIGNAL_ENABLE at
    // zero. A dump of a working controller shows exactly this difference.
    //
    MmioWrite16 (ARASAN_SD_BASE + SDHCI_NORMAL_INT_STATUS_ENABLE, 0xFFFF);
    MmioWrite16 (ARASAN_SD_BASE + SDHCI_ERROR_INT_STATUS_ENABLE, 0xFFFF);
    MmioWrite16 (ARASAN_SD_BASE + SDHCI_NORMAL_INT_SIGNAL_ENABLE, 0x0000);
    MmioWrite16 (ARASAN_SD_BASE + SDHCI_ERROR_INT_SIGNAL_ENABLE, 0x0000);

    //
    // Do not touch the card detect test bits: the working configuration on this
    // board reports Card Inserted = 0 in PRESENT_STATE and still runs commands,
    // so card detect does not gate anything here. 1-bit bus, default speed.
    //
    MmioWrite8  (ARASAN_SD_BASE + SDHCI_HOST_CONTROL, 0x00);
    MmioWrite16 (ARASAN_SD_BASE + SDHCI_NORMAL_INT_STATUS, 0xFFFF);
    MmioWrite16 (ARASAN_SD_BASE + SDHCI_ERROR_INT_STATUS, 0xFFFF);

    //
    // Power the bus at 3.3 V and keep 3.3 V signalling for identification.
    //
    // A dump of the working controller shows POWER_CONTROL = 0x0F, i.e. 3.3 V;
    // the 1.8 V UHS signalling is expressed through HOST_CONTROL2, not here.
    //
    MmioWrite16 (ARASAN_SD_BASE + SDHCI_HOST_CONTROL2, 0x0000);
    MmioWrite8 (ARASAN_SD_BASE + SDHCI_POWER_CONTROL, SDHCI_POWER_33V_ON);
    gBS->Stall (1000);
    ArasanSetSdClock (ARASAN_SD_IDENT_CLOCK_HZ);

    DEBUG ((
        DEBUG_INIT,
        "ArasanSd: ready, present=0x%08x hostctl=0x%02x power=0x%02x reset=0x%02x\n",
        MmioRead32 (ARASAN_SD_BASE + SDHCI_PRESENT_STATE),
        (UINT32)MmioRead8 (ARASAN_SD_BASE + SDHCI_HOST_CONTROL),
        (UINT32)MmioRead8 (ARASAN_SD_BASE + SDHCI_POWER_CONTROL),
        (UINT32)MmioRead8 (ARASAN_SD_BASE + SDHCI_SOFTWARE_RESET)));

    mArasanInitialized     = TRUE;
    mArasanAtTransferClock = FALSE;
    return EFI_SUCCESS;
}

BOOLEAN
ArasanIsInitialized (
  VOID
  )
{
    return mArasanInitialized;
}

EFI_STATUS
ArasanNotifyState (
    IN EFI_MMC_HOST_PROTOCOL    *This,
    IN MMC_STATE                State
    )
{
    switch (State) {
    case MmcHwInitializationState:
        return ArasanSdInitialize ();

    case MmcTransferState:
        //
        // Card is identified and addressed: move to the transfer clock, once.
        //
        // MmcDxe raises this notification repeatedly, and ArasanSetSdClock()
        // disables the card clock before reprogramming the divider and enables it
        // again afterwards. Doing that again in the middle of a data transfer
        // would abort the very transfer being set up, so only switch when the
        // clock is not already at the transfer rate.
        //
        if (!mArasanAtTransferClock) {
            ArasanSetSdClock (ARASAN_SD_TRANSFER_CLOCK_HZ);
            mArasanAtTransferClock = TRUE;
            DEBUG ((DEBUG_INIT, "ArasanSd: transfer clock %d Hz\n", (UINT32)ARASAN_SD_TRANSFER_CLOCK_HZ));
        }
        return EFI_SUCCESS;

    default:
        return EFI_SUCCESS;
    }
}

EFI_STATUS
ArasanSendCommand (
    IN EFI_MMC_HOST_PROTOCOL    *This,
    IN MMC_CMD                  MmcCmd,
    IN UINT32                   Argument
    )
{
    UINT16  Command;
    UINT16  NormalStatus;
    UINT16  ErrorStatus;
    UINT32  Index;
    UINTN   Spin;

    Index = MMC_GET_INDX (MmcCmd);

    //
    // CMD5 (SDIO send op cond) is only valid for SDIO cards and MmcDxe expects it
    // to fail on an SD card, exactly as the SDHOST backend does.
    //
    if (Index == 5) {
        return EFI_UNSUPPORTED;
    }

    MmioWrite16 (ARASAN_SD_BASE + SDHCI_NORMAL_INT_STATUS, 0xFFFF);
    MmioWrite16 (ARASAN_SD_BASE + SDHCI_ERROR_INT_STATUS, 0xFFFF);
    MmioWrite32 (ARASAN_SD_BASE + SDHCI_ARGUMENT, Argument);

    Command = (UINT16)((Index << 8) & 0x3F00);
    if ((MmcCmd & MMC_CMD_WAIT_RESPONSE) != 0) {
        if ((MmcCmd & MMC_CMD_LONG_RESPONSE) != 0) {
            Command |= SDHCI_CMD_RESP_LONG;
        } else {
            Command |= SDHCI_CMD_RESP_SHORT;
        }
        if ((MmcCmd & MMC_CMD_NO_CRC_RESPONSE) == 0) {
            Command |= SDHCI_CMD_CRC_CHECK;
            //
            // The index check is not defined for the 136-bit responses (CMD2,
            // CMD9), which is why they carry MMC_CMD_LONG_RESPONSE.
            //
            if ((MmcCmd & MMC_CMD_LONG_RESPONSE) == 0) {
                Command |= SDHCI_CMD_INDEX_CHECK;
            }
        }
    }

    //
    // Program the transfer descriptor before COMMAND is written, even for
    // commands without data: the controller samples the whole descriptor when
    // COMMAND is written, and leaving it at whatever the last transfer or the
    // reset left behind makes command behaviour depend on history.
    //
    // MmcDxe sends CMD17/18/24 itself and then pumps the payload through
    // ReadBlockData/WriteBlockData one block per call, so the block size and
    // count have to be set up here rather than in the data callbacks.
    //
    if ((Index == 17) || (Index == 18) || (Index == 24) || (Index == 25)) {
        BOOLEAN  IsRead     = (Index == 17) || (Index == 18);
        BOOLEAN  IsMultiple = (Index == 18) || (Index == 25);
        UINT16   TransferMode;
        UINT16   BlockCount;

        BlockCount   = IsMultiple ? ARASAN_SD_MULTI_BLOCK_COUNT : 1;
        TransferMode = 0;
        if (IsMultiple) {
            //
            // Block count enable only means anything for a multiple block
            // transfer; setting it on a single block transfer makes the
            // controller wait for block counting that never starts.
            //
            TransferMode |= SDHCI_TM_BLOCK_COUNT_EN | SDHCI_TM_MULTI_BLOCK;
        }
        if (IsRead) {
            TransferMode |= SDHCI_TM_READ;
        }

        //
        // Block size in bits[11:0] and the SDMA buffer boundary in bits[15:12].
        // A register dump of the working Linux configuration has 0x7200 here,
        // i.e. block size 512 with a boundary of 7 (4 KiB). The boundary is only
        // meaningful for DMA, but matching the known-good descriptor exactly
        // removes it as a variable.
        //
        MmioWrite16 (ARASAN_SD_BASE + SDHCI_BLOCK_SIZE_COUNT,
                     SDHCI_BLOCK_BYTE_LENGTH | SDHCI_BLOCK_BOUNDARY_4K);
        MmioWrite16 (ARASAN_SD_BASE + SDHCI_BLOCK_SIZE_COUNT + 2, BlockCount);

        //
        // Point single address DMA at the bounce buffer and start at its
        // beginning. Every block of a multiple block transfer lands after the
        // previous one, and ReadBlockData() advances an index as MmcDxe asks for
        // one block at a time.
        //
        if (!EFI_ERROR (ArasanEnsureDmaBuffer ())) {
            mArasanDmaBlockIndex = 0;
            MmioWrite32 (ARASAN_SD_BASE + SDHCI_DMA_ADDRESS,
                         (UINT32)(UINTN)mArasanDmaBuffer);
            TransferMode |= SDHCI_TM_DMA_ENABLE;
        } else {
            DEBUG ((DEBUG_ERROR, "ArasanSd: no SDMA buffer, falling back to PIO\n"));
        }

        MmioWrite16 (ARASAN_SD_BASE + SDHCI_TRANSFER_MODE, TransferMode);

        Command |= SDHCI_CMD_DATA_PRESENT;

        DEBUG ((
            DEBUG_MMCHOST_SD,
            "ArasanSd: cmd %d data transfer, mode=0x%04x count=%d\n",
            Index, TransferMode, BlockCount));
    } else {
        MmioWrite16 (ARASAN_SD_BASE + SDHCI_TRANSFER_MODE, 0x0000);
    }

    MmioWrite16 (ARASAN_SD_BASE + SDHCI_COMMAND, Command);

    for (Spin = 0; Spin < ARASAN_SD_CMD_POLL_COUNT; Spin++) {
        NormalStatus = MmioRead16 (ARASAN_SD_BASE + SDHCI_NORMAL_INT_STATUS);
        if ((NormalStatus & SDHCI_INT_CMD_COMPLETE) != 0) {
            MmioWrite16 (ARASAN_SD_BASE + SDHCI_NORMAL_INT_STATUS, SDHCI_INT_CMD_COMPLETE);
            return EFI_SUCCESS;
        }

        ErrorStatus = MmioRead16 (ARASAN_SD_BASE + SDHCI_ERROR_INT_STATUS);
        if (ErrorStatus != 0) {
            DEBUG ((
                DEBUG_ERROR,
                "ArasanSd: cmd %d failed, error status=0x%04x, normal=0x%04x\n",
                Index, ErrorStatus, NormalStatus));
            ArasanResetCmdLine ();
            return EFI_DEVICE_ERROR;
        }
    }

    DEBUG ((
        DEBUG_ERROR,
        "ArasanSd: cmd %d (0x%04x, arg 0x%08x) timed out, present=0x%08x clock=0x%04x "
        "normal=0x%04x error=0x%04x hostctl=0x%02x cmdreg=0x%04x\n",
        Index, Command, Argument,
        MmioRead32 (ARASAN_SD_BASE + SDHCI_PRESENT_STATE),
        (UINT32)MmioRead16 (ARASAN_SD_BASE + SDHCI_CLOCK_CONTROL),
        (UINT32)MmioRead16 (ARASAN_SD_BASE + SDHCI_NORMAL_INT_STATUS),
        (UINT32)MmioRead16 (ARASAN_SD_BASE + SDHCI_ERROR_INT_STATUS),
        (UINT32)MmioRead8 (ARASAN_SD_BASE + SDHCI_HOST_CONTROL),
        (UINT32)MmioRead16 (ARASAN_SD_BASE + SDHCI_COMMAND)));
    ArasanResetCmdLine ();
    return EFI_TIMEOUT;
}

EFI_STATUS
ArasanReceiveResponse (
    IN EFI_MMC_HOST_PROTOCOL    *This,
    IN MMC_RESPONSE_TYPE        Type,
    OUT UINT32                  *Buffer
    )
{
    if (Buffer == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    Buffer[0] = MmioRead32 (ARASAN_SD_BASE + SDHCI_RESPONSE_0);
    Buffer[1] = MmioRead32 (ARASAN_SD_BASE + SDHCI_RESPONSE_0 + 4);
    Buffer[2] = MmioRead32 (ARASAN_SD_BASE + SDHCI_RESPONSE_0 + 8);
    Buffer[3] = MmioRead32 (ARASAN_SD_BASE + SDHCI_RESPONSE_0 + 12);

    if (Type == MMC_RESPONSE_TYPE_R2) {
        //
        // Strip the leading start/transmission byte and the CRC the same way the
        // SDHOST backend does, so MmcDxe sees an identically shaped buffer.
        //
        UINT8   *Bytes = (UINT8 *)Buffer;
        UINT32  Idx;

        for (Idx = 0; Idx < (sizeof (UINT32) * 4) - 1; Idx++) {
            Bytes[Idx] = Bytes[Idx + 1];
        }
        Bytes[(sizeof (UINT32) * 4) - 1] = 0;
    }

    DEBUG ((
        DEBUG_MMCHOST_SD,
        "ArasanSd: response type %d: %08x %08x %08x %08x\n",
        Type, Buffer[0], Buffer[1], Buffer[2], Buffer[3]));

    return EFI_SUCCESS;
}

EFI_STATUS
ArasanReadBlockData (
    IN EFI_MMC_HOST_PROTOCOL    *This,
    IN EFI_LBA                  Lba,
    IN UINTN                    Length,
    OUT UINT32                  *Buffer
    )
{
    UINTN    Spin;
    UINTN    Offset;
    UINT8   *Source;
    UINT16   Status;
    UINT16   Error;
    BOOLEAN  Done;

    if (Buffer == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    if (mArasanDmaBuffer == NULL) {
        DEBUG ((DEBUG_ERROR, "ArasanSd: read with no SDMA buffer, LBA 0x%x\n", (UINT32)Lba));
        return EFI_DEVICE_ERROR;
    }

    //
    // MmcDxe sent CMD17/CMD18 and ArasanSendCommand pointed SDMA at the bounce
    // buffer, so this only has to wait for the data and copy the block out.
    //
    // A single block transfer completes as one unit. A multiple block transfer
    // is terminated by the CMD12 that MmcDxe sends after the blocks it wants, so
    // a block is also accepted as soon as the controller reports the buffer
    // readable.
    //
    Done = FALSE;
    for (Spin = 0; Spin < ARASAN_SD_DATA_POLL_COUNT; Spin++) {
        Error = MmioRead16 (ARASAN_SD_BASE + SDHCI_ERROR_INT_STATUS);
        if (Error != 0) {
            DEBUG ((
                DEBUG_ERROR,
                "ArasanSd: read error at LBA 0x%x, error=0x%04x present=0x%08x resp0=0x%08x\n",
                (UINT32)Lba, Error,
                MmioRead32 (ARASAN_SD_BASE + SDHCI_PRESENT_STATE),
                MmioRead32 (ARASAN_SD_BASE + SDHCI_RESPONSE_0)));
            ArasanResetCmdLine ();
            return EFI_DEVICE_ERROR;
        }

        Status = MmioRead16 (ARASAN_SD_BASE + SDHCI_NORMAL_INT_STATUS);
        if ((Status & (SDHCI_INT_XFER_COMPLETE | SDHCI_INT_BUF_READ_READY)) != 0) {
            Done = TRUE;
            break;
        }
    }

    if (!Done) {
        DEBUG ((
            DEBUG_ERROR,
            "ArasanSd: read timed out at LBA 0x%x, normal=0x%04x error=0x%04x "
            "present=0x%08x xfermode=0x%04x cmd=0x%04x block=0x%04x count=0x%04x dma=0x%08x\n",
            (UINT32)Lba,
            (UINT32)MmioRead16 (ARASAN_SD_BASE + SDHCI_NORMAL_INT_STATUS),
            (UINT32)MmioRead16 (ARASAN_SD_BASE + SDHCI_ERROR_INT_STATUS),
            MmioRead32 (ARASAN_SD_BASE + SDHCI_PRESENT_STATE),
            (UINT32)MmioRead16 (ARASAN_SD_BASE + SDHCI_TRANSFER_MODE),
            (UINT32)MmioRead16 (ARASAN_SD_BASE + SDHCI_COMMAND),
            (UINT32)MmioRead16 (ARASAN_SD_BASE + SDHCI_BLOCK_SIZE_COUNT),
            (UINT32)MmioRead16 (ARASAN_SD_BASE + SDHCI_BLOCK_SIZE_COUNT + 2),
            MmioRead32 (ARASAN_SD_BASE + SDHCI_DMA_ADDRESS)));
        ArasanResetCmdLine ();
        return EFI_TIMEOUT;
    }

    Offset = mArasanDmaBlockIndex * SDHCI_BLOCK_BYTE_LENGTH;
    Source = (UINT8 *)mArasanDmaBuffer + Offset;
    CopyMem (Buffer, Source, Length);
    mArasanDmaBlockIndex++;

    MmioWrite16 (ARASAN_SD_BASE + SDHCI_NORMAL_INT_STATUS,
                 SDHCI_INT_XFER_COMPLETE | SDHCI_INT_BUF_READ_READY);

    //
    // The last four words cover offsets 0x1F0..0x1FF of the block, where a
    // partition table lives: the MBR signature is 0x55 0xAA at 0x1FE, so
    // (last >> 16) == 0xAA55 proves the data path and the addressing.
    //
    DEBUG ((
        DEBUG_INIT,
        "ArasanSd: read LBA 0x%x, %d bytes, first=0x%08x tail=%08x %08x %08x %08x\n",
        (UINT32)Lba, (UINT32)Length, Buffer[0],
        Buffer[(Length / 4) - 4], Buffer[(Length / 4) - 3],
        Buffer[(Length / 4) - 2], Buffer[(Length / 4) - 1]));

    return EFI_SUCCESS;
}

EFI_STATUS
ArasanWriteBlockData (
    IN EFI_MMC_HOST_PROTOCOL    *This,
    IN EFI_LBA                  Lba,
    IN UINTN                    Length,
    IN UINT32                   *Buffer
    )
{
    DEBUG ((DEBUG_ERROR, "ArasanSd: WriteBlockData not implemented yet (LBA 0x%x, %d bytes)\n",
            (UINT32)Lba, (UINT32)Length));
    return EFI_UNSUPPORTED;
}

/** @file
  Serial I/O Port library functions with no library constructor/destructor

  Copyright (c), Microsoft Corporation. All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

//
// This console driver talks to the BCM283x/BCM2711 PL011 UART at
// PcdSerialRegisterBase (0xFE201000 on the high peripheral window), which is
// brought out on GPIO14 (TXD0) / GPIO15 (RXD0).
//
// It used to drive the AUX mini UART instead. The mini UART is clocked from the
// VideoCore core clock, so its baud rate divisor is only correct for as long as
// that clock does not change; on BCM2711 the firmware retunes it (as a side
// effect of changing the ARM clock, and through its own DVFS), which turns the
// console into bursts of framing-error garbage part way through the boot. The
// PL011 is fed from the dedicated 48 MHz UART clock, so the divisor stays valid.
//

#include <Uefi.h>

#include <Base.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/SerialPortLib.h>
#include <Library/PcdLib.h>
#include <Library/IoLib.h>
#include <Bcm2836.h>
#include <BcmMailbox.h>

//
// PL011 register offsets
//
#define PL011_UARTDR        0x00
#define PL011_UARTFR        0x18
#define PL011_UARTIBRD      0x24
#define PL011_UARTFBRD      0x28
#define PL011_UARTLCRH      0x2C
#define PL011_UARTCR        0x30
#define PL011_UARTIMSC      0x38
#define PL011_UARTICR       0x44

//
// PL011_UARTFR bits
//
#define PL011_FR_TXFF       (1 << 5)   // Transmit FIFO full
#define PL011_FR_RXFE       (1 << 4)   // Receive FIFO empty

//
// PL011_UARTLCRH bits: 8 data bits, FIFO enabled
//
#define PL011_LCRH_WLEN_8   (3 << 5)
#define PL011_LCRH_FEN      (1 << 4)

//
// PL011_UARTCR bits
//
#define PL011_CR_UARTEN     (1 << 0)
#define PL011_CR_TXE        (1 << 8)
#define PL011_CR_RXE        (1 << 9)

#define PL011_BAUD_CLOCK_HZ (48000000)

//
// Offset of the UART clock in the VideoCore clock tree, requested explicitly so
// the console does not depend on "enable_uart"/"init_uart_clock" in config.txt.
//
#define PL011_FIXED_BAUD_RATE 115200

/**
  Program the PL011.

  @return RETURN_SUCCESS in all cases; failures to talk to the firmware are not
          fatal because the UART may already be clocked and configured.
**/
RETURN_STATUS
EFIAPI
SerialPortInitialize (
  VOID
  )
{
    static BOOLEAN  g_bInitComplete = FALSE;
    UINT32          UartBase;
    UINT32          UartClkInHz;
    UINT32          BaudRate;
    UINT32          Divisor;

    if (g_bInitComplete) {
        return RETURN_SUCCESS;
    }

    UartBase    = (UINT32)FixedPcdGet64 (PcdSerialRegisterBase);
    UartClkInHz = FixedPcdGet32 (PL011UartClkInHz);
    BaudRate    = (UINT32)FixedPcdGet64 (PcdUartDefaultBaudRate);
    if (BaudRate == 0) {
        BaudRate = PL011_FIXED_BAUD_RATE;
    }

    //
    // Make sure the VideoCore drives the UART clock at the rate we divide by.
    // This is best effort: on a firmware that already set it up the request is a
    // no-op, and a failure here is not fatal.
    //
    {
        MAILBOX_SET_CLOCK_RATE MbSetUartCr;
        EFI_STATUS             Status;

        ZeroMem ((void*)&MbSetUartCr, sizeof (MbSetUartCr));
        MbSetUartCr.Header.BufferSize = sizeof (MbSetUartCr);
        MbSetUartCr.Header.TagID      = TAG_SET_CLOCK_RATE;
        MbSetUartCr.Header.TagLength  = 12;
        MbSetUartCr.ClockId           = CLOCK_ID_UART;
        MbSetUartCr.Rate              = PL011_BAUD_CLOCK_HZ;
        MbSetUartCr.SkipSettingTurbo  = 1;

        Status = MailboxProperty (
                    MAILBOX_CHANNEL_PROPERTY_ARM_VC,
                    (MAILBOX_HEADER*)&MbSetUartCr
                    );
        if (EFI_ERROR (Status)) {
            DEBUG ((DEBUG_WARN, "Failed to set the PL011 clock to 48MHz\n"));
        }
    }

    //
    // Disable the UART while it is being reconfigured.
    //
    MmioWrite32 (UartBase + PL011_UARTCR, 0);

    //
    // Baud rate: fractional divider, Divisor = (clock * 4) / baud with rounding,
    // IBRD takes the integer part and FBRD the 1/64 fraction.
    //
    Divisor = ((UartClkInHz * 4) + (BaudRate / 2)) / BaudRate;
    MmioWrite32 (UartBase + PL011_UARTIBRD, Divisor >> 6);
    MmioWrite32 (UartBase + PL011_UARTFBRD, Divisor & 0x3F);

    //
    // 8 data bits, no parity, one stop bit, FIFOs enabled.
    //
    if (FixedPcdGet8 (PcdUartDefaultDataBits) != 8 ||
        FixedPcdGet8 (PcdUartDefaultParity)    != 1 ||
        FixedPcdGet8 (PcdUartDefaultStopBits)  != 1) {
        return RETURN_INVALID_PARAMETER;
    }
    MmioWrite32 (UartBase + PL011_UARTLCRH, PL011_LCRH_FEN | PL011_LCRH_WLEN_8);

    //
    // Mask all interrupts and clear anything pending: this library polls.
    //
    MmioWrite32 (UartBase + PL011_UARTIMSC, 0);
    MmioWrite32 (UartBase + PL011_UARTICR, 0x7FF);

    //
    // Enable the UART, transmitter and receiver.
    //
    MmioWrite32 (UartBase + PL011_UARTCR, PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE);

    /* Test output */
    SerialPortWrite ((UINT8*) "PL011\r\n", 7);

    g_bInitComplete = TRUE;

    return RETURN_SUCCESS;
}


/**
  Write data to serial device.

  @param  Buffer           Point of data buffer which need to be written.
  @param  NumberOfBytes    Number of output bytes which are cached in Buffer.

  @retval 0                Write data failed.
  @retval !0               Actual number of bytes written to serial device.

**/
UINTN
EFIAPI
SerialPortWrite (
  IN UINT8     *Buffer,
  IN UINTN     NumberOfBytes
  )
{
    UINTN   Count;
    UINT32  UartBase;

    UartBase = (UINT32)FixedPcdGet64 (PcdSerialRegisterBase);

    for (Count = 0; Count < NumberOfBytes; Count++, Buffer++)
    {
        /* Wait until there is room in the transmit FIFO */
        while ((MmioRead32 (UartBase + PL011_UARTFR) & PL011_FR_TXFF) != 0)
        {
        }

        MmioWrite32 (UartBase + PL011_UARTDR, *Buffer);
    }

    return NumberOfBytes;
}


/**
  Read data from serial device and save the data in buffer.

  @param  Buffer           Point of data buffer which need to be written.
  @param  NumberOfBytes    Number of output bytes which are cached in Buffer.

  @retval 0                Read data failed.
  @retval !0               Actual number of bytes read to serial device.

**/
UINTN
EFIAPI
SerialPortRead (
  OUT UINT8     *Buffer,
  IN  UINTN     NumberOfBytes
  )
{
    UINTN   Count;
    UINT32  UartBase;

    UartBase = (UINT32)FixedPcdGet64 (PcdSerialRegisterBase);

    for (Count = 0; Count < NumberOfBytes; Count++, Buffer++)
    {
        /* Wait for a character in the receive FIFO */
        while ((MmioRead32 (UartBase + PL011_UARTFR) & PL011_FR_RXFE) != 0)
        {
        }

        *Buffer = (UINT8)(MmioRead32 (UartBase + PL011_UARTDR) & 0xFF);
    }

    return NumberOfBytes;
}


/**
  Check to see if any data is available to be read from the debug device.
**/
BOOLEAN
EFIAPI
SerialPortPoll (
  VOID
  )
{
    UINT32  UartBase;

    UartBase = (UINT32)FixedPcdGet64 (PcdSerialRegisterBase);

    if ((MmioRead32 (UartBase + PL011_UARTFR) & PL011_FR_RXFE) == 0)
    {
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

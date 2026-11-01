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
#define PL011_FR_BUSY       (1 << 3)   // UART busy transmitting
#define PL011_FR_TXFE       (1 << 7)   // Transmit FIFO empty

//
// PL011_UARTLCRH bits: 8 data bits, FIFO enabled
//
#define PL011_LCRH_WLEN_8   (3 << 5)
#define PL011_LCRH_FEN      (1 << 4)

//
// PL011_UARTCR bits
//
#define PL011_CR_UARTEN     (1 << 0)
#define PL011_CR_LBE        (1 << 7)   // Loop back enable (TX fed into RX)
#define PL011_CR_TXE        (1 << 8)
#define PL011_CR_RXE        (1 << 9)

#define PL011_BAUD_CLOCK_HZ (48000000)

//
// Console receive diagnostics (temporary bring-up aids, all off).
//
// SERIAL_RX_LOOPBACK_TEST: bounded internal loopback check at init.
// SERIAL_RX_ECHO:          print any byte that arrives on RX as "[hh]".
// SERIAL_RX_STATE_DUMP:    print the GPIO/PL011 registers once per init.
// SERIAL_RX_POLL_TRACE:    report polling and any pending byte with its flags.
//
// What the investigation on a Pi 4 established:
//  - the PL011, its GPIO14/15 muxing (GPFSEL1 = 0x00024000), the RX pad level
//    (GPLEV0 bit 15 idles high) and the internal loopback path are all fine;
//  - TerminalDxe reads keys via SerialPortGetControl()/SerialPortPoll() from an
//    interrupt driven timer event, and that whole chain works;
//  - the "no keyboard response" was caused by an earlier version of this file
//    reading the data register from inside SerialPortWrite, which stole every
//    byte before SerialDxe could see it;
//  - the only remaining oddity is an occasional burst of bytes that arrive with
//    no error flags while the adapter reports nothing transmitted, and whose
//    content looks like our own output sampled with a phase offset. That is a
//    wiring or crosstalk effect rather than firmware.
//
#define SERIAL_RX_DIAG            0
#define SERIAL_RX_LOOPBACK_TEST   0
#define SERIAL_RX_ECHO            0
#define SERIAL_RX_STATE_DUMP      0
#define SERIAL_RX_POLL_TRACE      0

//
// Offset of the UART clock in the VideoCore clock tree, requested explicitly so
// the console does not depend on "enable_uart"/"init_uart_clock" in config.txt.
//
#define PL011_FIXED_BAUD_RATE 115200

#if SERIAL_RX_DIAG
/**
  Write one character straight to the data register.

  Used by the diagnostics below so they never recurse back into SerialPortWrite.
**/
STATIC
VOID
SerialPortPutChar (
  IN UINT32  UartBase,
  IN CHAR8   Ch
  )
{
    while ((MmioRead32 (UartBase + PL011_UARTFR) & PL011_FR_TXFF) != 0) {
    }
    MmioWrite32 (UartBase + PL011_UARTDR, (UINT32)(UINT8)Ch);
}
#endif

/**
  Write a 16 bit value as four upper case hex digits.

  The PL011 data register carries the receive error flags in bits 11:8 (OE, BE,
  PE, FE), so printing all 16 bits of what was read tells us both the byte and
  whether it was received cleanly.
**/
#if SERIAL_RX_DIAG && (SERIAL_RX_LOOPBACK_TEST || SERIAL_RX_STATE_DUMP || SERIAL_RX_POLL_TRACE)
STATIC
VOID
SerialPortPutHex32 (
  IN UINT32  UartBase,
  IN UINT32  Value
  )
{
    STATIC CONST CHAR8  Hex[] = "0123456789ABCDEF";
    INTN                Shift;

    for (Shift = 28; Shift >= 0; Shift -= 4) {
        SerialPortPutChar (UartBase, Hex[(Value >> Shift) & 0xF]);
    }
}
#endif

#if SERIAL_RX_DIAG && (SERIAL_RX_ECHO || SERIAL_RX_POLL_TRACE || SERIAL_RX_STATE_DUMP)
/**
  Write a NUL terminated string, computing the length here so the diagnostics do
  not have to hard code it.
**/
STATIC
VOID
SerialPortPutString (
  IN UINT32        UartBase,
  IN CONST CHAR8  *Str
  )
{
    while (*Str != 0) {
        SerialPortPutChar (UartBase, *Str);
        Str++;
    }
}
#endif

#if SERIAL_RX_DIAG && SERIAL_RX_STATE_DUMP
/**
  Dump the state that decides whether an external byte can reach the receiver:
  the GPIO function select, the actual level seen on the RX pin, and the UART
  control/line-control/flag registers.

  GPLEV0 bit 15 is the interesting one: with the adapter's transmit line idling
  high (which it must, for the PL011 to see an idle stop bit) it should read 1.
  If it reads 0 while Linux receives fine, the pad is not being driven the way we
  think and the mux or the pad configuration is at fault.
**/
STATIC
VOID
SerialPortDumpState (
  IN UINT32  UartBase
  )
{
    SerialPortPutString (UartBase, "\r\nPL011 state: GPFSEL1=0x");
    SerialPortPutHex32 (UartBase, MmioRead32 (GPIO_GPFSEL1));
    SerialPortPutString (UartBase, " GPLEV0=0x");
    SerialPortPutHex32 (UartBase, MmioRead32 (GPIO_BASE_ADDRESS + 0x34));
    SerialPortPutString (UartBase, "\r\n             CR=0x");
    SerialPortPutHex32 (UartBase, MmioRead32 (UartBase + PL011_UARTCR));
    SerialPortPutString (UartBase, " LCRH=0x");
    SerialPortPutHex32 (UartBase, MmioRead32 (UartBase + PL011_UARTLCRH));
    SerialPortPutString (UartBase, " FR=0x");
    SerialPortPutHex32 (UartBase, MmioRead32 (UartBase + PL011_UARTFR));
    SerialPortPutString (UartBase, "\r\n");
}
#endif

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

#if SERIAL_RX_LOOPBACK_TEST
    //
    // Internal loopback self test: with LBE set the PL011 feeds its own
    // transmitter back into its receiver, so this checks the whole receive path
    // (RX FIFO, data register, FR.RXFE, our polling) without needing anything
    // wired to GPIO15. If this passes but no key ever arrives, the problem is
    // outside the SoC: the adapter's TX is not reaching GPIO15, or the cable is
    // transmit-only.
    //
    {
        UINT32  Result;
        UINT32  Received;
        UINTN   Spin;

        //
        // Clear both FIFOs first (toggling FEN resets them on the PL011) so the
        // byte read back below is definitely the one this test sends, not a
        // leftover from earlier output.
        //
        MmioWrite32 (UartBase + PL011_UARTLCRH, PL011_LCRH_WLEN_8);
        MmioWrite32 (UartBase + PL011_UARTLCRH, PL011_LCRH_FEN | PL011_LCRH_WLEN_8);

        //
        // Let the transmitter finish draining before switching the loopback on.
        // SerialPortWrite() only waits for TXFF, so up to a FIFO worth of
        // characters can still be shifting out; with LBE set those would be fed
        // back into the receiver and read instead of the test byte.
        //
        while ((MmioRead32 (UartBase + PL011_UARTFR) & (PL011_FR_TXFE | PL011_FR_BUSY)) != PL011_FR_TXFE) {
        }

        MmioWrite32 (UartBase + PL011_UARTCR,
                     PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE | PL011_CR_LBE);
        SerialPortPutChar (UartBase, (CHAR8)0x55);

        Result   = 0;
        Received = 0;
        for (Spin = 0; Spin < 1000000; Spin++) {
            if ((MmioRead32 (UartBase + PL011_UARTFR) & PL011_FR_RXFE) == 0) {
                Received = MmioRead32 (UartBase + PL011_UARTDR);
                Result   = ((Received & 0xFF) == 0x55) ? 1 : 2;
                break;
            }
        }

        MmioWrite32 (UartBase + PL011_UARTCR,
                     PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE);

        //
        // Report the whole data register, not just the byte: bits 11:8 carry the
        // overrun/break/parity/framing flags, so this distinguishes "loopback is
        // broken" from "the loopback byte picked up an error".
        //
        SerialPortPutString (UartBase, "PL011 loopback: sent 0x55, RX DR=0x");
        SerialPortPutHex32 (UartBase, Received);
        if (Result == 1) {
            SerialPortPutString (UartBase, " OK\r\n");
        } else if (Result == 2) {
            SerialPortPutString (UartBase, " MISMATCH\r\n");
        } else {
            SerialPortPutString (UartBase, " NO-BYTE\r\n");
        }
    }
#endif

#if SERIAL_RX_STATE_DUMP
    SerialPortDumpState (UartBase);
#endif

    g_bInitComplete = TRUE;

    return RETURN_SUCCESS;
}


/**
  Diagnostic: if a byte has arrived on the PL011, print it as "[hh]".

  This answers, without any doubt, whether the receive path works at all. It is
  called from the transmit path because that is where the firmware spends its
  time, so any key pressed while the console is being written shows up in the
  log. Note that it consumes the byte, so the normal console input path will not
  see it; that does not matter while input is broken anyway.

  Set SERIAL_RX_ECHO to 0 to take the diagnostic out.
**/
#if SERIAL_RX_ECHO
STATIC
VOID
SerialPortEchoReceivedByte (
  IN UINT32  UartBase
  )
{
    UINT8  Ch;
    CHAR8  Text[4];
    UINTN  Index;
    STATIC UINTN  EchoCount = 0;

    //
    // Cap the number of echoed bytes: with GPIO14 shorted to GPIO15 (the
    // external loopback test) every transmitted byte comes straight back, and
    // echoing those would otherwise feed itself and flood the console.
    //
    if (EchoCount >= 200) {
        return;
    }

    if ((MmioRead32 (UartBase + PL011_UARTFR) & PL011_FR_RXFE) != 0) {
        return;
    }

    EchoCount++;
    Ch = (UINT8)(MmioRead32 (UartBase + PL011_UARTDR) & 0xFF);

    Text[0] = '[';
    Text[1] = (CHAR8)((Ch >> 4) < 10 ? ('0' + (Ch >> 4)) : ('A' + (Ch >> 4) - 10));
    Text[2] = (CHAR8)((Ch & 0x0F) < 10 ? ('0' + (Ch & 0x0F)) : ('A' + (Ch & 0x0F) - 10));
    Text[3] = ']';

    // Write straight to the data register; calling SerialPortWrite here would recurse.
    for (Index = 0; Index < 4; Index++) {
        SerialPortPutChar (UartBase, Text[Index]);
    }
}
#endif


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
#if SERIAL_RX_ECHO
        SerialPortEchoReceivedByte (UartBase);
#endif
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
#if SERIAL_RX_POLL_TRACE
        //
        // Consume the byte and report the whole data register. Bits 11:8 carry
        // OE (overrun), BE (break), PE (parity error) and FE (framing error),
        // which is what distinguishes "the adapter really sent something" from
        // "the receiver is mis-sampling an idle line".
        //
        // Consuming it also keeps these spurious bytes away from BDS, which
        // otherwise echoes a "?" for each of them.
        //
        STATIC UINTN  Hits = 0;

        if (Hits < 20) {
            UINT32  Data;
            UINT32  Flags;

            Data  = MmioRead32 (UartBase + PL011_UARTDR);
            Flags = (Data >> 8) & 0xF;
            Hits++;

            SerialPortPutString (UartBase, "[rx data=0x");
            SerialPortPutHex32 (UartBase, Data & 0xFF);
            SerialPortPutString (UartBase, " flags=");
            SerialPortPutHex32 (UartBase, Flags);
            SerialPortPutString (UartBase, "]");
        }
#endif
        return TRUE;
    }
    else
    {
#if SERIAL_RX_POLL_TRACE
        //
        // Unconditional (but bounded) probe: proves whether anything polls the
        // serial port at all. Without this, "no data ready" and "nobody ever
        // calls Poll" look identical from the outside.
        //
        STATIC UINTN  Polls = 0;

        if (Polls < 20) {
            Polls++;
            SerialPortPutString (UartBase, "[poll]");
        }
#endif
        return FALSE;
    }
}

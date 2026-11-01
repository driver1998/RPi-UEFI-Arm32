/** @file
*
*  Header defining the Pi2Board constants (Base addresses, sizes, flags)
*
*  Copyright (c), Microsoft Corporation. All rights reserved.
*
*  This program and the accompanying materials
*  are licensed and made available under the terms and conditions of the BSD License
*  which accompanies this distribution.  The full text of the license may be found at
*  http://opensource.org/licenses/bsd-license.php
*
*  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
*  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
*
**/

#ifndef __PI2BOARD_PLATFORM_H__
#define __PI2BOARD_PLATFORM_H__

// DDR attributes
#define DDR_ATTRIBUTES_CACHED             ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK
#define DDR_ATTRIBUTES_UNCACHED           ARM_MEMORY_REGION_ATTRIBUTE_UNCACHED_UNBUFFERED

#define BOOT_IMAGE_ATTRIBUTES             CacheAttributes

// Main system DRAM as defined by the PCD definitions of system memory.

// Peripheral register block
//
// NOTE: On BCM2711 (Pi 4) the "high peripheral" window is 32 MB wide and spans
// 0xFE000000-0xFFFFFFFF. Besides the legacy BCM283x peripherals (UART, GPIO,
// SD/eMMC, ...) that window also contains what on BCM2836 lived in a separate
// 256 MB region:
//   0xFF800000  ARM local peripherals (mailboxes, IRQ/FIQ source registers)
//   0xFF841000  GIC-400 distributor
//   0xFF842000  GIC-400 CPU interface
// With the window only 16 MB wide (ending at 0xFF000000) the GIC stays unmapped,
// so the first GIC register access after the MMU has been enabled takes a
// translation fault and the platform hangs without any output.
#define SOC_REGISTERS_PHYSICAL_BASE       0xFE000000
#define SOC_REGISTERS_PHYSICAL_LENGTH     0x02000000
#define SOC_REGISTERS_ATTRIBUTES          ARM_MEMORY_REGION_ATTRIBUTE_DEVICE

// ARM Core Peripheral register block
//
// On BCM2711 the ARM local peripherals moved from 0x40000000 to 0xFF800000.
// The descriptor is kept so the memory map layout stays unchanged; it lies
// inside the SOC_REGISTERS window above, which is harmless because both are
// mapped as device memory.
#define ARM_REGISTERS_PHYSICAL_BASE       0xFF800000
#define ARM_REGISTERS_PHYSICAL_LENGTH     0x00800000
#define ARM_REGISTERS_ATTRIBUTES          ARM_MEMORY_REGION_ATTRIBUTE_DEVICE

// GPU Shared memory block
// GPU_SHARED_MEMORY_PHYSICAL_LENGTH must match the System Memory PCD & Pi2 
// boot "config.txt" settings for the GPU memory.
// 
// Note : Memory returned by the GPU (e.g. Framebuffer) is tagged 0xC0000000
// to make it physically none cachable. Address needs to be stripped to make
// it an ARM accessible address (e.g. 0xDE000000 --> 0x1E000000).

#define GPU_SHARED_MEMORY_PHYSICAL_LENGTH       0x02000000
#define GPU_SHARED_MEMORY_ATTRIBUTES            ARM_MEMORY_REGION_ATTRIBUTE_DEVICE

// RPi Board Information
#define RPI2_BOARD_MEMORY_1024MB                0x00000001

// List of board that only supports 1 UART
// - All 1024Mb board is expected to only support 1 UART

#define BOARD_SUPPORT_SINGLE_UART(a) \
    (a == RPI2_BOARD_MEMORY_1024MB) \

#endif

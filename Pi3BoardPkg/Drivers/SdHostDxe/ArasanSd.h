/** @file
  Interface of the Arasan SDHCI backend for EFI_MMC_HOST_PROTOCOL.

  Copyright (c), Microsoft Corporation. All rights reserved.

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#ifndef __ARASAN_SD_H__
#define __ARASAN_SD_H__

#include <Uefi.h>
#include <Protocol/MmcHost.h>

/**
  Reset the controller, power the bus at 3.3 V and clock it at the identification
  frequency. Safe to call more than once.
**/
EFI_STATUS
ArasanSdInitialize (
  VOID
  );

/**
  TRUE once ArasanSdInitialize() has completed successfully.
**/
BOOLEAN
ArasanIsInitialized (
  VOID
  );

EFI_STATUS
ArasanNotifyState (
    IN EFI_MMC_HOST_PROTOCOL    *This,
    IN MMC_STATE                State
    );

EFI_STATUS
ArasanSendCommand (
    IN EFI_MMC_HOST_PROTOCOL    *This,
    IN MMC_CMD                  MmcCmd,
    IN UINT32                   Argument
    );

EFI_STATUS
ArasanReceiveResponse (
    IN EFI_MMC_HOST_PROTOCOL    *This,
    IN MMC_RESPONSE_TYPE        Type,
    OUT UINT32                  *Buffer
    );

EFI_STATUS
ArasanReadBlockData (
    IN EFI_MMC_HOST_PROTOCOL    *This,
    IN EFI_LBA                  Lba,
    IN UINTN                    Length,
    OUT UINT32                  *Buffer
    );

EFI_STATUS
ArasanWriteBlockData (
    IN EFI_MMC_HOST_PROTOCOL    *This,
    IN EFI_LBA                  Lba,
    IN UINTN                    Length,
    IN UINT32                   *Buffer
    );

#endif // __ARASAN_SD_H__

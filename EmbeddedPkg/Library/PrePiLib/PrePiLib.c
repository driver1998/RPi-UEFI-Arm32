/** @file

  Copyright (c) 2008 - 2009, Apple Inc. All rights reserved.<BR>

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include <PrePi.h>

//
// Hack to work in NT32
//
EFI_STATUS

EFIAPI

SecWinNtPeiLoadFile (
  IN  VOID                    *Pe32Data,
  IN  EFI_PHYSICAL_ADDRESS    *ImageAddress,
  IN  UINT64                  *ImageSize,
  IN  EFI_PHYSICAL_ADDRESS    *EntryPoint
  );


EFI_STATUS
EFIAPI
LoadPeCoffImage (
  IN  VOID                                      *PeCoffImage,
  OUT EFI_PHYSICAL_ADDRESS                      *ImageAddress,
  OUT UINT64                                    *ImageSize,
  OUT EFI_PHYSICAL_ADDRESS                      *EntryPoint
  )
{
  RETURN_STATUS                 Status;
  PE_COFF_LOADER_IMAGE_CONTEXT  ImageContext;
  VOID                           *Buffer;

  ZeroMem (&ImageContext, sizeof (ImageContext));

  ImageContext.Handle    = PeCoffImage;
  ImageContext.ImageRead = PeCoffLoaderImageReadFromMemory;

  Status = PeCoffLoaderGetImageInfo (&ImageContext);
  DEBUG ((DEBUG_INIT, "    LoadPeCoffImage: GetImageInfo returned 0x%x, size 0x%x\n",
          (UINT32)Status, (UINT32)ImageContext.ImageSize));
  ASSERT_EFI_ERROR (Status);


  //
  // Allocate Memory for the image
  //
  Buffer = AllocatePages (EFI_SIZE_TO_PAGES((UINT32)ImageContext.ImageSize));
  DEBUG ((DEBUG_INIT, "    LoadPeCoffImage: buffer 0x%08x\n", (UINT32)(UINTN)Buffer));
  ASSERT (Buffer != 0);


  ImageContext.ImageAddress = (EFI_PHYSICAL_ADDRESS)(UINTN)Buffer;

  //
  // Load the image to our new buffer
  //
  Status = PeCoffLoaderLoadImage (&ImageContext);
  DEBUG ((DEBUG_INIT, "    LoadPeCoffImage: LoadImage returned 0x%x\n", (UINT32)Status));
  ASSERT_EFI_ERROR (Status);

  //
  // Relocate the image in our new buffer
  //
  Status = PeCoffLoaderRelocateImage (&ImageContext);
  DEBUG ((DEBUG_INIT, "    LoadPeCoffImage: RelocateImage returned 0x%x\n", (UINT32)Status));
  ASSERT_EFI_ERROR (Status);


  *ImageAddress = ImageContext.ImageAddress;
  *ImageSize    = ImageContext.ImageSize;
  *EntryPoint   = ImageContext.EntryPoint;

  //
  // Flush not needed for all architectures. We could have a processor specific
  // function in this library that does the no-op if needed.
  //
  InvalidateInstructionCacheRange ((VOID *)(UINTN)*ImageAddress, (UINTN)*ImageSize);

  return Status;
}



typedef
VOID
(EFIAPI *DXE_CORE_ENTRY_POINT) (
  IN  VOID *HobStart
  );

EFI_STATUS
EFIAPI
LoadDxeCoreFromFfsFile (
  IN EFI_PEI_FILE_HANDLE  FileHandle,
  IN UINTN                StackSize
  )
{
  EFI_STATUS              Status;
  VOID                    *PeCoffImage;
  EFI_PHYSICAL_ADDRESS    ImageAddress;
  UINT64                  ImageSize;
  EFI_PHYSICAL_ADDRESS    EntryPoint;
  VOID                    *BaseOfStack;
  VOID                    *TopOfStack;
  VOID                    *Hob;
  EFI_FV_FILE_INFO        FvFileInfo;

  DEBUG ((DEBUG_INIT, "  LoadDxeCoreFromFfsFile: FfsFindSectionData\n"));
  Status = FfsFindSectionData (EFI_SECTION_PE32, FileHandle, &PeCoffImage);
  if (EFI_ERROR  (Status)) {
    return Status;
  }
  DEBUG ((DEBUG_INIT, "  LoadDxeCoreFromFfsFile: PE32 section at 0x%08x\n", (UINT32)(UINTN)PeCoffImage));


  DEBUG ((DEBUG_INIT, "  LoadDxeCoreFromFfsFile: LoadPeCoffImage, start\n"));
  Status = LoadPeCoffImage (PeCoffImage, &ImageAddress, &ImageSize, &EntryPoint);
// For NT32 Debug  Status = SecWinNtPeiLoadFile (PeCoffImage, &ImageAddress, &ImageSize, &EntryPoint);
  DEBUG ((
    DEBUG_INIT,
    "  LoadDxeCoreFromFfsFile: LoadPeCoffImage returned 0x%x, image 0x%08x size 0x%x entry 0x%08x\n",
    Status, (UINT32)ImageAddress, (UINT32)ImageSize, (UINT32)EntryPoint));
  ASSERT_EFI_ERROR (Status);

  //
  // Extract the DxeCore GUID file name.
  //
  DEBUG ((DEBUG_INIT, "  LoadDxeCoreFromFfsFile: FfsGetFileInfo\n"));
  Status = FfsGetFileInfo (FileHandle, &FvFileInfo);
  ASSERT_EFI_ERROR (Status);

  DEBUG ((DEBUG_INIT, "  LoadDxeCoreFromFfsFile: BuildModuleHob\n"));
  BuildModuleHob (&FvFileInfo.FileName, (EFI_PHYSICAL_ADDRESS)(UINTN)ImageAddress, EFI_SIZE_TO_PAGES ((UINT32) ImageSize) * EFI_PAGE_SIZE, EntryPoint);

  DEBUG ((EFI_D_INFO | EFI_D_LOAD, "Loading DxeCore at 0x%10p EntryPoint=0x%10p\n", (VOID *)(UINTN)ImageAddress, (VOID *)(UINTN)EntryPoint));

  Hob = GetHobList ();
  if (StackSize == 0) {
    // User the current stack

    ((DXE_CORE_ENTRY_POINT)(UINTN)EntryPoint) (Hob);
  } else {

    //
    // Allocate 128KB for the Stack
    //
    BaseOfStack = AllocatePages (EFI_SIZE_TO_PAGES (StackSize));
    ASSERT (BaseOfStack != NULL);

    //
    // Compute the top of the stack we were allocated. Pre-allocate a UINTN
    // for safety.
    //
    TopOfStack = (VOID *) ((UINTN) BaseOfStack + EFI_SIZE_TO_PAGES (StackSize) * EFI_PAGE_SIZE - CPU_STACK_ALIGNMENT);
    TopOfStack = ALIGN_POINTER (TopOfStack, CPU_STACK_ALIGNMENT);

    //
    // Update the contents of BSP stack HOB to reflect the real stack info passed to DxeCore.
    //
    UpdateStackHob ((EFI_PHYSICAL_ADDRESS)(UINTN) BaseOfStack, StackSize);

    SwitchStack (
      (SWITCH_STACK_ENTRY_POINT)(UINTN)EntryPoint,
      Hob,
      NULL,
      TopOfStack
      );

  }

  // Should never get here as DXE Core does not return
  DEBUG ((EFI_D_ERROR, "DxeCore returned\n"));
  ASSERT (FALSE);

  return EFI_DEVICE_ERROR;
}



EFI_STATUS
EFIAPI
LoadDxeCoreFromFv (
  IN UINTN  *FvInstance,   OPTIONAL
  IN UINTN  StackSize
  )
{
  EFI_STATUS          Status;
  EFI_PEI_FV_HANDLE   VolumeHandle;
  EFI_PEI_FILE_HANDLE FileHandle = NULL;

  if (FvInstance != NULL) {
    //
    // Caller passed in a specific FV to try, so only try that one
    //
    Status = FfsFindNextVolume (*FvInstance, &VolumeHandle);
    if (!EFI_ERROR (Status)) {
      Status = FfsFindNextFile (EFI_FV_FILETYPE_DXE_CORE, VolumeHandle, &FileHandle);
    }
  } else {
    Status = FfsAnyFvFindFirstFile (EFI_FV_FILETYPE_DXE_CORE, &VolumeHandle, &FileHandle);
  }

  if (!EFI_ERROR (Status)) {
    return LoadDxeCoreFromFfsFile (FileHandle, StackSize);
  }

  return Status;
}


EFI_STATUS
EFIAPI
DecompressFirstFv (
  VOID
  )
{
  EFI_STATUS          Status;
  EFI_PEI_FV_HANDLE   VolumeHandle;
  EFI_PEI_FILE_HANDLE FileHandle;

  Status = FfsAnyFvFindFirstFile (EFI_FV_FILETYPE_FIRMWARE_VOLUME_IMAGE, &VolumeHandle, &FileHandle);
  if (!EFI_ERROR (Status)) {
    Status = FfsProcessFvFile (FileHandle);
  }

  return Status;
}



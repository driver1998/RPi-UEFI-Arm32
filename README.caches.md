# Caches on BCM2711 (Raspberry Pi 4), ARM32 EDK2 port

Current state: **both caches are disabled.** The port boots to BDS, brings up the
eMMC2 SD controller and identifies the card in that configuration. Everything in
this note is about what happens when the caches are turned on, and what has
already been ruled out, so the next person does not repeat the bisect.

Switches:

- `ArmPkg/Library/ArmLib/ArmV7/ArmV7Mmu.c`: `ENABLE_DATA_CACHE`,
  `ENABLE_INSTRUCTION_CACHE`
- `Pi2BoardPkg/Include/Pi2Board.h`: `DDR_ATTRIBUTES_CACHED`, currently
  `ARM_MEMORY_REGION_ATTRIBUTE_WRITE_THROUGH`

## What the cache configuration does

- MMU on, DRAM Write-Through: no memory corruption. The dcache self test inside
  `ArmConfigureMmu` passes (64 words written and read back, zero wrong) and the
  read-only string printed right after it is intact.
- MMU on, DRAM Write-Back: read-only strings start coming back modified a few
  prints into the boot, i.e. memory really is clobbered. Marking the mappings
  Inner Shareable changed nothing. Write-Back is therefore not an option until
  the underlying cause is understood.
- Instruction cache: setting `SCTLR.I` used to stall the core instantly with no
  output when the MMU was already on. It now behaves, and with both caches
  enabled the boot gets considerably further (see below).

## Where it stops with caches on

With data cache Write-Through and instruction cache on:

```
DecompressFirstFv, start
DecompressFirstFv, returned 0x0      <- LZMA decompression succeeds
LoadDxeCoreFromFv
  LoadDxeCoreFromFfsFile: FfsFindSectionData
  LoadDxeCoreFromFfsFile: PE32 section at 0x3AC14064
  LoadDxeCoreFromFfsFile: LoadPeCoffImage, start
    LoadPeCoffImage: GetImageInfo returned 0x0, size 0x23CE0
    LoadPeCoffImage: buffer 0x3ABF0000
     PeCoffLoaderLoadImage: context copied
     PeCoffLoaderLoadImage: inner GetImageInfo returned 0x0
     PeCoffLoaderLoadImage: reading headers, SizeOfHeaders 0x240, hdr offset 0x80, te 0
```

The last line is printed immediately before this call, and the print that follows
it never appears:

```c
Status = ImageContext->ImageRead (
            ImageContext->Handle,        // FV, 0x3AC14064
            0,
            &ImageContext->SizeOfHeaders,// 0x240, sane
            (VOID *)(UINTN)ImageAddress  // 0x3ABF0000
            );
```

`PeCoffLoaderImageReadFromMemory` is a single `CopyMem` of 576 bytes. It does not
return, and no fault, ASSERT or further output is produced. Note the destination
range ends at `0x3AC12CE0` while the source starts at `0x3AC14064`, so they are
adjacent but do not overlap. With caches off the same load lands at
`0x3AB16000`, a different address, and completes.

So the problem is not "slow", it is "the write side of a plain buffer copy does
not come back".

## Ruled out during this investigation

| Suspect | How it was tested | Result |
|---|---|---|
| `CacheMaintenanceLib` being the stubbed MdePkg version | Read `MdePkg/Library/BaseCacheMaintenanceLib/ArmCache.c`: all eight entry points are `ASSERT(FALSE)` stubs | **Not the cause here.** The DSC already maps `CacheMaintenanceLib` to `ArmPkg/Library/ArmCacheMaintenanceLib`, so the stubs were never linked. Worth knowing, but it did not explain anything. |
| `ldm/stm` optimised `CopyMem` | Switched `BaseMemoryLib` from `BaseMemoryLibStm` to MdePkg's portable one | No change; the copy implementation is not involved |
| Instruction cache / stale instructions | Ran with I-cache off and D-cache on | Same stall at the same place, so instruction fetch is not involved |
| LZMA decompression under a cache | `DecompressFirstFv` returns 0x0 | Decompression is fine with the data cache on |
| Bad header size causing a huge copy | Printed `SizeOfHeaders` | 0x240, perfectly sane |

## Where to go next

The remaining question is why a 576 byte write to a freshly allocated page does
not complete with the data cache on. Worth trying, cheapest first:

1. Self test the destination right after `AllocatePages` in `LoadPeCoffImage`:
   write a pattern to `ImageAddress`, read it back, print both. If that fails,
   the problem is the buffer, not the cache protocol or the loader.
2. Try a fixed image address instead of the allocator's choice, to tell "this
   page" apart from "any page the allocator returns".
3. Check whether the region ends up with a different memory attribute than
   expected. `LoadPeCoffImage` allocates through `AllocatePages`, and the HOB
   built by `MemoryPeim` describes the same DRAM the MMU already mapped; a
   mismatch there would show up exactly like this, as read-after-write
   disagreement rather than as a fault.

Anything past that needs a debugger or a bus trace, because the failure produces
no exception and no output.

## Keeping the tree usable meanwhile

The instrumentation that produced the log above is still in
`ArmPlatformPkg/PrePi/PrePi.c` (one print per boot stage) and
`EmbeddedPkg/Library/PrePiLib/PrePiLib.c` (prints around the DXE core load).
They are bounded and only run once per boot, so they are cheap to keep; the
per-image prints that were added to `MdePkg/Library/BasePeCoffLib/BasePeCoff.c`
were removed because they would fire for every module load.

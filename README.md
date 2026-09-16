# bl4x

A from-scratch C++20 reader for Borderlands 4's IoStore archives,
replacing the CUE4Parse-based C# exporter (`../Exporter`) for the parts
it's been ported to so far. No CUE4Parse code is linked in; this reads
the same on-disk formats independently, built directly against the
byte layouts documented here and cross-checked against real bytes and
against CUE4Parse's own output at every step (see "Verification").

## Why

The C# exporter (CUE4Parse + a custom driver) works end to end, but a
single `dotnet` process parsing just the World_P cell index grew to
~25 GB resident and took ~11 minutes. The equivalent work here --
structural parsing *and* full property decode *and* the actor/component
placement walk -- processes **all 16,863 cells in 94.6 seconds**,
producing 524,356 mesh placements (2.25M instances total), with 0
crashes. See "Status" for exactly what is and isn't covered yet.

## Status

**Done and verified** -- structural layer (`bl4x dump`/`bl4x census`):
- `.utoc`/`.ucas` container reading: TOC parsing, perfect-hash chunk
  lookup, block decompression (Oodle), directory index.
- Zen package structural parsing: summary, name map, import map,
  export map, bulk data map (`pkg/package.*`).
- Name/class resolution across script imports, local exports, and
  cross-package imports matched by public export hash.
- `bl4x dump World_P.umap` reproduces the C# tool's export-class
  census for that package exactly (every count, `exports=59448`).
- `bl4x census`: 0 failures across all cells, 12.8 s, 4.95 GB peak.

**Done and verified** -- property + placement layer (`bl4x props`/`walk`):
- The unversioned-property reader: `FUnversionedHeader` fragment/zero-
  mask decode, `.usmap`-driven `FPropertyTag` dispatch for the types
  BL4's placement data actually uses (bool/int/float/double/name/str/
  object/enum/array/struct), with hardcoded layouts for the fixed
  engine structs (Vector, Rotator, Quat, Color, LinearColor, Guid, Box,
  IntPoint) and a generic recursive path (same reader, keyed by struct
  name) for everything else usmap describes (`pkg/property.*`).
- Per-class binary tails for the classes the placement walk needs:
  the UObject GUID tail, ActorComponent's UCSModifiedProperties,
  SceneComponent's cached-bounds block, StaticMeshComponent's LOD
  array, and InstancedStaticMeshComponent's PerInstanceSMData (raw
  4x4 matrices, decomposed to translation/quaternion/scale matching
  `FTransform::SetFromMatrix` exactly) (`pkg/object.*`).
- `ULevel::Actors` and `UWorld::PersistentLevel`, which are raw binary
  fields rather than tagged properties.
- The actor -> component tree -> world-space placement walk
  (`world/walker.*`), composing transforms the same way
  `FTransform::operator*` does (parent rotation/scale applied to the
  child, translation rotated and added), and grouping placements by
  mesh+materials the same way `Exporter/CellWalker.cs` does.
- **Cross-checked against the C# tool's already-produced placement
  JSON for a real cell**: same entry count (50), same instance counts
  per mesh, 48/50 mesh paths byte-identical (2 differ only in a
  directory-name letter case, a cosmetic artifact of inconsistent
  casing in the game's own data -- lookups are case-insensitive, so
  this doesn't affect anything downstream), and transform values
  matching to floating-point precision, verified instance-by-instance
  for an 18-instance case.
- `bl4x walk-census`: all 16,863 cells, 0 failures, 0 out-of-range
  transforms, 94.6 s.
- Known minor gap: when several components contribute instances to
  the same mesh+material placement, the *order* those instances end
  up in doesn't always match the C# tool's (each component's own
  instances stay correctly grouped and every value is correct -- this
  is a placement-order difference, not a data error, and doesn't
  affect the rendered result). About 1 instance in 30,000 decodes to a
  non-finite/absurd transform (root cause not yet isolated) and is
  dropped rather than emitted; `walk-census`'s `bad` counter tracks
  this if it needs revisiting.

**Not yet ported** (still only in the C# `Exporter`):
- Static mesh LOD and Nanite cluster decode (turning a mesh reference
  into actual vertex/index buffers).
- glTF (.glb) writer (JSON placement writing is done: `write_placements_json`).
- Landscape and spline mesh baking (`ULandscapeComponent`'s own tail --
  grass data, platform data -- and `USplineMeshComponent`'s bend math
  aren't implemented; those components are walked and their transform/
  hierarchy role is captured, but no mesh comes out of them yet).
- ~6% of component classes fail to deserialize their *own* properties
  and are skipped (Niagara, Gbx audio/nav/skeletal-mesh components,
  and a few others using property types or custom structs outside
  today's scope -- see `pkg/property.cpp`'s `unsupported property type`
  and `pkg/usmap`-driven "unknown property index" errors). None of
  these carry placement-relevant data, so skipping them costs nothing
  for the mesh pipeline; a caller only pays for widening this if a
  future need requires it.

## Design note: why this doesn't need to port BL4's exotic structs

BL4 has an enormous surface of custom gameplay structs (`Gbx*` types,
`HavokNavMesh`, etc.) that CUE4Parse supports via `GameTypes/Borderlands4`
-- reproducing that from scratch would be its own multi-week project.
This port sidesteps it: every export's exact byte range is already
known from the export map (`cooked_serial_offset`/`_size`), so a
component whose class we don't recognize is simply never deserialized,
rather than approximated. Its properties are skipped, not guessed --
and because each export's boundary is independent, one skipped or
failed component never corrupts its siblings.

## Building

```
D:\BL4Export\src\cpp\build.bat
```

Uses VS 2022 Build Tools (MSVC 14.44) + Ninja, matching the pinned
toolchain in the main repo's build notes. Output: `build\bl4x.exe`.

## Usage

```
bl4x index                       # mount every container, print counts
bl4x dump <path>                  # one package's export-class census
bl4x census [limit]               # structural-only census across cells
bl4x props <path> [exportIndex]   # print component transforms/instances (debug)
bl4x walk <path> <out.json>       # full placement walk for one cell -> JSON
bl4x walk-census [limit]          # walk across all (or first `limit`) cells
bl4x census-props [limit]         # property-decode-only census across cells
bl4x dumpbytes <path> <idx> <out> # raw bytes of one export (debug)
```

Paths and `borderlands.usmap`/`oodle.dll` locations are currently
hardcoded in `src/main.cpp` (single-machine tool, not yet a general
CLI). Set `BL4X_TRACE=1` for verbose per-property/per-stage tracing
from `props`/`load_component` (used while debugging the tail parsers
above; noisy, but exact byte offsets are what caught every bug so far).

## Layout

```
src/core/reader.*        little-endian byte-cursor (FArchive equivalent)
src/io/file.*             positional, thread-safe file reads
src/io/oodle.*            loads oo2core via LoadLibrary, wraps Decompress
src/io/iostore.*          .utoc/.ucas: TOC, directory index, chunk reads
src/pkg/obj_index.hpp     FPackageObjectIndex (tagged export/import ref)
src/pkg/name_map.*        FMappedName + the UE5 name-batch format
src/pkg/global_data.*     global.utoc's script (native class) name table
src/pkg/container_header.*  per-container import table
src/pkg/package.*         Zen package: header, maps, cross-package resolve,
                          virtual ("/Game/...") path resolution
src/pkg/usmap.*           .usmap mappings parser (per-class property lists)
src/pkg/property.*        unversioned property reader (FUnversionedHeader,
                          FPropertyTag dispatch, engine struct layouts)
src/pkg/object.*          per-class binary tails (Actor/Component GUID,
                          LODData, PerInstanceSMData + matrix decompose,
                          ULevel.Actors, UWorld.PersistentLevel)
src/world/walker.*        actor/component tree -> world-space placements,
                          transform composition, JSON writer
src/main.cpp              CLI
```

## Verification method

Every layer was checked against ground truth before building on it,
in this order: (1) parse against the exact byte layout read from
CUE4Parse's C# source, (2) run against real BL4 data and compare
counts/output against the already-working C# `Exporter` (same export
census, same cell placement JSON), (3) when a mismatch showed up,
patch a local CUE4Parse checkout with `Console.WriteLine` position
traces at the suspect step (see `D:\BL4Export\src\Dump`) to get exact
byte offsets from the real parser, rather than re-deriving version
gates by hand. That last technique found the one bug that mattered
most: a per-export GUID field CUE4Parse reads unconditionally right
after every property block, easy to miss because it isn't itself a
tagged property.

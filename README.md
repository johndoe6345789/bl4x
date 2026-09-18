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

**Done and verified** -- texture layer (`bl4x texture`/`texture-census`/`texture-batch`):
- `UTexture`/`UTexture2D`/`FTexturePlatformData` deserialize, per-mip
  bulk data resolution (inline `ForceInlinePayload` consumed straight
  from the current read stream; `.ubulk`/`.uptnl`/`.m.ubulk` sibling
  files otherwise), and DDS (BC1/3/4/5/6H/7, B8G8R8A8/R8G8B8A8/G8)
  writing (`pkg/texture.*`).
- **Virtual Texture decode** (`pkg/virtual_texture.*`, `pkg/bc_decode.*`):
  the overwhelming majority of BL4's world/material textures ship with
  an empty regular mip array and their pixel data in an
  `FVirtualTextureBuiltData` tiled atlas instead (Morton/Z-order
  addressed pages with a per-tile border to trim on assembly). Parses
  the modern (UE5) per-mip addressing path, resolves each resident
  tile's bulk data the same way as a regular mip, decodes BC1/BC3/BC4/
  BC5 blocks (hand-written, no external decoder) plus the uncompressed
  8/32-bit-per-pixel layer formats, and stitches them into one flat
  RGBA8 bitmap for the requested mip level.
- Verified byte-exact on a real texture (`T_1x1_Grid`, regular mips:
  11,064-byte DDS output matches the sum of all 8 known-correct mip
  sizes plus the header exactly) and by output-size and pixel-variance
  sanity checks across hundreds of VT textures (`T_GunToter_Armor_AORM`:
  2048x2048 R8G8B8A8, DDS size matches `2048*2048*4 + header` exactly;
  spatially-varying, non-degenerate pixel data confirmed by sampling).
- `bl4x texture-census 300`: went from 30/390 Texture2D exports
  loading (mostly landscape heightmaps/weightmaps plus general VT
  material textures, both previously misreported as "no resident mip
  data") to 303/390, with every remaining failure being the single,
  clearly-identified `PF_BC6H` gap below.
- Known gap: BC6H (HDR block compression, used by some VT layers --
  likely skyboxes/lightmap-adjacent content) and BC7 aren't decoded;
  both need large partition/endpoint tables that are easy to
  transcribe wrong with no ground-truth decoder to diff against, unlike
  every other layer in this tool, so they're left as an explicit,
  loud failure (`unsupported VT layer pixel format`) rather than a
  silent wrong-pixel risk. Deprecated VT codecs (`ZippedGPU`, `Crunch`)
  are likewise rejected rather than guessed at, matching how rare they
  are in a UE5.6 game shipped in 2025.

**Done and verified** -- mesh geometry (`bl4x mesh`/`mesh-batch`):
- `UStaticMesh`'s binary tail up to and including `FStaticMeshRenderData`
  (`pkg/mesh.cpp`): BodySetup/NavCollision/Sockets skip, then either
  Nanite or classic geometry. `StaticMaterials` (for material-slot ->
  asset-path mapping) turned out to already be available as a regular
  tagged property, sidestepping the need to parse `UStaticMesh`'s far
  more fragile *post*-RenderData tail (ray tracing proxy, Lumen card
  data, distance fields) at all.
- **Nanite cluster decode** (`pkg/nanite.hpp/.cpp`, `pkg/nanite_cluster.*`,
  `pkg/nanite_bits.hpp`) -- the primary geometry format for BL4, since
  most meshes are Nanite-only with fallback LODs stripped: page loading
  (root pages inline, streaming pages via the shared bulk-data
  mechanism, recursively resolved cross-page dependencies), the packed
  (structure-of-array) cluster header, the generalized-triangle-strip
  index decode, and per-vertex attribute decode (quantized position,
  octahedral normal, quantized tangent angle, variable-bit-width vertex
  color, the custom 20-bit UV float encoding) via three parallel
  delta-coded byte planes (Low/Mid/High). Only emits the finest-LOD
  ("leaf", `EdgeLength < 0`) clusters, matching the reference exporter.
- **Classic (non-Nanite) LOD decode** (`pkg/static_mesh_lod.*`): the
  fallback path for the minority of meshes that still carry it --
  position/tangent-basis/UV/color vertex buffers and a 16- or 32-bit
  index buffer, both the inline and separately-bulk-data-backed forms.
- Verified on real BL4 content, not just structurally: a 500-mesh random
  sample decodes 493/500 (the 7 failures are packages with no
  `StaticMesh` export at all -- a sampling artifact, not a decode bug),
  6.6M triangles total, 390 of them via the Nanite path. Spot-checked a
  53,659-triangle Nanite mesh (`SM_Elpis_Crater_01`) byte-for-byte
  plausibility: every decoded normal is unit-length to 6 decimal places
  across all 38,003 vertices (a strong correctness signal for the
  octahedral-unpack + quantization math), the position bounding box
  matches a crater's expected shape, and every triangle index is in
  range.
- Known gap: BC6H/BC7 Nanite vertex-color/UV layers aren't reachable
  (color/UV use a dedicated encoding, not BC textures) so this doesn't
  apply to mesh data; the analogous gap here is voxel/brick clusters
  (`bVoxel`), which for BL4's UE5.5 engine version reduces to "cluster
  has zero triangles" (voxel Nanite is a UE5.6+ feature) -- skipping
  them costs nothing for BL4 content specifically, unlike a future,
  newer UE5.6+ title.
- Known gap, fixed along the way: `TPerPlatformProperty<T>` structs
  (`FPerPlatformInt`/`Float`/`Bool` -- used by e.g. `UStaticMesh.MinLOD`)
  have a custom `Serialize()`, not a tagged property list, and were
  previously misparsed as a generic usmap struct; `pkg/property.cpp`
  now special-cases them like the other hardcoded engine structs.

**Material -> texture** (`pkg/material.*`): a mesh section's
`UMaterialInstanceConstant` is followed through `TextureParameterValues`
and then its `Parent`, up to 8 hops, scoring each bound texture by
parameter name (`BaseColor`/`Albedo`/`Diffuse` beat a `_D`/`_BC` texture
name, which beats a bare "colour"); normal/roughness/comp/mask/emissive
parameters are rejected outright. That is *one* texture, not BL4's
actual shading: these are layered materials (`MaterialLayers`/`Blends`
with colourisation, grime and wear layers) whose graph this doesn't
evaluate. 102 of 114 materials in the default test region resolve; the
misses are emissive fixtures and foliage that only binds a packed
"Composite" map. `pkg/texture_export.*` writes the chosen mip (<= 1024
px, halved on the CPU if needed) as a plain 32-bit TGA.

**Not yet ported** (still only in the C# `Exporter`):
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
bl4x texture <path> <idx> <out.dds>  # decode one Texture2D export -> DDS
bl4x material <path>              # print a material instance chain's properties
bl4x bake <out> <tile_m> <cell.umap>...   # bake cells -> models/textures/tiles
bl4x bake-all <out> <tile_m>              # ... every World_P cell (16,863)
bl4x find-cells <max_hits> <pattern>...   # locate cells by mesh-name substring
bl4x texture-census [limit]       # texture decode census across cells
bl4x texture-batch <list.txt>     # texture decode census for a path list
bl4x mesh <path> <idx> [out.obj]  # decode one StaticMesh export -> stats + optional OBJ
bl4x mesh-batch <list.txt>        # mesh decode census for a path list
bl4x find-export <path> <class>   # list export indices matching a class (debug)
bl4x raw-props <path> <idx>       # print any export's tagged properties, needs BL4X_TRACE=1 (debug)
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
src/pkg/texture.*         UTexture2D/FTexturePlatformData decode, DDS writer
src/pkg/texture_export.*  decoded mip -> RGBA8 TGA (capped size)
src/pkg/material.*        material instance chain -> base-colour texture
src/pkg/virtual_texture.* FVirtualTextureBuiltData parse + tile assembly
src/pkg/mesh.*            UStaticMesh entry point, Nanite/classic dispatch
src/pkg/nanite.*          FNaniteResources, page loading/orchestration
src/pkg/nanite_cluster.*  one packed cluster's header/decode/vertex-resolve
src/pkg/nanite_bits.hpp   Nanite's bit/byte-level decode primitives
src/pkg/static_mesh_lod.* classic (non-Nanite) vertex/index buffer decode
src/pkg/bc_decode.*       BC1/BC3/BC4/BC5 software block decoders
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

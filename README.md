# wxl-modern-assets

Teaches the Client's native loaders to read modern-era assets - M2 models, the M3 (HotS/SC2) model
family, WMO map objects, and BLP/DDS textures - **in memory, at load time**, never by pre-converting
files on disk. Same philosophy as `wxl-modern-adt`: one path per format, gated on the bytes it sees
(chunk presence, field values, version range), not on a version number, so a native asset is served
unchanged and a modern one is reshaped onto the exact contract the native parser already reads.

Four independent format pipelines share this module, each behind its own build toggle (see below).

## What it does

### M2 models

A modern M2 (source inner-version 272-274) is the client's own MD20 layout plus a handful of wider
records and source-only encodings. `downport::ProcessInPlace` reshapes it onto the client's version
(264) by sliding each theme's records onto the client stride and normalizing source-only encodings -
most work happens in place; only the synthesized texture-coordinate-combos array grows the buffer:

- `Cameras` - compacts the source camera record (drops the explicit FoV float, keeps the appended FoV track).
- `Particles` - compacts the source emitter record; scopes the alpha-key cutoff at draw time.
- `Ribbons` - clamps source ribbon indices at load; opts the multi-texture ribbon combine at draw.
- `Animations` - masks the split in/out blend time back to one value; remaps out-of-range sequence ids.
- `Textures` - synthesizes the texture-coordinate-combos array a source model omits, preserves
  equipment texture-animation loops, and normalizes object-skin texture types before parse-time
  finalize reads them.

A model that ships as a chunked MD21 container (`Md21.hpp`) is de-chunked first: the MD20 block is
extracted and the TXID-referenced texture names are inlined via a FileDataID resolver, then the
downport runs on the now-self-contained image.

The reshape can run **host-side** (the DLL then sees an already-compacted image, staged via a marker
bit in the inner version field so it only needs to clear the bit and register the model) or
**in-process** as a fallback when the host is off or didn't serve the file. Either way, `Skin::Rebuild`
runs at skin finalize to rebuild the material/texunit contract a source skin omits (decoding each
batch's packed shaderId into the client's fixed-function blend bits), scoped to models this module
reshaped. The bone-palette split (`common::BoneBudget`, 75 bones/draw ceiling) runs unconditionally on
every skin finalize regardless of origin - it's a hard client constraint, not a modern-M2 concern.

### M3 models (HotS / SC2 / MD34 family)

A **drop-in** flow, host-only: drop `<base>.m3` (optionally with a `<base>.m3a` animation library and
a `<base>.m3cfg` sidecar) inside a loose `Patch*.MPQ` folder, and a request for `<base>.m2` bakes it on
the spot into a client model + skin (cached after the first bake). `.dds` sibling textures serve as the
matching `.blp` on request, transcoded through the shared DDS->BLP path.

The pipeline is reader-agnostic by design: `M3Model.hpp` defines a format-agnostic intermediate `Model`
(verts/regions/batches/materials/bones/particles/ribbons) that any MD34-family container reader
populates - `Md34.cpp` is the first (and currently only) reader - and `M2Build.cpp` is the only
consumer, baking that intermediate into a client M2 + skin. Sequence mapping, texture prefix override,
z-lift, particle tint, and ribbon-to-mesh baking (permanent sheet geometry like wings or banners,
instead of motion trails) are all sidecar options (`Options.hpp`).

### WMO map objects

Root and group files are down-converted independently, data-gated the same way as the ADT merge:
`WmoClient.hpp` is the client contract (canonical chunk order, the positional loader's field layouts);
`WmoSource.hpp` declares only the deltas - source-only chunks stripped (`GFID`/`MOUV`/`MODI`/`MOSI`),
the v2 poly-material chunk converted back to `MOPY`, shader ids above the client's 0-6 range collapsed
(with two shaders needing their surviving layer promoted first), and the relocated MOBA material id
folded back onto the client offset. Group flags are recomputed from the chunks actually emitted and
every other bit preserved - the loader walks optional sub-chunks positionally off those flags with no
magic check, so a stale bit desyncs the whole rest of the group.

The same translate code (`WmoTranslate.cpp`) runs from two call sites: the host transform
(`WmoHost.cpp`, fires on every `.wmo` open) and an in-process fallback (`WmoRuntime.cpp`, fires on the
core's `OnWmoRootLoad`/`OnWmoGroupLoad` between the read and the native chunk walk) that only reshapes
what the host didn't already reshape - on an already-client-shaped buffer the translate reports no
change, so the in-process path is a no-op whenever the host is doing the work.

### BLP / DDS textures

- `BlpTranscode::TranscodeBlp` re-encodes the uncompressed-BGRA BLP encoding (3) - which the client
  cannot decode at all (renders white) - to DXT5, keeping the full mip chain.
- `BlpTranscode::CapBlpMips` caps an oversized texture's larger edge by dropping top mip level(s) and
  re-basing the mip table (no decode/re-encode). Environment textures follow the client's
  `environmentDetail` setting (256 on Low/custom-low, otherwise 512) while other modern textures cap
  at 1024. The environment cap can be overridden with `SET environmentTextureMaxEdge`,
  `SET worldTextureMaxEdge`, `WXL_ENV_TEXTURE_MAX_EDGE`, or `WXL_WORLD_TEXTURE_MAX_EDGE`.
- Item texture components are converted to the paletted format expected by the 3.3.5 character
  compositor and follow `componentTextureLevel` (256, 512, or 1024 depending on the selected quality).
- `TextureScratch.cpp` patches the client's boot-time mip-decode scratch buffer (via the DllMain
  install seam, before any client boot code) wide enough to actually decode the 2048 tileset chain the
  host now serves - without it, the wider chain overflows a scratch sized for 1024.
- `Dds.cpp` transcodes DXT1/3/5 DDS to the client BLP container; a texture-format concern kept separate
  from any model pipeline so any source that ships DDS sidecars (currently M3's drop-in flow) calls into
  it directly instead of owning its own copy.

## Layout

- `shared/common/` - `Chunk.hpp` (generic IFF chunk walker), `Math.hpp` (Vec3/Quat/Mat4).
- `shared/models/m2/` - `Contract.hpp` (source version range, unaligned byte access), `Downport`
  (orchestrator), `Md21` (chunked-container de-chunk), and one file pair per theme (`Cameras`,
  `Particles`, `Ribbons`, `Animations`, `Textures`).
- `shared/models/m3/` - `M3Model.hpp` (format-agnostic intermediate representation), `Md34` (MD34
  container reader), `M2Build` (intermediate -> client M2 + skin baker), `Options` (drop-in sidecar
  parsing).
- `shared/models/wmo/` - `WmoChunks.hpp` (shared chunk-tag constants), `WmoClient.hpp` (client
  contract), `WmoSource.hpp` (source-only deltas), `WmoTranslate` (the transform), `Resolver.hpp`
  (FileDataID -> path callback contract).
- `shared/textures/blp/` - `BlpTranscode`, `Dxt.hpp`.
- `shared/textures/dds/` - `Dds` (DDS -> client BLP).
- `host/models/m2/Register.cpp`, `host/models/wmo/WmoHost.cpp`, `host/textures/blp/BlpHost.cpp` -
  register the host-side `Transform` hook for their format.
- `host/models/m3/M3Host.cpp` - registers the host-side `Provide`/`Exists` hooks for the drop-in flow
  (scans loose `Patch*.MPQ` folders, bakes and caches on first request).
- `src/common/` - `AssetRegistry` (DLL-only, thread-safe set of live model pointers a module reshaped),
  `BoneBudget` (bone-palette split; applies to any client M2, not just modern-M2-sourced content).
- `src/models/m2/` - `ModernM2` (module entry point, binds the core's M2 events), `Skin` (material/
  texunit contract rebuild at skin finalize).
- `src/models/wmo/` - `WmoRuntime` (in-process down-convert fallback).
- `src/textures/blp/` - `TextureScratch` (boot-time mip-scratch widen patch).
- `src/models/m3/ParticleProbe.cpp` - temporary per-frame diagnostic for M3 particle bring-up, not part
  of the drop-in pipeline; slated for removal once that work is done.

## Format toggles

Each pipeline is independently excludable at configure time, without touching source
(root `CMakeLists.txt`):

```
-DWXL_MODERN_M2=OFF   # excludes shared/host/src models/m2
-DWXL_MODERN_M3=OFF   # excludes shared/host/src models/m3
-DWXL_MODERN_WMO=OFF  # excludes shared/host/src models/wmo
-DWXL_MODERN_BLP=OFF  # excludes shared/host/src textures/blp
```

All four default `ON`. `shared/common/` and `shared/textures/dds/` are unconditional shared
infrastructure, not format-specific, so no toggle excludes them.

## Building

The M2, WMO, and BLP transforms run host-side for the best case (bytes arrive at the client already
reshaped) but also compile a DLL-side in-process fallback for when the host is off. M3's drop-in flow
and BLP's transcode/cap only take effect when the host is serving, so build both halves together:

```
.\build.ps1 -BuildHost
```

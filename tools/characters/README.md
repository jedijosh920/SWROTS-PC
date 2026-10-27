# Character tools

Readers and converters for turning Jedi Academy / Movie Duels **player models** into SWROTS-PC
characters, plus the asset readers needed to pull assets out of the original games.

Everything here is plain Python (numpy + Pillow; the converter also wants `fast_simplification`).

| File | Purpose |
|---|---|
| `ghoul2.py` | reads a Jedi Academy `.glm` (surfaces, vertices, normals, UVs, per-vertex bone weights) |
| `slayer_msh.py` | reads and rebuilds a SWROTS character `.msh` (chunks → groups → objects → sub-draws, plus the tail string block) |
| `convert_malgus.py` | the converter: skeleton mapping, weight transfer, palette partitioning, simplification, texture atlas and `.stx` writer |
| `tpc.py` | decodes a KOTOR/KOTOR II **TPC** texture (0x80 header, DXT1 / 32-bit) |
| `erf.py` | random-access reader for BioWare **ERF/MOD/SAV** archives (handles multi-GB texture and model packs) |
| `gff.py` | BioWare **GFF** reader (`.utc`, `.ifo`, …) — note scalars are stored inline, not as offsets |

## Conversion outline

1. **Read the source** — `ghoul2.read_glm()` gives surfaces with vertices, normals, UVs and weights.
2. **Map the skeletons** — `vader_joints()` reads the SWROTS target template's bones, `jka_joints()`
   reads `_humanoid.gla`, and `build_segments()` derives a per-bone rigid transform. `target_bone_for()`
   translates JA bone names to SWROTS names (`lfemur_g` → `hip_L_g`, and so on).
3. **Rebind** — every source vertex is transformed by its weighted bones and re-expressed against the
   target indices; at most `MAX_WEIGHTS` influences survive.
4. **Partition** — `partition()` splits a category into palette-safe draws (`PALETTE_LIMIT`, with
   `PALETTE_SIZES` used for the bone-index sub-draws), because one draw may reference at most 19 bones.
5. **Write** — `fill_object()` packs vertices into the template's object (stride 36 or 44), `empty_object()`
   clears the ones not used, `with_triangle_total()` fixes the group header.
6. **Textures** — build one atlas per material, then `write_stx()` emits a DXT5 `.stx`. **DXT5 alpha is
   opacity in SWROTS**, so pass everything through `make_texture_opaque()` or the model turns see-through.

## Gotchas that cost real time

- **DXT5 alpha = opacity.** The converter fills alpha from luminance (`lum * 0.6`, 0–160), which makes a
  dark model nearly invisible. Always force the alpha blocks to `FF FF 00 00 00 00 00 00`.
- **1024² is the game's texture limit.** 2048² textures fail to resolve and the model renders see-through.
- **`Base=` must be a built-in class.** Only the game's own classes can drive a custom character.
- **The material name in the mesh tail selects the `.stx`.** To reuse a texture set under a different
  name, rewrite the STX internal name at `0x40` to match the material.
- **Skirt/cape cloth bones differ per skeleton.** The native ROTS Vader carries `ec_cape_*` that the
  Anakin skeleton lacks; map unknown bones to a torso fallback or the garment will collapse.
- Keep vertices and indices in step — a converted object whose sub-draw indices exceed its vertex count
  fails to load.

## Verification

`slayer_msh.build(slayer_msh.parse(mesh))` must be byte-identical, and every sub-draw's `refs` count must
stay within `PALETTE_LIMIT` with no index past the object's vertex count. Every converter in this tree
does that check before writing.

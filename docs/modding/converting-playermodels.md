# Converting a playermodel

SWROTS-PC loads its characters as `.msh` files under `mods/meshes/chars/<folder>/`, with one texture
(`.stx`) per material. A custom character is a `.ini` in `mods/characters/` that names a `Mesh=`, a
`Costume=` and a `Base=` class.

Because the port's character format is close to the Jedi Academy one, a JA player model can be converted
rather than remodelled. The steps below are what `tools/characters/` automates.

## 1. Pick the target skeleton

A converted mesh has to match the skeleton of the class it borrows, because the animations are the
class's. The bones differ between classes — measure before you convert:

| Class | Bones | Notes |
|---|---|---|
| `ICinDrallig`, `IMace` | 60 | identical to each other, so a mesh moves between them freely |
| `ISerra` | 62 | female rig; also the class that carries the custom lightning |
| `IAnakin` | 64 | male rig; lightning is available once the port's Anakin hook is built |
| `IDooku`, `IVader`, `IJDBrute` | fewer | no lightning rows |

Take the class's own `.msh` from the game as the **template** — that is where the object layout, bone
order and materials come from.

## 2. Source the model

Movie Duels keeps its characters in large `.pk3` packs (`MD_Models_*.pk3`). Copy the model's folder out,
including `model.glm`, every `.skin` and all its textures.

Read the `.skin` for the surface → texture mapping and pick the variant you want; some models ship seven
or more (hood up/down, mask on/off, caped, colour variants). Surfaces listed `*off` are not drawn and
should be skipped, and surfaces whose texture is missing (often a shared `stormtrooper/caps.tga`) must be
dropped or the build fails.

## 3. Convert

```python
import convert_malgus as base
import ghoul2, slayer_msh

mesh = slayer_msh.parse(template_path.read_bytes())
chunk = mesh.chunks[0]
target_names, target_positions = base.vader_joints(chunk)
source_names, source_positions = base.jka_joints(read_gla(gla_path))
segments = base.build_segments(source_names, source_positions, target_names, target_positions)
```

Then, per surface: transform each vertex by its weighted bones into the target space, keep the strongest
`MAX_WEIGHTS` influences, and append the triangles. Group the surfaces into categories (body, head) that
map to the template's objects, `partition()` them into palette-safe draws, `fill_object()` each one, and
clear the template objects you did not fill.

Finish with `simplify_category()` against a triangle budget — the loader wants roughly ≤5,400 triangles
per strip.

## 4. Textures

Build one atlas per material, placing each source texture in its own cell and remapping the UVs into
that cell. Then:

- `write_stx()` writes a DXT5 `.stx` whose **alpha channel is opacity**, so run `make_texture_opaque()`
  over the result or the model renders see-through.
- Keep the atlas at **1024² maximum**; the game will not resolve larger textures.
- Name each `.stx` after the mesh's material string, or rewrite the STX internal name at `0x40`.

## 5. The ini

```ini
[Character]
Class=IMyCharacter
Name=Display Name
Title=Subtitle
Base=ICinDrallig
Costume=MyCostume
Mesh=mycharacter\mycharacter
Saber=0 128 255
Blades=single
Lightning=1
Side=Hero
```

`Base=` is resolved against the game's own classes only — a custom class name will not work here.
`Lightning=1` does something only on `ISerra`, `IMace`, `ICinDrallig` and (with the port's Anakin hook
built) `IAnakin`; on anything else it is silently ignored.

## 6. Pictures

A custom character needs **both** menu pictures, or it will not appear in the roster:

- `mods/interfc/front_end/s_selduel_<costume>head.stx` (64²)
- `mods/interfc/front_end/s_selduel_<costume>bust.stx` (256×128)
- `mods/interfc/hud_b/hud_face_<costume>01.stx` (64²)

Movie Duels ships a portrait for almost every character in
`gfx/menus/charactermenuicons/*.tga`; use those rather than screenshots.

**Never reuse a `s_selduel_*` name the game's own `.pak` already owns.** A loose file overriding a
PAK texture faults at `0x210B6B` and the game hangs in its error handler.

## 7. Verify before launching

- `slayer_msh.build(slayer_msh.parse(bytes)) == bytes` — byte-exact round trip
- no sub-draw references a bone palette larger than `PALETTE_LIMIT`
- no index past the object's vertex count
- textures are ≤1024² and opaque
- both menu pictures exist and are named after the costume

Then check `logs/Message.log` after launch for the loader's verdict.

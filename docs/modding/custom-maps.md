# Custom maps

A mod can add a map of its own: a model with its collision, played on in [Heroes vs
Villains](../research/heroes-vs-villains.md) or walked in with the [developer map](../research/levels.md). The port
builds it on the developer map (Grievous Survival's level made an empty, lit floor as it loads): the map's model takes
the place of the floor's, its collision boxes are added to the level's brushes, and the floor stays under it all.

## Installing one

A custom map comes as a folder to drop into the game's `mods\` folder:

```
mods\maps\cantina\map.ini               the map
mods\maps\cantina\cantina_light.bin     its baked light (optional)
mods\maps\cantina\mus_<slot>.hwx        its music (optional)
mods\maps\cantina\load_a.stx (b, c)     its loading picture (optional)
mods\maps\cantina\arena_small.stx       its pictures on the Select Arena screen (optional): the row's, 128x32
mods\maps\cantina\arena_large.stx       and the large one, 256x128 (as the game's s_selduelarena_* pictures)
mods\meshes\maps\cantina\cantina.msh    its model
mods\meshes\maps\cantina\*.stx          its textures (named as the model names them)
```

The port reads `mods\maps\*\map.ini` at every start (the log says `Custom maps: cantina (MOS EISLEY CANTINA), ...`).
Then:

- **Heroes vs Villains**: its Map row (Multi Player > Heroes vs Villains); in the console `hvv map cantina`; the
  debug menu's Heroes vs Villains tab.
- **Versus** (the game's own duels: rounds, its HUD, its duel cameras): the map is an arena on the Select Arena
  screen, after the open ones (with its own pictures, see below). In the console `versus ILuke IVader map cantina`
  (you against the computer).
- **Walking in it**: `devmap cantina` in the console (`devmap` alone: the floor), or its button on the debug menu's
  Game tab.
- The launch argument `custommap:<map>` next to `devmap:1` (for example in `mods\Default_Xbox.cfg`).

## The map file

`mods\maps\<map>\map.ini`, in the map's own coordinates (the model's: y up, the game's units, about 37 to the metre:
the human characters are 73-75 units tall):

```ini
[Map]
Name=Mos Eisley Cantina          ; as shown
Music=mus_m13_templebattle01_lp  ; its music (optional): a music slot of the game, the track beside map.ini
Model=maps\cantina\cantina       ; its model under meshes\, without .msh
Vertices=10234                   ; the model's vertex count (all its parts)
Floor=0                          ; the height (y) of its floor: it stands there on the dev map's floor
HeroesLine=0 -350                ; Heroes vs Villains: the middle of the heroes' line (x z)
VillainsLine=0 350               ; and of the villains'; the lines run across the line between them
LineGap=150                      ; between fighters in a line
Spawns=-400 0, 400 0, 0 -600     ; where fighters come back (x z, any number), else near the fight
Bounds=-900 -900 900 900         ; x0 z0 x1 z1: where fighters may come back

[Collision]
Box1=0 100 450 600 100 20 0      ; a box: its centre (x y z), half its size (x y z), its turn (degrees, about y)
Box2=...                         ; numbered from 1, without gaps

[Lighting]                       ; all optional; a colour is one number (grey) or three (red green blue)
Ambient=0.10 0.09 0.08           ; the model's ambient light (the floor's is 0.8)
Baked=cantina_light.bin          ; its baked light: a colour a vertex (u32 0xAARRGGBB, the model's vertex order)
BakedScale=0.5                   ; the baked light dimmed (below 1) or brightened
CharacterAmbient=0.26 0.23 0.20  ; the characters' ambient light (the dev map's is 0.55)
Lights=0.6                       ; the point lights' intensity, scaled
PlayerAmbient=0.3                ; the player's ambient light (left out: CharacterAmbient's)
FarClip=8000                     ; how far the camera sees (the dev map's is 5000): a long map's far end

[PointLights]                    ; optional: lights of its own (they light the characters), in place of the level's
Light1=0 120 300 600 1 0.9 0.8 1.5 ; x y z range red green blue intensity (y from its floor)
Light2=...                       ; numbered from 1, without gaps
```

The map is turned and moved so that the middle between its two lines is the dev map's middle and the heroes face the
villains along +z: the intro's camera, the line-up and the end are then the same as on the floor.

## Battlefront

A map with command posts can host [Battlefront](../battlefront.md) (it is then offered on its setup screen):

```ini
[Battlefront]
CommandPosts=0 0; 300 250; -280 -160; 560 -480   ; the posts (x z, separated by ";"), named A, B, C... in this order
CloneStart=700 245                               ; where the clones start (x z)
DroidStart=-720 -120                             ; and the droids
```

Without the starts, the clones start on the heroes' line and the droids on the villains'. Each side starts with the
post nearest its start. Keep the posts on open floor, inside `Bounds=`, a few hundred units apart.

## Light

Without `[Lighting]` the model is lit by a bright ambient light alone, as the floor. With a baked light the level's
static entry takes it per vertex (its "ambient lighting only" turned off): a converted map can keep the lighting its
first game baked. Dim it all to taste: the cantina uses its Battlefront II light at half, a low ambient and dimmer
characters.

The walls take the baked light; the characters are lit by the level's point lights and ambient light. Without
`[PointLights]` those are the dev map's own lights, gathered round the middle of the floor: on a large map, characters
far from them are lit from one side (light them with lamps of their own where the map's lamps are; short ranges, as
the game's levels use). The player has an ambient light of its own (`PlayerAmbient=`, else the characters').

## Music

`Music=` names one of the game's music slots (the files `audio\xbox\mus_*.hwx`); the track beside map.ini, named
after the slot, plays in its place while the map plays (only then: other levels keep the slot's own track, and the
`mods\audio\xbox\` music replacements are not touched). The track must be in the slot's format: Xbox ADPCM, at the
slot's sample rate and exactly its length (`tools/audio/hwx.py encode`, see [replacing music](replacing-music.md)).
The level's own music start is kept: its player start triggers a sequencer, which now holds one music object alone,
naming the map's slot.

## Loading picture

`load_a.stx`, `load_b.stx` and `load_c.stx` beside map.ini (optional, all three) are the loading screen's picture
while the map loads: one 640x256 picture cut in three side by side, 256x256, 256x256 and 128x256, in the format of
the game's own (`interfc\front_end\loading\load_back_*_a/b/c.stx`, DXT). As in the game's, the map's name and anything
else shown is part of the picture: the screen itself only writes "Loading". Without them the screen is plain dark.

The game draws the textures the level's `loadScreenPrefix:` names (the mission list's; Grievous Survival's are
`load_back_uta_griev_a/b/c`, Grievous on Utapau, in its PAK only); the port serves the map's in their place, or
blacks them out, for that start only.

## The model

A model in the game's mesh format ([file formats](file-formats.md)), static (no skeleton), with its textures as STX
files in its folder ([replacing textures](replacing-textures.md)), each with its own name inside (offset 0x40)
matching the name the model gives it. The level lights it with ambient light only (its baked light, one entry per
vertex, is white: hence `Vertices`).

## How it works

`src/game/custommaps.cpp` reads the maps; `src/game/devmapbuild.cpp` builds the level:

- **The model.** The dev map keeps one static mesh entry, the cave's, whose model is the floor's
  (`MGR_StrippedStaticsList`, its `MGR_ManagedMeshID` 6). The model an id names is the static manager's
  `MGR_ManagedMeshData` entry (u32 capacity, u32 count, then per mesh its name and its folder under `meshes\`): the
  port rewrites entry 6 to the map's (name and folder), places the static entry (`MGR_WorldPosition`) where the map
  stands, and gives the static instance as many baked light entries as the model has vertices.
- **The collision.** The level's collision is its brushes (the scene's second list of BSP trees, in world
  coordinates). A box brush is one convex hedron (8 vertices; its edges and faces only index them), 13 nodes (they
  index the planes) and 6 planes (a normal and a distance). Each collision box is a copy of the cave's first wall
  brush, each vertex moved to its corner of the box and each plane turned with it; the brush manager
  (`IBrushManager`) gets a handle per box (`__handledetails`: its id, its class and what it stops: the player,
  characters, projectiles, the camera, ...; a copy of the wall's) and its `__handlecount` grows.
- The loose model and the textures in its folder are declared to the level when the level asks for the model, as
  for a custom character's body.
- **The light**: the static entry's `MGR_AmbientLight` and `MGR_Ambientlightingonly`; the static instance's baked
  light (one u32 a vertex); `ILevelProperties`' "Character ambient color" and "Dynamic-mesh ambient color"; the light
  manager's lights' `__intn`.
- **The music**: Grievous Survival's player start (`IPointSpawnPlayer`, "Initially trigger objects") triggers a
  sequencer (`ISequencer_x`, "Single Random Event", its "Items" each an `Objects` list) over `IAudio` objects of
  Type Music, event triggered. The dev map keeps the sequencer with its first item alone and that item's music
  object, its "Sound Link" (`ep3\Ep3_Streaming\music\<slot>`) naming the map's slot; the port serves the map's file
  in the slot file's place for that start (`kernel::SetDiscFileOverride`, ahead of mods\). Several music objects
  naming the same slot streamed it at once, out of step (heard as the track and a quieter echo by turns).
- **The AI has no paths** on the dev map: it walks straight at its target. In Heroes vs Villains a fighter that a
  wall, the bar or a platform keeps from its target is moved beside it (see [Heroes vs
  Villains](../research/heroes-vs-villains.md#how-it-works)).

## In Versus

A level is a Versus duel by its own file: `ILevelProperties`' "Head to head arena?" (+0xCC; the level start
0x2AADE0 then runs the duel's setup 0x27DBC0: fighters, HUD faces, cameras), two `IVaderMultiSpawnPoint` objects
(an `IPointSpawnPlayer` by descent; "Player ID" absent for player 1, 'Player 2' for player 2; each round, 0x27C380
puts the fighters on them, set down on the floor) and the duel's camera, an `ICameraControl` 'Rail Tracking' (its two
focus objects the spawn points, re-pointed to the fighters; its rail in its "Cinematics", relative to it) with its
helpers and trigger bound. The launch arguments name the fighters (`v_c1`, `v_c2`), their controllers (`v_p1`,
`v_p2`; -1 the computer) and the rounds (`v_roundcount`); the duel returns to the menus by itself.

So a custom map's duel is the game's Versus starting its Utapau arena (`u107_uta_grievous_01`, whose PAK brings the
duel's fighters, sounds, intro cameras and HUD art), whose level file the port serves as the dev map built for a
duel: the head-to-head flag, Grievous Survival's player start made player 1's spawn point and player 2's added
(220 units apart across the middle), and the arena's own camera objects (from the arena's level file as it loads)
turned and moved from the arena's fighters onto these, their focus on these spawn points. The arena's rail was
laid out for its own room: on a map with walls the game's camera (the rail, the intro's shots) stays while it sees
both fighters from above the floor, and where it does not, the port's own takes over for at least a second (side-on
to the fighters, on the side that sees them, nearer them where a wall stands behind it; the map's boxes). Versus
starts the arena's own music object (Type Music, "Trigger Flags" 0x20), so the map's track object takes its
properties; the loading picture is the Utapau arena's (`load_back_uta_plain_`), served as the map's own; a map of its
own has no sky around it. Its row on the Select Arena screen (the arena list,
`ArenaList`: its entries built from the mission list's Duel rows, 0x2D2AB0) is an entry the port adds after the open
arenas and Random (the cursor never moves onto a locked row, 0x2C69F0), the Utapau arena's level with the map's
name; its pictures take places the port adds to the screen's picture sets, drawn with the map's own. Chosen, it
rewrites the game's launch of the duel (`kernel::SetLaunchFilter`, just before its restart): the arena becomes the
Utapau one, with `devmap:1 versus:1 custommap:<map>`.

## Not yet

- Collision other than boxes (a model's own shape: ramps, stairs); fighters stand on the dev map's floor.
- Paths for the AI (a map's own navigation).
- The map's own point lights and sky (its light so far: baked, ambient, and the level's lights scaled).

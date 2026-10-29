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
Modes=Versus HvV Battlefront     ; the modes that offer it (optional; all of them when left out)
Music=mus_m13_templebattle01_lp  ; its music (optional): a music slot of the game, the track beside map.ini
Model=maps\cantina\cantina       ; its model under meshes\, without .msh
Vertices=10234                   ; the model's vertex count (all its parts)
Sky=maps\cantina\cantina_sky     ; its sky's model (optional), about the sky's origin (see below)
Floor=0                          ; the height (y) of its floor: it stands there on the dev map's floor
KillHeight=-200                  ; a fighter below this height (y) dies, as in a fall into the void (optional)
HeroesLine=0 -350                ; Heroes vs Villains: the middle of the heroes' line (x z)
VillainsLine=0 350               ; and of the villains'; the lines run across the line between them
LineGap=150                      ; between fighters in a line
Spawns=-400 0, 400 0, 0 -600     ; where fighters come back (x z, any number), else near the fight
Bounds=-900 -900 900 900         ; x0 z0 x1 z1: where fighters may come back

[Collision]
Box1=0 100 450 600 100 20 0      ; a box: its centre (x y z), half its size (x y z), its turn (degrees, about y)
Box2=...                         ; numbered from 1, without gaps
Brushes=cantina_brushes.bin      ; its own collision (optional): convex volumes, see below

[Lighting]                       ; all optional; a colour is one number (grey) or three (red green blue)
Ambient=0.10 0.09 0.08           ; the model's ambient light (the floor's is 0.8)
Baked=cantina_light.bin          ; its baked light: a colour a vertex (u32 0xAARRGGBB, the model's vertex order)
BakedScale=0.5                   ; the baked light dimmed (below 1) or brightened
CharacterAmbient=0.26 0.23 0.20  ; the characters' ambient light (the dev map's is 0.55)
Lights=0.6                       ; the level's point lights' intensity, scaled
```

`Modes=` keeps a map to some modes: `Versus` (its row on the Select Arena screen), `HvV` (Heroes vs Villains' Map
row) and `Battlefront` (its setup screen, for a map with command posts), separated by spaces or commas. A duel arena
says `Modes=Versus`, a large map `Modes=HvV Battlefront`. The console's `devmap`, `hvv map` and `versus ... map` take
any map.

The map is turned and moved so that the middle between its two lines is the dev map's middle and the heroes face the
villains along +z: the intro's camera, the line-up and the end are then the same as on the floor.

## Battlefront

A map with command posts can host [Battlefront](../battlefront.md) (it is then offered on its setup screen):

```ini
[Battlefront]
CommandPosts=0 0; 300 250 140; -280 -160; 560 -480   ; the posts (x z, separated by ";"), named A, B, C... in this order
CloneStart=700 245                               ; where the clones start (x z)
DroidStart=-720 -120                             ; and the droids
```

Without the starts, the clones start on the heroes' line and the droids on the villains'. Each side starts with the
post nearest its start. Keep the posts on open floor, inside `Bounds=`, a few hundred units apart.

A post on a floor of its own (a bridge, a landing pad) gives that floor's height after its x z, in the map's own y
(as `Floor=`; without it, a post is on `Floor=`): its marker stands there, and only fighters on that floor (within
180 units of its height) count for it, not those on a street under the bridge. The computer's troopers walk straight
to a post, so one going for a raised post from a floor more than 100 units below is brought up onto it once within
700 units, before it walks under that floor into whatever lies below. It is not brought up again within 8 seconds,
and after three times in a row that did not keep it on that floor (it fell back to a lower one) it walks as the others
do until it reaches a post: place a raised post where there is room around it, on a floor the game holds fighters on.

## Collision of its own

`Brushes=` names a file beside map.ini: the map's collision as convex volumes (floors, walls, steps, ramps), one
after the other, each a BSP tree in the level file's format, in the map's own coordinates (the model's). Each
volume is a tree with one hedron: its vertices, edges and faces (their planes and corner loops), its planes
(outward normals, n . p = d) and a chain of nodes, two a plane and the solid leaf last. They are placed as the
model is (turned and moved with it) and added to the level's brushes, each with its brush manager handle. Fighters
then stand on the map's own floors at any height, and the dev map's floor goes down 1150 units, under the map's
lower parts (a fall there ends on it).

A converter writes these files from Jedi Knight II maps: the JK2 solid and player-clip brushes, one volume each
(`convert_map.py` in the community JK2 pack, not in the port). Maps without `Brushes=` keep the dev map's floor where
it is.

The level's load stops in the game's error handler (in `IStaticManager`, as the static's meshes are set up) when
the model is too large: a model of 19786 vertices loads, one of 30101 does not, whatever its textures.

## Sky and pits

Without `Sky=` a map of its own has no sky around it. With it, the level's sky backdrop (Utapau's `backdrop`, an
`IStatic` drawn about the `ISectorSkybox` before the rest, lit by its ambient light alone) draws the map's sky model
instead, turned as the map is; Utapau's cloud domes and its sky dome go. The model is a static one, as the map's,
in the sky's own units: Utapau's backdrop is some 600 units across and the converter's sky box is 1000 units wide.

`KillHeight=` kills a character that falls below it (the character's instant kill, as in the game's own falls into
the void: in Heroes vs Villains the fighter comes back as after any defeat), once a fall: checked every frame, it
kills a character again only after it has been above the line. In Heroes vs Villains and Battlefront it applies while
the fight is on (not during the line-up or the intro).

## Light

Without `[Lighting]` the model is lit by a bright ambient light alone, as the floor. With a baked light the level's
static entry takes it per vertex (its "ambient lighting only" turned off): a converted map can keep the lighting its
first game baked. Dim it all to taste: the cantina uses its Battlefront II light at half, a low ambient and dimmer
characters.

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

- The camera's line of sight (Versus) and the AI's moves (Heroes vs Villains) know the boxes only, not `Brushes=`.
- Paths for the AI (a map's own navigation).
- The map's own point lights (its light so far: baked, ambient, and the level's lights scaled).

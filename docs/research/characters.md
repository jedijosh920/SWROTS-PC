# Characters: classes, costumes, texture sets and bodies

How the game decides what the player character is and looks like, and what the port changes so
that any story level can be played as any character ([`player` and the Characters
tab](../debug-menu.md#playing-as-another-character)). Addresses are the retail Xbox executable's
(NTSC-U), as loaded. The code is `src/game/characters.cpp` (the player's class, costume, texture set
and body; HUD portraits), `src/game/resources.cpp` (loading from other levels, animation lookups),
`src/game/fixes.cpp` (the animation binding rebuild), `src/kernel/misc.cpp` (restarting a mission)
and `src/debug/console.cpp`, `src/debug/menu.cpp` (the commands and the tab).

## Four layers

| Layer | What it is | Where the game keeps it | Port command |
|---|---|---|---|
| Class | behaviour: moves, weapons, AI, health (`IAnakin`, `ICloneTrooper`) | the class registry, by name | `player <class>` |
| Costume | a named body for the class ("Mesh Choice") | the class's costume list; character +0x1E8 | `player <class> <costume>` |
| Texture set | alternative textures for the body ("Starting texture set") | the costume list's header; character +0x1EC | `skin <set>` |
| Body | the mesh (`meshes\chars\<folder>\<file>.msh`) and its animation binding (`.ban`) | the costume's record | `mesh <mesh>` |

## Creating the player

At a level start, 0x2AB660 creates the player through **0xB1F90** (`kSpawnPlayer`, cdecl
`(TString* class, int costume)`) with the launch settings' player class (+0xAC: the mission
list's or the versus select's class). The character factory creates it by class name (class
registry 0xE9B60, create vfunc +0x30). The new character is given its costume through its vfunc
+0x2C8 (0xB1D60); at **0xB1DE1** (`mov eax, [esp+0xC] / mov edx, [ecx] / push eax`) `ecx` is the
character and `[esp+0xC]` the costume index. -1 means the profile's costume (`[[0x68D944]+0x98]`),
an index into the *level's own* class's list.

The port hooks both: 0xB1F90 replaces the class name, and 0xB1DE1 calls `PlayerVariant(player,
costume)`, which chooses the costume, gives the player its own copy of the list when the body
changes, and sets the texture set. The chosen names live in the port's memory: every level change
and restart reboots the game image, and the game's strings go with it.

Versus fighters are created another way (0x27B7D0 from the duel's class table, then spawn 0xA2DE0):
there 0xB1F90 swaps the class, but 0xB1DE1 is not reached, so costumes, texture sets and bodies do
not apply in Versus yet.

## Costume lists

Static data in `.data`, one per class, found from a character at **+0x1E0** (set by the class's
constructor, e.g. 0x29DD26 for Anakin). The game's own lists are shared by every character of the
class; the port gives the player a copy when it changes the body, so NPCs of the class keep theirs.

```c
struct CostumeList {                 // 0x2C bytes, then the records
    const char* name;                // the class without its "I": "Anakin", "CloneTrooper"
    uint32_t    numbers[4];          // small numbers, meaning unknown (0x45, 0x0A, 0x45, 0x0A for most)
    const char* textureSets[6];      // suffixes, null-terminated: "_var01", "_var02"
    Costume     costumes[];          // up to a record with a null name
};
struct Costume {                     // 20 bytes
    const char* name;                // "Anakin_Duel"
    const char* mesh;                // "AnakinDuel\\AnakinDuel", under meshes\chars, no extension
    uint32_t    zero;
    const char* code;                // three letters: "AKN", "CLT", "CLC"
    uint32_t    zero2;
};
```

The port finds the lists by scanning `.data` once for this shape: a name in `.rdata`, four numbers
below 0x10000 (the weapon, font and skeleton-joint tables built alike have pointers there), and a
first record whose name has no backslash and whose mesh has one. 40 lists match. A class's list is
the one whose name is the class name without its "I".

Some lists name costumes whose models are not on the disc (cut during development): `Sidious`,
`Emperor`, `Luke`, `CommanderCody` (`clonecomm`), `Obiwan_Dirty` and `Obiwan_NPC_Dirty`, `PadwMale`,
`Grievous_shell`, and the battle droid's plain `BattleDroid` (its levels use `hordeBattleDroid`). The
port never picks those: a class whose chosen or usual costume is missing gets the first one that is
on the disc (`VariantOnDisc`). A class with no body at all (a costume whose mesh is on the disc
with its binding) is refused: `ICommanderCody`, whose only costume is the cut `clonecomm` and of
whom the disc keeps only the HUD portrait and versus bust (no definition, tables or animations; with
another body his state code faults at 0x616D1), and `IPoggle`, whose model is static.

Costumes cover the class's looks across the story, and also NPC copies of the same bodies
(`Anakin_NPC_Duel`); `player` matches a costume by number, name, or the shortest name containing
the text, so `duel` is `Anakin_Duel`.

## Texture sets

The list header's suffixes are the class's texture sets, which level designers pick for each
character they place: the instance property **"Starting texture set"** (registered by
`ICharacter::Prop_Serialize`, 0x15B4F3: "(Default)" and the suffixes), stored at character
**+0x1EC** (0 the plain textures, 1 the first suffix). When the body loads (0x155763, called from
`ICharacter::Init`), the suffix `[list + set*4 + 0x10]` is put in the global **0x6941A0**, and the
mesh's texture loader (**0x14F7E0**) first asks for `<folder>\<texture><suffix>` and uses it if the
level declares it, else the plain texture. Character **+0x1F4** is a suffix that overrides it (the
versus `_duel` look of a fighter against itself), and 0x15C670 / 0x15C730 are editor helpers that
step or reset the set.

| List | Sets | `_var01` textures on the disc (levels) |
|---|---|---|
| CloneTrooper | `_var01`, `_var02` | `hordetrooper_var01` (the 501st: m09a, m13, u109, u113), `clonesniper_var01` (m13) |
| BlazeTrooper | `_var01`, `_var02` | `clonesky_var01` (m13, u113) |
| Stormtrooper | `_var01`, `_var02` | none |
| BattleDroid | `_var01` | `hordebattledroid_var01` (m01, m05, m06a, m12b, u105, u107, u112) |
| GrappleDroid | `_var01` | `grappledroid_var01` (m06a, m06b, u106) |
| BattleDroidFly, BuzzDroid, CloneWalker, CommanderCody | `_var01` | none |

No `_var02` texture is on the disc. The clone trooper's plain textures are the 212th's orange
(Utapau); the 501st's blue is `_var01`, placed only in the Coruscant temple levels.

For a set, the port sets +0x1EC on the player and declares the set's textures in the running level
from wherever the disc has them (every `*<suffix>.stx` in the body's folder), so the 501st look works
on Utapau too.

## Bodies and animation bindings

A body is a mesh plus its **animation binding** (`<mesh>.ban`), which maps the skeleton to the
animations the character uses. The disc has 75 meshes under `meshes\chars\`; the 48 shipped with a
binding are character bodies. The other 27 are dismemberment limbs (`*_limbs`), debris
(`*_chunks`), vehicles (gunship, Neimoidian shuttle), droid-throw pieces, Poggle's static model and
`skeleton\skeleton_lightningfx` (the lightning effect). Loaded as a body, these fault in the mesh
setup (0x611B4, from `TGMCharMesh` vfunc +0x28) and the game then loops in its own error handler,
writing `Message.log` without end. The port offers and accepts only bodies: disc meshes with a
binding, and loose meshes under `mods\` (the engine builds their binding).

When the body changes, the port gives the player its own copy of the costume list with the body's
mesh in the costume's record. A body on another class needs a binding for that class's animations,
which the engine rebuilds; three things in that path had to be fixed:

1. **The lookup's found list is compact.** The binding rebuild (0x68670 validates a binding,
   0x68804 rebuilds) asks the animation lookup (0x65C50, vtable slot 0 of `[[0x66F7A4]+0x28]`; the
   call returns to **0x6890D**) for every animation the class uses. The lookup leaves animations it
   cannot find out of its found list, but the rebuild reads one entry per name. A few animations exist
   in no PAK (`Anakin_Frc_Jump_C1`, `_C2`, cut from Anakin's force jump), so the rebuild read past the
   list. For that caller only, the port lays the found list out again in the names' order (engine
   array resize 0x21B70), each missing name taking its nearest neighbour's animation (the names are
   sorted, so a close relative).
2. **A failed rebuild threw the binding away.** After reporting missing animations ("BAN file creation
   yielded missing anim errors"), the rebuild freed the binding and returned none (from **0x689F8**),
   and the game crashed using it (0x68F37). The port continues on the success path (**0x68A33**).
3. **A binding of the wrong size could be accepted.** The validator compares the binding's count with
   the request, but when the packer says so (its vfunc +0xF4, at `[esp+0x13]`) a different count goes
   on to a name-by-name comparison that reads past a shorter binding, which can then pass and be filled
   past its end (0x68F71). At **0x68703** a different count now always rebuilds.

The engine writes a rebuilt binding next to the mesh on the disc drive; the port redirects it to
`cache\disc\` (`src/kernel/file.cpp`) and, when the engine reads it straight back, serves that copy
before any other PAK's (the manual-file hook in `resources.cpp`); it used to get the original
character's binding again.

A loose mesh under a name no PAK has (a modder's own body) is declared to the level from `mods\`
(`DeclareFromDisc`), and the mesh lists include `mods\meshes\chars\`.

## Saber colours

A saber's colour is a key (saber +0x814, float r, g, b) set with its vfunc **+0x604** (`SetColor`,
thiscall `(const float rgb[3])`, `ret 4`: 0x286180 for the single-bladed sabers, vtables 0x5B1FF0,
0x5B2620, 0x5B2C90, 0x5B32F8; 0x2864F0 for the double-bladed, 0x5B3958). The effect (0x2E1600)
draws the pure keys (1,0,0), (0,1,0), (0,0,1), (1,0,1) with the colours of the `sabercolor` table
(0x6512F4 blue, 0x651300 green, 0x65130C red, 0x651318 purple; the command is 0x2F4C50), any other
key as it is; that is why `sabercolor` changes every saber of a colour.

A character equipping a saber (0x280480, at 0x28054E) gives it `[character + 0xF20]` if set, else
its own default; +0xF20 is a `const float*` override that IJedi's constructor clears and Versus sets
per fighter (IGameManager::Init 0x27B872, from 0x650C98). The power-up switch (saber vfunc +0x614,
0x286B90; called by the character update 0x281230 when a power-up starts and ends) brightens the key
(blue (0,0,1) to (0.3, 0.5, 1)), which is not a pure key and so is drawn raw, the original colour
whatever `sabercolor` says; but it does nothing when the owner's +0xF20 is set.

The port's `saber` points the player's +0xF20 at its own colour (at creation, before the equip, and
live), and recolours the player's sabers in its four weapon slots (character +0x1080, 0x20 apart) with
`SetColor`. Only characters that are IJedi (IsA, vfunc +4, with the type key 0x249950, as the power-up
switch asks) have the field. Cutscene events and scripts can still set a saber's colour.

## Spawning characters

The game spawns characters at run time in three places, all alike: the AI respawner (AI.cpp, 0x19C1E0),
the versus fighters (0x27B7D0) and a developer item spawner with no callers (TGVaderInterface,
0x2DF600, which builds a transform in front of the player). The recipe:

1. The class's object: the class registry's lookup **0xE9B60** (cdecl `(const char* class)` -> class
   info), then **0xE2350** (cdecl `(class info)` -> object; class info vfunc +0x30). Every class is
   registered at startup, so any class can be created in any level.
2. `object+0xC &= ~0x08000000` (created objects are marked inactive), the costume at +0x1E8 (and the
   texture set at +0x1EC).
3. **0xA2DE0** (cdecl `bool (object, owner or null, const float transform[16], 1)`): places it (object
   vfunc +0x1F4), pushes it on the spawn stack (0x68D9A8, count 0x68DA38) and adds it to the level's
   instance manager (`[[0x68D944]+0x58]`: 0xB6540 unowned, 0xB67B0 owned), which initializes it
   (`ICharacter::Init`, loading what the level lacks through the port's resource hooks). Logged by
   the game as "SpawnEnd %s (unowned)".
4. The object's vfunc **+0xB4** (thiscall, no arguments; as the item spawner at 0x2DF849 and the player's
   creation do): without it the character exists but is not seen.

A character's transform is at **+0x150**: 4 rows of 4 floats (right, up, forward, position; the copy
0x1A7970 takes 3 of each). The port places a spawn 120 units in front of the player, turned round.

Loading a character once the level has loaded needed two fixes in the port's manual-file handling: the
packer is then no longer streaming (+0x1D0 is 0) and its stream reader is freed while +4 still points
at it, so (a) a manual file (a body's `.ban`) the disc does not have as a file is served from a PAK copy
instead of being opened on the disc, and (b) the packer's "put the stream back" after a manual read
(0x51E40, called by the scene at 0x4B0D5) returns at once when not streaming.

**Sounds.** The game's character sound manager (`[0x6943F0]`) registers a class's sound set only while the level
loads (it checks at 0x159729; a class created later gets "Spawned character '%s' is initialized after load, and
will not have valid sounds" and no set, character `+0x444` = 0). The class's script adds its own cues to its set
at load (Vader's breathing), which a character created later never runs. A spawned character's own sounds are
therefore skipped: its clashes and hits when it blocks or is struck, its voice, its footsteps. Registering a set
late is not enough (an attempt gave a half-built set and faults at 0x16F262); it needs the script's sound setup.
A class that has a set already (a character of that class loaded with the level) is another matter: a character of
it created later takes that set (seen: a Vader spawned where a Vader was placed in the level got the placed one's
set). Characters placed in a level file load with it, whatever their class, each class with its set
([levels](levels.md#modes-and-missions-from-the-games-own-objects)): a way to give spawned classes their sounds.

**Shared bindings.** A body's animation binding (`<mesh>.ban`) is one resource per mesh, shared by every
character wearing it; it is rebuilt for a class whose animations it lacks. Two classes in one body in
the same level therefore fight over it, and the one rebuilt over crashes in its animation (0x663C5).
The level's own characters are in it too: a player in the clone trooper body as Anakin had the
level's clones animate with Anakin's binding (Jedi moves on clones), then crash on an animation index
it lacks (0x616D1). A body worn by a class none of whose costumes it is is therefore a **private
copy**: the costume record names `<mesh>__pb<hash of the class>`
(`clonetrooper\hordetrooper__pb6b3437b5`), which the port serves as the original (the mesh, its
binding, anything named after it: `PrivateBodyOriginal`, `src/game/resources.cpp`), and whose
binding the engine rebuilds and keeps under the private name. The binding also stays as long as the
mesh is loaded, which the port keeps for the level (see below), so the port records the class of
every body it places in a costume of its own (spawns, players) and refuses a spawn of another class
in one; a spawn takes no other body.

**Grievous's sabers.** A character's sabers are made in its Init (IJedi::Init 0x2814C0, each named slot
through 0x280480, attached to Att-RH, Att-LH, Att-AUX1 and Att-AUX2); a slot record is char+0x1064+slot*0x20
(+1 hidden, +0x1C the saber). Grievous has two sets of arms; his class script's Init (0x3BD2F0) sends
"SetTwoArms" (0x3BC820: the B arms' materials hidden, sabers 3 and 4 switched off, their hidden flags left
clear). A character created once the level is loaded runs that Init inside the spawn (0xA2DE0 -> 0xB6540 ->
vfunc +0x220), so the activation after it (IJedi's +0xB4, 0x280790: every saber not flagged hidden on)
brought sabers 3 and 4 back at his hidden wrists: sabers floating. The port sends "SetTwoArms" again after
activating a Grievous (vfunc +0x2AC, thiscall `(event, float delay)`, as the game's script call does):
`SettleActivated` in `src/game/characters.cpp`.

## Changing the player live

Reloading the body in place does not work: re-running the body load's mesh part on the live player
(costume vfunc +0x2C8, the mesh holder +0x388's load vfunc +0x70, post-mesh 0x152780) loads the mesh,
but the animation binding and state (+0x438, from `TLinkScript::Lock`) stay the old body's ("No
running animation has valid movement data" every frame; the character spins). The port replaces the
player instead (`game::ReplacePlayer`), as if the level had started with the new character:

1. The new character is created as a spawn is (class, costume, texture set, body) and placed at the
   player's transform (+0x150).
2. It becomes the player: the primary character record's +0xC (record `[0x68DCEC]`, 0xB1C60; what
   0xA30F0 and some 400 callers return), +0x390 = 2 (the player-controlled character), controller 0
   bound with 0x150580 (thiscall `(int id)`, rebinding the input manager's slot through 0x8ADD0), and
   0x68EF70 pointing at its transform.
3. The level's references to the old player move to it, found by scanning the game's memory and
   static data for pointers to the old character (as `findrefs` lists them) and moving those in known
   holders, matched by vtable and field: the camera's focus lists (TCamComPrimaryFocusList, vtable
   0x56CB0C, +0x44), camera controls, the level's triggers and points. The focus lists cache the
   target's node (entries of 0x70 bytes: +0 "is the player", +4 the target, +8 the node from the
   target's vfunc +0x5C); each list resolves its entries again with its vfunc 5 (0xC4650). Other
   characters' pointers to the player move too: an opponent's (+0x3B8 and nearby) and its AI's. A
   character has two AI objects, each pointing back at it: the AI controller (character +0x9FC,
   back at +0x10; its target at +0x41C with the target's instance id at +0x418, set by 0x193D90) and
   the AI data (+0xA00, back at +0x29C). A pointer is taken as theirs only when the nearest object
   start below it is such a pair, so stray values are never changed (an earlier rule moving any value
   that looked like a pointer into the player in contiguous memory changed texture data). An enemy
   still aiming at the removed player crashed in its AI (0x180C31, 0x193889).
4. The level's objects keep the player as a reference of two parts, a pointer and then its
   instance id (object +4), and some look it up by the id: the id moves with each pointer moved
   (ICharacterGoto +0x1F4, the focus lists' +0x48, ICameraControl +0x278), and so do ids in other
   characters and their AI. The duel levels' master camera (IMasterCameraVader, 0x5B4DC0) follows
   the id at +0x23C alone; with the old one it stayed where it was until the next cutscene.
   The game manager (`[0x7EB964]`) knows the players by instance id (object +4), per slot (+0x2A4,
   +0x1E4 slots); the HUD's bars find the player through it (0x27AB30). The ids are moved over. The
   HUD portrait objects (HudVitals, vtable 0x5A82B0) keep the face they picked: +0xD "picked" and
   +0xC "gave up" are cleared, and they pick the new class's face on the next frame.
5. The old player lets go of its controller slot first (0x150580 with -1). Removing a character
   unbinds its slot (0x8ADD0 with -1: the input manager `[0x68D4F4]` +0x1D8[slot] entry's character,
   +0x7C, is cleared), and the old player still held slot 0 (its +0x43C), now the new player's: the
   input then had no character, and the first move reading the stick's direction relative to it
   crashed (0x89329). The port checks the entry after the swap.
6. The old player is removed as the game removes its own objects: deactivated (vfunc +0xB0: it
   leaves the level's lists, such as the Jedi deflecting projectiles, 0x7EAAF0; without it a
   projectile's deflection check crashes at 0x26F14A), then deleted (0xA2FE0: vfunc +0x1CC, then
   `TManager_Object::Delete`, 0xB5840).

The level's own player is held by nothing else, so destroying it would free its body; a later
character in that body (`player off` after a live change) then reloads the mesh under a model still
bound to the old data (0x6754A). The port keeps a reference to every character mesh the level loads
(`SceneLoadHook`, `src/game/resources.cpp`). With the mesh kept, its binding is kept too, so a body
another class wore earlier in the level as its own costume is left to a restart (see *Shared
bindings*); bodies worn as private copies never are.

The level's own player's class is recorded where the player is spawned (0xB1F90; the mission names
it without the "I": `Anakin`), and its costume where the costume is chosen, for "each level's own".

## Optional sequences

A character's behaviour (GBehavior `CharState`) runs sequences of steps (the steps at sequence +0x44,
their count at +0x40, 0x14 bytes each; four step kinds, handled by the objects at CharState +0xBC to
+0xC8). A level loads only the optional sequences its own characters use; the others exist with no
steps and a "not loaded" mark (+1; optional is bit 0x80 of +0x30). A character in a level that does
not have it (played or spawned) can branch to one: the game reports it ("Character of type 'IObiwan'
... is attempting to branch to sequence 'GSO_Launcher_Light', which is marked as optional but not
loaded for this level!") and branches anyway (0x16C940). The step walk (0x49CAD0) ends only on the
last step, which for no steps lies before the first, so it walked on through the memory after the
empty list and crashed, at random, on a step kind it has no handler for (0x49B82A, null handler) or
on what that left behind. The port refuses such a branch after the report (`src/game/fixes.cpp`):
the character does not do that move.

The duel moves are among them (`Block2_*`: blocks and block reactions against a lightsaber;
`BlockShunt_*`, `TrapShunt_*`: the clashes a saber lock comes out of; `GSO_Launcher_*`,
`GSE_Clearing_Sweep_*`), so a hero spawned in a level without duels (Utapau's Separatist HQ) blocked
stiffly and never got into a lock, and neither did the player's own Obi-Wan against him. The launch
settings' developer switch `optionalanims` (`[[0x66F7A4]+4]` +0xE4, registered at 0x14983; +0xE3 forces
it) has the level loader (the one read, 0xB12E9) load every optional sequence of the level's
characters and the animations they play. The port turns it on (`[Debug] OptionalMoves`, on with the
debug menu) by making that read `mov dl, 1`. The extra animations come from other levels' PAKs like
any missing one; eight were never made and get a close relative as a stand-in (Anakin's force jump
C1/C2, the grapple droid's mount V3 and hole-out, the clone's cover-out left and slide in/loop/out:
`StandInAnimation`, `src/game/resources.cpp`); without them those classes fail to initialise ("You
are missing required animations"). Measured in Utapau with four heroes spawned: 21 refused branches
without it, none with it, for 0.3 MiB of Xbox memory and 1.9 MiB of the port's.

## Sides

A character's AI data (`AIData.cpp`, property names from its Prop_Serialize at 0x19CF00 onwards) is
embedded at character **+0xBB8** and pointed at by **+0xA00**; it points back at +0x29C. A second AI
object, the AI controller (+0x9FC, back at +0x10), holds the current target (+0x41C, with its instance
id at +0x418). The side is the AI data's **"Target Player"** (+0x50; with +0x1C set it counts as a side):
the level's enemies have it, the player and its allies not. "Is that character my enemy?" (0x18F3F0,
thiscall on the AI controller): with the player, the character's own "Target Player" (0x18F300); with
teams ("Team Setting" +0x214, a bit per team A-H; "Teams Ignore Player Unless Attacked" +0x218), a
differing "Target Player" makes enemies, else a shared bit makes friends and an other without teams is
no enemy; without teams, a differing "Target Player" makes enemies, then the class's "Enemies" list
(+0x20; "Preferred Enemies" +0x38). The level's characters have no teams. A spawned hero keeps its
class's "Target Player" (set: your enemy), so an ally that only had a team stayed on the level
enemies' side: they ignored each other, and it fought back when hit. The port's `spawn ... ally|enemy`
sets "Target Player" (0 for an ally, 1 for an enemy) and gives the player and its allies the team bit
0x8000, its enemies 0x4000, outside the designers' A-H. Fighting a character with teams gives the player
every team that one lacks (0x18FA8C), so team bits alone do not keep sides: the port hooks the check
(0x18F3F0) and, when either character has a side of the port's (ally 0x8000, enemy 0x4000, neutral
0x2000, riot 0x1000), decides by side: riot is everyone's enemy, neutral no one's (and "Ignored By AI",
+0x52, set), else the player's side (the player, allies, level characters not targeting the player)
against the enemies'. A neutral spawn that loses health riots.

**Behaviours.** The AI data's "Controller" (+0x08) is the AI controller type the character gets when it
is placed, built by TGCoreInterface's factory (0x1365E0, thiscall `(type, owner, info)`): 1 Pursue,
2 Attack, 3 Idle, 4 Patrol, 5 Roam, 6 Stalk, 7 Wall, 8 Goto, 9 GiveItem, 14 Follow, 15 RunAway. A level
gives a character an info object with the controller's settings (TAIFollowControllerInfo: "Follow This
Character" +0x94); a Follow controller built without one follows the player (0x183930). Patrol, Goto,
Wall and GiveItem need the level's data (a route, a point, a wall, an item). Setting "Controller" on a
spawn before it is placed did not change its behaviour (it kept roaming), and RunAway so set crashed
in its controller (0xB9B84, from TAIBaseControllerInfo vfunc 56): how the AI picks its controller at
runtime is still open. The AI data also has a
formation system ("Formation Data": "Can Join Formations", "Target What Leader Targets", "Formation
Members"), not used by the port. Character +0x3F4 is a category, not a side: 1 the player,
0x10 troops (clones and droids alike), 0x08 duelists (Jedi, Vader); the AI keeps a copy at +0x3FC.

## Health and Force

A character's health is a float at **+0x130** and its maximum at **+0x134** (1000 for Anakin, 50 for a
clone trooper); Force power at **+0xA40** and its maximum at **+0xA44**, on the Jedi-like characters.
The game's `health`, `maxhealth` and `power` variables only set them, on the player (0x150420,
0x150450, 0x150480, through 0xA3130); read back they give what was last set, not the player's values,
and so do `forcelevel`, `combatskilllevel` and `forcepowerlevel`. `god` (options +0xC9) covers health
only; the port's infinite Force keeps +0xA40 at +0xA44 every frame.

## HUD portraits

The game manager (`[0x7EB964]`) keeps the level's character portraits in an array at +0x260
`{capacity, count, data}` of `{face, head, name}`, filled at the level start for the characters the
level expects (0x27BE30, add 0x1F8A00). The HUD finds the player's face by name (0x27B740) from the
table of twelve portraits per class (0x650C30; heads 0x34 bytes on). The port loads a portrait from
the table that the level did not expect, through the texture manager (`[[0x645F7C]+0x38]+0x4C`,
vfunc +4). Classes without a portrait (the clone trooper) show none.

## Restarting the mission

When a change of character cannot be made live (above), the port restarts the running mission:
an in-process reboot with the launch data the running game image started with, as the game's own
Restart Mission does (`kernel::RestartMission`). The launch data page itself is no use: the game
rewrites it while it runs, and from the menus it no longer names the mission. The port keeps a copy
from every boot (`BootInit`). A mission booted straight from `mods\Default_Xbox.cfg` has no launch
data; its restart boots the same config again.

If a game thread does not stop within 4 seconds (a game stuck in its error handler), the reboot falls
back to starting the game process again; the character choice is handed to the new process as
`SWROTS_PLAYER` (`kernel::SetBeforeRelaunch`), which it reads at its start.

## Animation playback

What a character plays is its `TCharState` (`[char+0x434]`, vtable 0x58EFB8; Indiana Jones' `TCharState_Game`):

- `+0x30` the current sequence (record: `+0x8` speed, `+0xC` name, `+0x10` animation, `+0x1C` its function, `+0x28` end
  code, `+0x50` next sequence or -1); `+0x50` the sequence's speed (the class scripts' SetAnimSpeed writes it), `+0x54`
  the play's speed (0 freezes the pose: a sequence ends only *past* its last frame); `+0x78` transitions done, `+0xCC`
  / `+0xD0` transitions waiting; `[+0xB8]` the range state: `+8` / `+0xA` first / last frame (int16), `+0x14` the
  current frame (float, the animation's own frame number), `+0x24` rate.
- Each tick of the character (TickScript, vtable +0x24, 0x49CD90) advances the frame by `+0x50 * +0x54 * rate`: 60
  frames a second at speed 1 (seen in game). Past the last frame, in that same call: the linked next sequence, else the
  sequence function with its end code (the class moves on, e.g. 0x38A140(instance, class object, -1, -1)), else it
  loops from its first frame.
- PlaySequence 0x150E30 is `thiscall(char, sequence, float blend seconds, int start frame, bool, float speed)`. Played
  from outside the character's tick it is queued and starts at its next tick; the class sometimes queues its idle
  right after (seen about every other play), which puts it away again.
- Back to the class's own (its idle) from the port: set the script context for the character (`TCharState` vfunc +0xC
  sets `[0x6943CC]`, `[0x69652C]`, `[0x6964C0]`, `[0x696528]`; save and restore them), then 0x38A140(`[0x6964C0]`,
  `[0x69652C]`, -1, -1).

The port's missions (`missions.cpp`, `play_anim`) hook TickScript to loop a range, hold a frame and put a play back.

## Hits, targets and the dead

- Every hit on a character goes through Reaction_Call 0x159E20 (`thiscall(char, bump, collision, sequence)` -> bool;
  the classes' wrapper 0x26A960 calls it); returning true without calling it is the game's own "no hit taken" (no
  damage, no reaction, no block). Its real handler Reaction_Handle starts at 0x158930 (vtable +0x300). The port has
  one hook there (`characters.h` HookReactions): script actors take none; Cody's rolls dodge in it.
- Targets: the AI's target check (0x19FFC0) skips the dead (`+0x12C`), characters whose AI is off, "Ignored By AI"
  (`[+0xA00]+0x52`) and hidden ones; the player's lock-on, Force powers and grapples ask the candidate's brain
  IsEnemy (0x18F3F0). IsEnemy false for a script actor (in the port's IsEnemy hook) keeps everyone off it.
- Death: `+0x118` dying, `+0x12C` dead. An NPC's death (0xA3000) fades it out after `+0x5DC` seconds and removes it
  (0xB5840), unless "Cinematic Death?" (`+0x5D4`) is set, which keeps the body. A mission's body that must never get
  up is a script actor holding a death animation's last frame instead (no hit reaches it).
- Bumping: HandleCharacterBumping (0x16AED0, from the character's tick) pushes the two characters of each contact
  apart; a character with the byte `+0x7F4` ("character push", Phys_EnableCharacterPush; 1 from its constructor, the
  script function 0x17A020(bool) sets it, e.g. off during ForceAction_2HandPush) at 0 takes no part: others pass
  through it and it is never shoved. Missions: `pushable(c, false)`.

## Paired kills

The kill moves are pairs: the attacker's half (`..._GB`, the grabber) and the victim's (`..._GR`, the grabbed), both
from the moment of contact (the approach is a separate "attempt" animation). The reaction tables (`r_<class>.csv`,
columns Reaction / AttackerReaction / Sound) start both halves when an attempt connects on a victim at critical
health; e.g. Anakin's lunge: `Anakin_Kill_Lunge_Human_GB` (0x318B60, animation `anakin_atk_kill_lunge_humangb`) and
the human's `Kill_Lunge_GR` (0x3D34C0, `anakin_atk_kill_lunge_humangr`).

- The attacker's sequence, at its start: 0x2BC210(1, 0, 0) (its weapon), Grapple_Grab 0x172E00(x, y, z) (the
  partner: the script's target, else `[+0x3D8]`, else `[attacker+0x5B0]` (an instance id), else in a cutscene the
  nearest; ids stored both ways, victim `+0x5AC` / attacker `+0x5B0`; the victim's physics commit `+0x4E4` becomes a
  TCommit_GrappleDefend (vtable 0x58EB38) that holds its root to the attacker's at that offset: the lunge's is
  (0, 0, 0), the pair's animations made around one origin), slow motion 0x176BF0(0.5, 0, 9999, 0). Frame code 6:
  0x401F40 (IDS_FX_LIGHTSABER_Body_Impale at the saber's ATT_saber_middle); 20: slow motion off. At its end: release
  0x172E20, 0x2BC1B0(0, 0). 0x174B40(name, 0, 0, 0, 0) plays a sequence on the grapple partner by name.
- The victim's `Kill_Lunge_GR`: at its start IDS_FX_LIGHTSABER_IMPACT_HEAVY at its bone root_g and 1000 damage to
  itself (0x2BB460: nothing to an invincible one); at frame code 49 the release and `Fall_StandingF`
  (`Anakin_HitRe4_VSpin`), then `R_OnBack` (`Anakin_KD_B_OnGroundIdle`).
- A sequence's events (0x1F6310 start, 0x1F6350 left, 0x1F62D0 end, 0x1F6390 frame code, 0x1728C0 = the code) are
  registered by asking them while the class is set up (the setup object's `+4` set: each ask adds the event to the
  sequence through 0x1F8A00); an event not asked then never comes.

The port's missions play the pair from an `[Animations]` attacker's sequence (`play_anim` with `partner`): the same
calls as 0x318B60, which itself cannot run on another class (it keeps its state in Anakin's class object).

## Open questions

- **Carrying state over in a live change**: the new character starts with its class's health and
  Force, not the old player's; the old player's combat state (a grab, a saber lock) is not handed on.
- **Versus**: costumes, texture sets and bodies for fighters need the fighter creation path
  (0x27B7D0 / 0xA2DE0) hooked like 0xB1DE1.
- The four numbers in a costume list's header.
- Characters a class's moves were not made for (droids with a Jedi's moves) stretch or twist; there
  is no retargeting.

**Following (open).** The first mission's Obi-Wan and R2-D2 follow Anakin through their AI data's
"Controller": Obi-Wan's is 13, R2-D2's 11, types the controller factory (0x1365E0) does not build, so a
companion behaviour of its own. The AI controller (+0x9FC) keeps a default behaviour at +0x14 (Obi-Wan
17, R2-D2 11, a spawned Obi-Wan 7) and the current one at +0x18 (13, 11, 5). Writers of +0x18 in the AI
code: 0x181F1E, 0x1829ED, 0x193FB3, 0x19C7F7, 0x19C8CF, 0x19CD81 and others from 0x1A0EA4 on.

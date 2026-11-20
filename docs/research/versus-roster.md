# The versus roster: how Yoda became a fighter

The Xbox release has nine versus fighters. The disc also has Yoda's select-screen head and bust,
his HUD portrait and his name and title in every language, but no slot for him: a fighter that was
planned and left out. The port adds him as a tenth fighter with his own cell, and is built so that
more fighters can be added the same way ([adding versus fighters](../modding/adding-versus-fighters.md)).

This page records what the game does and what the port changes, for anyone continuing the work.
Addresses are the retail Xbox executable's (NTSC-U), as loaded. The code is `src/game/roster.cpp`
(the select screens and the duel), `src/game/versus.cpp` (duel cameras, the `duelist` command),
`src/game/aliases.cpp` (Yoda's blocks), `src/game/characters.cpp` (HUD portraits) and
`src/game/resources.cpp` (loading from other levels, generated textures).

## The pieces

| What | Where it comes from | What the port does |
|---|---|---|
| A cell on the select grid | the menu's `HeadsTextSet` | adds a picture to the menu as it loads |
| Name and title | a table of ten text IDs | answers for extra slots |
| Unlocked | the profile's lock bytes | extra slots are never locked; the profile is untouched |
| Random | `rand() % unlocked count` | chooses among unlocked fighters and the extras |
| The fighter in the duel | a class table of nine | a longer table, built per duel |
| Saber colours | a table of nine slots x two players | a longer copy |
| Intro and win cameras | `cinematics\introcamera\<Name>_*_Cam.cin` | another fighter's (Yoda: Anakin's) |
| The character itself | the arena's PAK | loaded from another level's PAK |
| Player 2 against itself | `<texture>_duel` textures | darkened copies generated at run time |
| Reactions to a duelist | the character class | Yoda's blocks mapped to his own animation |

## The select screen

The versus select screen is `interfc\front_end\xml\select_jedi.xbl_xml`, with the menu handler
"JediList" (factory 0x2D54D0, vtable 0x5D68B8). Everything about a cell is found by its slot
number: 0-8 are the fighters, 9 is Random.

- **Number of cells.** The handler makes as many cells as the menu's `HeadsTextSet` has pictures,
  less one (the last is the "locked" picture). Adding a picture adds a cell.
- **Menu texture sets** (compiled `.xbl_xml`): the strings `textureSet` and the set's name (each a
  u32 length and the text), a u32 count, then per picture the strings `texture`, folder and name,
  and 28 bytes `{0, 0, float width, float height, 0, 0, 0}`. The engine asks for the menu by its
  source name (`select_jedi.xml`); the port edits the PAK's copy as it is read
  (`RegisterResourcePatch`).
- **The sets:**
  - `HeadsTextSet`: fighters 0-8, Random, [extras], Locked.
  - `BustsTextSet`: fighters 0-8, [unused 9, extras], evil Anakin, Random, Locked. The last three
    are found from the end (count-3, count-2, count-1), so inserting before them is safe.
  - `FullBodyTextSet`: player 1's 0-8, player 2's 0-8, Random. Player 2 takes the picture 9 on
    when both chose the same fighter (`mov eax, [edx + eax*4 + 0x24]` at 0x2D4ECE). With extras the
    blocks are `slots + 1` long and that displacement is patched to match.
- **Full-body pictures** (0x2D4DB0, thiscall): the port's hook clears them for extra slots, as for a
  locked fighter, since the disc has none.
- **Locks.** `IsLocked` (0x2D4920) and nine inline checks (`cmp edi, 9 / je <unlocked>` at 0x2D49A8,
  0x2D4A4F, 0x2D4B48, 0x2D4BEF, 0x2D4CD4, 0x2D4DEB and others; the grid's own at 0x2D57A4 with `esi`)
  read the profile's byte at `profile + 0x2A8 + slot`. `je` becomes `jae`, so every slot from Random
  on counts as unlocked and extras never read the profile. The save writes exactly nine lock bytes
  (0x24C4E0), so nothing about extras reaches a save file.
- **Names and titles** (stdcall `(TString* out, slot)`, 0x2C6610 and 0x2C65E0) look up a table of ten
  text IDs (0x7F2F2C, 0x7F2F54). The hooks give extras their IDs (Yoda: `IDS_SHELL_YODA`,
  `IDS_SHELL_JEDI_MASTER`, both on the disc) through the TString constructor 0x222FA0.
- **Random** (0x2D4CF0) calls `rand() % n` (0xA23D0) with the number of unlocked fighters, assuming
  they come first. Both calls (0x2D4CF7, 0x2D4D09) go to the port's chooser instead.
- **The arena screen** (`select_arena.xbl_xml`, 0x2D2240) shows only the two fighters' full-body
  pictures, from a set like the select screen's (player 2's offset at 0x2D23D2). Extras show their
  bust there, a 256x128 picture in a 128x256 frame (the engine repeats the edges of a picture smaller
  than its frame, so the bust keeps its own size).

## The duel

- **Which fighters.** The shell writes the chosen class names into the launch settings
  (`[0x7F33FC] + 4` and `+ 8`, `char*`), taking a slot's class from the class table (nine names and
  a null, read by slot at 0x2C6644).
- **Setting up** (0x27D980, thiscall) creates both players' fighters of *every* class in the table,
  keeps the two whose names match the launch names and releases the rest; fighter records are at
  `manager + 0x26C`, indexed by the table's slot. An unknown class name never creates a fighter,
  and the duel crashes later (0x27C460).
- **The port** keeps two longer tables: the select screen's (nine, Random's unused entry, extras) and
  the duel's, built when the duel sets up: the nine plus only the extras chosen. An extra fighter
  is therefore loaded only for a duel it is in, in duel slot 9 and up. Six instructions address the
  duel table (one of them at the table + 4); their displacements are patched to the port's copy.
- **Saber colours**: 0x650C98, three floats per slot and player, all zero for the character's own.
  Player 2 always has an override for the nine; the port's copy gives extras none, except player 2
  in a mirror (see below). The fighter keeps a pointer into the table, so it must stay put.
- **Cameras.** The intro and win shots are `cinematics\introcamera\<Name>_Intro_Cam.cin` and the
  like. The disc has none for Yoda; `versus.cpp` hands him Anakin's (`cameras` in the fighter row).
  An intro animation is `animation\<name>\<name>_intro.bnm`; Yoda has none. The port makes his
  (`yodaintro.cpp`): a cutscene keeps each actor's animation as a clip's memory image, and his bonus
  mission's opening one (`cinematics\u113\cin_u113_010.cin`, actor `R1_Yoda_baseShape`) ends with
  him reaching out with the Force and standing on guard; its last 2.4 s become `Yoda_Intro`
  (without the cutscene's `origin` track; his cameras are Anakin's, scaled to his size), served as a
  generated memory image and played as a custom character's own intro (`customchars.cpp`). His
  line is "Touch me you cannot!" (YDA060), which only his mission's dialogue archive has: it is
  added to the generic one, and asked for through one of his kill lines (he has no lines for the
  "Versus" context, and his taunts need clones or droids).

## Custom characters

The extras are a list made at every start (`roster.cpp`): Yoda, then the [custom characters](../modding/custom-characters.md)
that bring their select pictures (up to 8 extras), each with its name and title as texts of the port's own
(`IDS_PORT_<CLASS>_NAME`, `_TITLE`), its base's intro and win cameras and its own saber colour. The duel's fighter
for a custom character is its base class; the duel keeps a slot's fighter for the player whose chosen name equals
the fighter's class name (0x27B8CD, 0x27B983), so the port compares the chosen name with the slot's class.

Its pictures are read in the place of a pair the disc has and never shows (`s_selduel_gbghead` / `gbgbust`): the
menus take only pictures read with `res_null.pak`; a loose picture no PAK has fails in the pre-cache (0x210B6B, the
image pointer null), declared at the scene's start or not.

## Loading a character the arena does not have

A duel arena's PAK only holds the nine fighters. When the engine asks for a character the level does
not declare, the port finds it in another level's PAK, declares it the way the engine's folder scan
would, and loads it: the definition (`meshes\chars\common\yoda.xml`), its main script definition
(`s_yoda.xml`), its tables (`r_yoda.csv`, `a_yoda.csv`), model, textures and animations, plus the
animations the engine requires of it (including grapple animations of every fighter it can meet).
For Yoda these come from the Jedi Temple levels. See
[how loading works](../modding/how-loading-works.md#what-the-port-changes).

## Player 2 against itself: Sith Yoda

In a mirror match the game gives player 2 a different look: the duel sets the texture suffix
`_duel` for player 2 (0x27B88D), and the texture resolver (0x14F7E0) uses `<texture>_duel` when the
level declares `d:\<folder>\<name>_duel` (checked through 0x4BDA0). The nine fighters have such
textures (e.g. `meshes\chars\darthvader\vaderhead_duel.stx`); the evil Anakin bust is the select
screen's counterpart.

Yoda has none, so when *both* players chose him the port generates them:

- STX: a 0x184-byte header with the texture's name at 0x40; format 3 is DXT1, 4 is DXT3/5, not
  swizzled. Each colour block's two endpoints are desaturated and darkened (22% brightness, a cold
  tint), keeping the block's DXT1 transparency mode (endpoint order).
- The textures are named `<name>_duel`, written to `cache\disc\` and read from there like loose
  files (textures served from memory decoded but were never drawn), and declared in the duel's level.
- Player 2 gets a red saber in the same case.

Outside a Yoda mirror nothing is generated or declared, so Yoda looks as he always does. The menus
still show his normal bust for player 2: menu pictures are found by name among the textures already
loaded, and loading a generated texture while the menu itself loaded crashed (0x210B6B).

## Reactions: why Yoda floated

Reactions are sequences of the character's script class (GScript, compiled into the executable).
The attacker's table (`r_<attacker>.csv`) names the defender's reaction generically, e.g.
`Block1_React_High_Alt`; the defender's class plays it. Every human class inherits the base class's
sequences, which play Anakin's animations through PlaySequence (0x1F5B20, e.g. `Block1_React_High`
plays `Anakin_BloRe1_Hi`). A class renames them with its **alias list**, vtable slot 6
(thiscall `(animations, sequences)`): it fills a null-terminated array of `"animation\0sequence"`
strings and hands it to 0x1F6460, which adds both halves to the two lists.

Yoda's class (vtable 0x608C10, alias method 0x494190) was written for his bonus mission against
clones: its 41 aliases cover blaster and clone hits, not a saber duelist's blows. Those fall back to
Anakin's animations, which the port loads for him; on Yoda's body they keep Anakin's hip height and
he floats. The port adds 55 aliases, every `Block1_*` and `Block2_*` sequence, to his own block
reaction (`Yoda_BlockRe1`). Mapping hits, grapples, throws and falls to his few hit animations was
tried and looked worse than Anakin's, so those stay as they were. A few reactions can still look off.

## Not done yet

- Yoda's intro and win animations (see Cameras).
- A darker player 2 bust in the menus and a darker HUD portrait for Sith Yoda.
- More fighters: see [adding versus fighters](../modding/adding-versus-fighters.md).

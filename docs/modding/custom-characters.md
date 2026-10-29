# Custom characters

A mod can add a character of its own: a new character class (for example `ILuke`) that the game creates wherever
it creates characters by name, beside the game's own (nobody is replaced). It shows up in the debug menu's
Characters tab (play as it, spawn it), in the console (`player ILuke`, `spawn ILuke enemy`), in [Heroes vs
Villains](../research/heroes-vs-villains.md) (the main menu's select screen, the debug menu's tab, `hvv`), and a
level can place it as an object.

A custom character has its own **body** (model and textures), **costume name**, **saber colour**, **name** and
**side**. Its **moves, AI, sounds and voice** are those of an existing class, its *base* (for example `IAnakin`):
the game's characters' behaviour is compiled into the game and cannot be written anew, so a new character fights
like the base it names.

## Installing one

A custom character comes as a folder to drop into the game's `mods\` folder:

```
mods\characters\luke.ini                  the character
mods\meshes\chars\luke\luke.msh           its body
mods\meshes\chars\luke\luke_body.stx      its textures (named as the model names them)
mods\meshes\chars\luke\luke_head.stx
```

Nothing else is needed: the port reads `mods\characters\*.ini` at every start (the log says `Custom characters:
ILuke (LUKE SKYWALKER, from IAnakin)`), and the body's textures are loaded with it.

## The character file

`mods\characters\<name>.ini`:

```ini
[Character]
Class=ILuke              ; the new class's name (a class the game does not have)
Name=Luke Skywalker      ; as shown (Heroes vs Villains' titles, scoreboard, kill feed)
Title=Jedi Knight
Base=IAnakin             ; the class whose moves, AI and sounds it has
Costume=Luke             ; its costume's name
Mesh=luke\luke           ; its body, under meshes\chars, without .msh
Mesh2=luke2\luke2        ; player 2's body when both Versus players pick it (optional; else its own)
Saber=0 255 0            ; its saber's colour, 0-255 each (optional; 0 255 0 is the game's own green)
Saber2=255 0 0           ; player 2's when both Versus players pick it (optional; else red for a hero, blue for a
                         ; villain, as the game's dark Anakin)
SaberLeft=255 0 0        ; its second saber's colour, the one in its left hand (optional; on a two-saber base)
Blades=single            ; its saber: single, double (double-bladed) or dual (two sabers) (optional; the base's)
Hilt=LsaberObi\LsaberObi ; its hilt, a weapon mesh under meshes\weapons, without .msh (optional; the base's)
Scale=1                  ; its size (optional; 0.86 makes a Jedi Brute body about as tall as Vader)
Health=1000              ; its health (optional; the base's: the Jedi Brute 220, Anakin 1000)
Force=1000               ; the Force it holds (optional; the base's: the Jedi Brute has none, Anakin 1000)
ForceFrom=IVader         ; Force controls on a base without them (optional; the Jedi Brute so far: push, grip by
                         ; holding the push, saber throw, heal, as Vader's)
ForcePowers=Push Grip SaberThrow Heal ; which (optional; Lightning too if listed)
Intro=IVader             ; whose Versus intro and outro it plays (optional; the base's): IAnakin, ICinDrallig, IDooku,
                         ; IMace, IObiwan, IOldObiwan, ISerra or IVader
IntroAnimation=GBguard_Atk_CombinedV1_Call ; its Versus intro is this animation of its base's instead (optional;
IntroSpeed=1.2           ; the Jedi Brute's twirl), at this speed (optional; 1)
Lightning=1              ; on Serra Keto's class (Base=ISerra): Force lightning instead of her stun (optional)
Grapples=IDooku          ; on Serra Keto's class: Count Dooku's grapple throws instead of hers (optional)
ForceStorm=1             ; on Serra Keto's class: the Force Storm special (optional; see "Other duelists' moves")
Breathing=0              ; on Darth Vader's class (Base=IVader): without his breathing (optional; 1)
Side=Hero                ; Hero or Villain: its side in Heroes vs Villains by default
Speaker=Luke             ; its name in subtitles (optional; the name's first word)
Code=                    ; whose lines it says, a dialogue speaker code such as AKN or OBW (optional; the base's)

[Voice]                  ; optional: its own lines for some dialogue events (see "Its voice" below)
BAT=luk001 luk002        ; battle cries
TNT=luk003 luk004        ; taunts
KIL=luk008 luk009        ; after a kill
INT=luk010               ; in its Versus intro (optional; else a TNT line)
WIN=luk011               ; on winning a Versus round (optional; else a KIL line)
EFF=luk012               ; efforts: jumps, falls (optional; said with HIT for its base's grunts)

[Subtitles]              ; optional: the texts of its own lines
LUK001=For the Rebellion!
```

## Player 2's look

The game's fighters have a second look for player 2 in Versus (dark Anakin, Jedi Vader): the duel gives player 2's
character the texture suffix `_duel`, and each of its textures is then `<texture>_duel` where the level has one. A
pack gives its character the same: `<texture>_duel.stx` next to each body texture (e.g. `luke_body_duel.stx`, its name
inside `luke_body_duel`), declared with the body like its other textures. Without them player 2 looks like player 1;
`Saber2=` (or the other side's colour) still tells them apart.
Player 2 can also wear another body: `Mesh2=` (a body under `meshes\chars`, as `Mesh=`, on the same skeleton) is
player 2's in a mirror duel, with its own textures (its `_duel` ones where it has them).
Its HUD portrait for player 2 is `<portrait>_duel.stx` next to its own (e.g. `hud_face_luke01_duel.stx`), when the
pack has one.

## The body

A character body is a model in the game's character format ([file formats](file-formats.md#character-meshes)),
with the base class's skeleton (the same bone names; the human Jedi share one) so that the base's animations play on
it. The port builds its animation binding (`.ban`) the first time it is worn (`cache\disc`). Its textures are STX
files ([replacing textures](replacing-textures.md)), each with its own name inside (offset 0x40) matching the name
the model gives it, in the model's folder.

A body can be tried before it becomes a character: `player IAnakin mesh luke\luke` wears it as Anakin.

Textures made for another game's lighting can look washed out here: the game lights its characters flatly (no
per-pixel lighting or bump maps), so colours that another engine warms or deepens show as they are (Battlefront
II's Luke's hair is an olive grey there, golden in its own lighting); grade them for this game. Where a texture's
alpha cuts a shape out (hair), fill the transparent texels' colour with the nearby opaque colour: the game filters
the texture, and black behind a cut-out shows as a dark fringe.

## Its saber

`Saber=` sets the colour, `Blades=` the kind and `Hilt=` the hilt; all keep the base's when left out, and they work
on any Jedi or Sith base. `Hilt=` names a weapon mesh: one of the game's (`LsaberObi\LsaberObi`, `LSaberDooku\LSaberDooku`,
`LsaberMace\LsaberMace`, ...) or the pack's own under `mods\meshes\weapons\` ([replacing weapons](replacing-weapons.md)
has the format); a double-bladed saber keeps the double hilt its blades need. A pack's own hilt needs, beside its mesh
and textures, its attachment points and bounds under the mesh's name (`<name>.gat`, `<name>.gin`, e.g. a game saber's
`.xbl_gat` and `.xbl_gin` renamed): the blade runs from `ATT_saber_base` to `ATT_sabertip`, and without them it comes
out of the hand across the hilt and hits nothing. The kinds:

- `single`: one blade (a base with a double-bladed saber gets the Jedi Knights' hilt);
- `double`: the Jedi Brute's double-bladed saber (Darth Maul: `Base=IJDBrute`, whose moves are made for it, needs
  no `Blades=` at all);
- `dual`: two sabers, the second like the first. Only a base whose moves use the off hand (Serra Keto, `ISerra`)
  wields it; with the others the second saber hangs at the side.

`SaberLeft=` colours the second saber (the left hand's) on its own, e.g. a purple and a red one.

The Jedi classes' Init fills up to four weapon records (character +0x1064, 0x20 bytes each: +0 the double-bladed
flag, +4 the hilt mesh, +8 its colour) before their weapon setup (0x2814C0) makes a saber of each record that has a
mesh (0x280480; a flagged one becomes the double-bladed saber class). The port changes a custom character's records
just before that (at 0x2814D2).

## Its Force

Every Jedi script class has the Force moves (the shared Jedi set-up puts each move's handle at the same place in every
class: Force push's at +0x119E of the class object), but each class's own player rules decide what the buttons do
(the class's StateManager calls them: Vader's 0x48E5B0, the Jedi Brute's 0x3D9D50), and the Jedi Brute's have no
Force rules: its buttons did nothing. With `ForceFrom=`, the port runs Force rules after the base's (at 0x3D9D38),
as Vader's: the Force button tapped pushes (back, left or right with the stick for its variants; held, it grips),
the saber-throw button throws, both stick clicks heal, and lightning when listed in `ForcePowers=` (the script
instance's enable bytes +0x2DC push, +0x2DB grip, +0x2DF throw, +0x2DD lightning, +0x2D9 heal; their costs +0x330..
as the Jedi set-up 0x400EE0 sets them, which the Jedi Brute's Init leaves out). The second stance has its own set.
Its Force sounds are the `ForceFrom=` class's: a character's sounds are its event tree's (`s_<class>`, [file
formats](file-formats.md#event-trees): the sounds it loads and what plays when its sequences and animations play), and
the Jedi Brute's has none for the Force, so Vader's Force entries (push, grip, saber throw, heal, lightning, the
sounds they load) are added to it as it loads. They are keyed by the Force sequences and animations, which the Jedi
Brute plays the same as Vader; the game's brutes never play them. (So far for `ForceFrom=IVader` on the Jedi Brute.)

The computer uses them too. Its rules are the same ones (the AI runs the class's player rules: what they offer on a
decision frame are its candidates, 0x175D60), so the port offers it every power it can pay for (the Force at least
the power's cost, heal only below its health threshold, and only while the character's "Allow Force Powers" is on,
as Vader's do), and its attack table chooses among them: a class's `a_<class>.csv` (one per script class, bound to
its moves by name) gives each move its range, facing and odds. The Jedi Brute's has no Force rows, so the
`ForceFrom=` class's are added to it as it loads (`a_vader.csv`'s ForcePush, ForceGrasp, ForceLightning, ForceHeal
and ForceSaberThrow rows, those it lacks). The game's own Jedi Brutes share that table but their rules never offer a
Force move, and they hold no Force: they fight as before.

## Other duelists' moves

On Serra Keto's class (`Base=ISerra`) a character can fight with moves of other duelists while Serra keeps hers:

- `Lightning=1`: her stun (the Black button, R) becomes Force lightning, and her stun grip lightning in a grip;
- `Grapples=IDooku`: her grab becomes the generic one, and its throws Count Dooku's knock-down and launch with his
  palm blast;
- `ForceStorm=1`: block + stun (LT + Black; Shift + R) calls lightning down on every enemy near it, five strikes,
  for 500 Force, one every 15 seconds and not in a round's first 8 seconds; the computer's uses it too. It hits enemies
  only.

How: the game swaps sequences per character (0x173AB0, in the character's script turn: what its script asks for,
another plays; every class's Init makes its own, e.g. Dooku's throws instead of the generic ones), so the port makes
the swaps in the character's first turn (after Serra's rules, at 0x479228). The lightning sequences are every Jedi
class's (its lightning is the IJedi's); its lightning is enabled (script instance +0x2DD). Dooku's throws are
written out again as the port's own sequences on Serra's class (his own write fields of his script instance past the
end of Serra's): the same animations, events and frame codes, his palm blast from effect slots of the port's; their
victims play `Dooku_Grab_KD_GR` / `Dooku_Grab_Launch_GR`, which every class has. Dooku's lightning and grapple sounds
are added to Serra's event tree (entries she never plays) and his grapple rows to her attack table for the computer.
The storm's strikes are the game's own explosion objects with its electric explosion and Force shock effects: a column
of lightning comes down on each enemy, then hits ("Electrified" damage, every class's hit reaction).

## How it works

`src/game/customchars.cpp`. The game creates a character of a class through the class registry's lookup (0xE9B60,
`cdecl (name)` -> the class info), whose vtable slot 0x30 creates the character (from 0xE2350); a level's objects
are resolved the same way (0xE9C30). The port answers the custom name with a copy of the base's class info with
a vtable of its own whose create calls the base's, then:

- gives the character the custom **costume list** (+0x1E0: a list per class, set by the class's constructor; a
  0x2C-byte header, then 20-byte records `{name, mesh "folder\file", ?, code, ?}` up to a null name): the custom
  costume first, then the base's, so the character wears its own body by default;
- points its **saber colour** (+0xF20, an RGB the saber takes when the character equips it, in ICharacter::Init)
  at the custom colour.

The character is otherwise the base class (the game names it so: `characters` lists an ILuke as IAnakin), so its
sounds are the base's (and a character of a class placed in the level has its sound set: see
[characters](../research/characters.md)).

A model under `mods\` whose name no level has (a new body) is declared to the level when it is first asked for,
and so are the textures (`.stx`) in its folder: the model loader only asks for the textures the level declared.

## On the Versus select screen

With its select pictures in the pack, a custom character gets a Versus slot of its own (after Yoda's), its name
and title under it, and fights in the duel as itself (also in the main menu's Heroes vs Villains, which picks
fighters on the same screen):

```
mods\interfc\front_end\s_selduel_<costume>head.stx       the strip's cell, 64x64 DXT5 (a ring in its alpha, as the game's)
mods\interfc\front_end\s_selduel_<costume>bust.stx       the bust above the strip, 256x128 DXT5
mods\interfc\front_end\s_selduel_fb_<costume>_v1.stx     its full body, player 1's (optional), 128x256 DXT5
mods\interfc\front_end\s_selduel_fb_<costume>_v2.stx     player 2's (optional)
```

(`<costume>` in lower case, e.g. `s_selduel_lukehead.stx`.) Any number of custom characters get their slots (up to
nineteen after Yoda, Commander Cody and the Droid Commander among them; the strip scrolls). The full bodies show
under the strip and on the arena screen, as the game's fighters'; without them the arena screen shows the bust in
the full body's frame.

The menus use only pictures read with the shared PAK (`res_null.pak`; a picture under `mods\` that no PAK has is
refused: its pre-cache finds no image, 0x210B6B), so a custom character's entries in the screens' picture sets name
pictures the disc has (the cut Grievous bodyguard's, Random's full body), and the port loads the custom pictures from
their files (the texture loader's direct path, as the concept art screen loads its pictures) and draws them in their
place: a menu picture item (draw 0x2C7790) holds the custom texture only while it is drawn, and the port draws the
strip of heads.

The strip of heads shows 11 fighters at a time; with more it scrolls with the cursor, with the arena screen's blue
arrows at its ends where fighters are out of view (the port draws the strip as the game does, 0x2D56F0, from the
first fighter in view; a cursor whose fighter is out of view is hidden; after player 1's pick the opponent's cursor
stays in view).

In the duel a fighter needs intro and outro cameras (`cinematics\introcamera\<name>_Intro_Cam.cin`, outro the
same): a custom character uses its base's, and a base that was never a duelist (the Jedi Brute) Vader's (without
them the duel's start waits forever and a round's end crashes).

### Its intro and outro

A duelist's intro and outro (the win at a round's end) are sequences of its class (`Vader_Intro`, `Vader_Outro`:
the animation of the same name, and the line said at a moment of it, a taunt in the intro and a kill line in the
outro, from the dialogue entries for Versus). The duel plays the sequence named after the fighter's character
(0x27B550: `<name>_Intro`), so a custom character plays its base's; a base that was never a duelist (the Jedi Brute)
has none and just stands there. With `Intro=` it plays that duelist's, with its cameras: the duelist's two sequence
functions are added to the base class's own when the class is set up (src/game/classsetup.cpp: after the class's
own, before its end marker, so each gets a record as the class's own do; they are handed a scratch class object,
since the handle slot they write would be one of the base's own), its animations are declared in the base class's
animation folder (the class finds its animations only there), and the duel asks for the duelist's name.

With `IntroAnimation=` its intro is one of its base's own animations instead (Darth Maul: the Jedi Brute's saber
twirl, `GBguard_Atk_CombinedV1_Call`, which the brute plays as its "Flourish"), played as a duelist's intro is: a
sequence `<costume>_Intro` of the port's (on duel maps only, like the duelists' own), whose first frame does what
Vader's intro's does and says the intro line. It is no attack (no attack-table row, no hit): the character stays where
it stands. The outro and the cameras stay `Intro=`'s.

A duelist's intro and outro change the character's size (Vader's grows him to 1.1 and back, 0x178FD0): a custom
character's changes are relative to its `Scale=`, so it keeps its own size.

Its line in them is its `INT` line (intro) or `WIN` line (outro) when it has them, else a `TNT` or `KIL` line of its
own, else the base speaker's: with `Intro=`, the base's speaker has no lines for the Versus context, so the line is
asked for without it (0x17ACC0).

The duel keeps a slot's fighter for the player whose chosen name equals the fighter's class name (0x27B7D0,
compared at 0x27B8CD and 0x27B983); a custom character's fighter is its base class, so the port compares the chosen
name with the slot's class instead.

## Its HUD portrait

```
mods\interfc\hud_b\hud_face_<costume>01.stx    64x64 DXT5, black where transparent (as the game's faces)
```

The HUD picks a face by the character's class (0x25E9xx, the face's name from the game's portrait table, the
lookup 0x27B740 called at 0x25EADD with the character in `esi`); for a custom character the port hands it the
custom face instead, loaded from its file (the texture loader's direct path: the resources' path decodes a texture no
PAK has to nothing).

The pause screen's combat chart shows the player's head picture (the same lookup with "head", 0x2B2A91): a custom
character's is its select-screen head (`s_selduel_<costume>head.stx`).

## Its voice

A character's voice lines are the dialogue engine's (`TDialogueEngine`), not its class's sound set (sabers,
footsteps): each level loads a dialogue database (its own `Dialogue\<mission>_Dialogue`, else `GEN_Dialogue`) of
entries per speaker code and event (BAT battle cry, TNT taunt, KIL kill, FSE first sight of an enemy, HIT hit or
saber lock, DIE death, and grunts), whose lines are in `Dialogue\*.rpx` archives (headerless Xbox ADPCM, mono,
22050 Hz) with their texts in the string tables. A character's speaker code is its costume record's code (+0xC),
taken by RegisterSpeaker (0x8D800) in its Init: a custom character's record has the base's code (or `Code=`), so it
says the base's lines; its name in subtitles (+0xA04, set at 0x15BC56 from `IDS_SPEAKER_<its list's name>`) is
its own (`Speaker=`).

With a `[Voice]` section it says its own lines for those events: the files `mods\characters\<name>\voice\<line>.xadpcm`
(headerless Xbox ADPCM, mono, 22050 Hz, as the game's), their texts in `[Subtitles]`. The port serves the generic
archive (`Dialogue\gen_dialogue.rpx`, mounted in every level) with the lines added (built in `cache\voice\` when
they change, from a pack's own `mods\Dialogue\gen_dialogue.rpx` when there is one, so the lines it replaces stay;
any number of lines fit) and, when the dialogue engine starts one of the character's entries for such an event (StartEntry,
0x90330), gives the entry the character's line for that call: the voice, the subtitle and the line's length follow
it. For an event it has no lines for, it says those of a related one it has (KIL: WIN then TNT; WIN: KIL, TNT;
INT: TNT, BAT; FSE: BAT, TNT; TNT: BAT, KIL; BAT: TNT, FSE), and for its base's efforts (lines numbered 900 and
up: pains, grunts) its `EFF` lines (jumps, falls) or its `HIT` lines, a pain's `HIT` first; only what it has none
of stays its base's (logged). The lines take turns per event (a turn passes only when the
engine starts or queues one). Subtitles show with the game's subtitles on (the Settings menu, or `set subtitles true`).

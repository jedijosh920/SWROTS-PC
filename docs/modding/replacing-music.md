# Replacing music

The music is a set of looping tracks in `GameData\audio\xbox\`, named `mus_*.hwx`. A file of the same name
under `mods\audio\xbox\` replaces a track wherever it plays. Worked example: the main menu set to
*Padmé's Ruminations*, and the final duels scored with *Battle of the Heroes*, *Anakin vs. Obi-Wan* and
*The Boys Continue*, each heard only in the duels.

## How a track is stored

A `.hwx` track is headerless Xbox ADPCM, stereo: blocks of 36 bytes per channel (a 4-byte header, then
32 bytes of 4-bit codes) holding 65 samples each, decoded as in `src/audio/audio.cpp`.

The file holds no sample rate or length. Both come from the game's sound table, `audio\xbox\ep3.xb_wml`,
which is found through `ep3.xb_sml` and is packed in the level PAKs. The game:

- plays the track at the table's rate (about 32000 Hz, a little different per track), so audio at
  another rate plays too fast or too slow, and lower or higher;
- loops it after the table's byte count. A longer file is cut off there, and a shorter one doesn't
  loop where it ends.

**A replacement must be exactly as long as the original, at its rate.** The sound table can't be replaced
from `mods\` (see [Limits](#limits)), so a track's length can't change.

## Which tracks a level plays

Levels play their music through cues in the level file (`levels\<level>.slp`), each naming a track.
Many tracks are shared between levels, so replacing one changes it everywhere it is used. Some examples:

| Track | Rate | Length | Played in |
|---|---|---|---|
| `mus_ui_mainmenu_lp` | 32179 | 1:18.3 | the main menu |
| `mus_m14_dotforch_lp` | 32000 | 1:26.9 | both final duels (start and last rounds), versus Separatist Throne Room 1 |
| `mus_ingamebattle28_lp` | 32000 | 1:59.2 | Obi-Wan's duel (`m14_mus_theduelobi`), the only story level; also the Mustafar versus arenas and bonus missions |
| `mus_ingamebattle17_lp` | 32095 | 1:45.7 | Anakin's duel (`m15`), Mustafar assassination (`m12b`) |
| `mus_ingamebattle21_lp` | 32000 | 1:57.9 | Obi-Wan's duel, Palpatine's office (`m08`), `m12b` |
| `mus_ingamebattle24_lp` | 32034 | 1:36.8 | Anakin's duel, Order 66 (`m11a`), versus Throne Room 2 |
| `mus_ingamebattle02_lp` | 32070 | 1:06.7 | no level (free to repoint a level to it) |

`hwx.py slots` lists every track with its rate and length, and `hwx.py cues <level>.slp` lists the tracks a
level's cues play. The menu and the end screens play their tracks by name from the game's code.

## Making a replacement

1. **Dump the sound table and the level.** With `[Mods] DumpResources=1` ([dumping assets](dumping-assets.md)),
   start the game and load the level: `dump\audio\xbox\` gets `ep3.xb_sml` and `ep3.xb_wml`, and
   `dump\levels\` gets the level file.
2. **Look up the slot.**

   ```
   python tools/audio/hwx.py slots dump\audio\xbox
   ```

3. **Edit the music to the slot's length**, in any audio editor. Plan the loop: the end runs straight into
   the start, so end on a phrase that leads back into the opening, or blend the last few seconds into
   the audio just before the start point. To match the original's loudness, decode it and compare
   (`hwx.py decode mus_x.hwx original.wav --sounds dump\audio\xbox`).
4. **Export a 16-bit stereo WAV at the slot's rate**, e.g. with ffmpeg:

   ```
   ffmpeg -i edit.wav -ar 32000 -ac 2 -c:a pcm_s16le edit_32000.wav
   ```

5. **Encode it.** The output name picks the slot. Anything over the slot's length is trimmed and anything
   short is padded with silence, with a 30 ms fade at the loop point (`--fade`):

   ```
   python tools/audio/hwx.py encode edit_32000.wav mods\audio\xbox\mus_ingamebattle28_lp.hwx --sounds dump\audio\xbox
   ```

6. **Check the log.** `logs\swrots.log` shows `Mod override: \audio\xbox\mus_….hwx` when the track starts.

## Giving a level its own track

Because tracks are shared, music meant for one level would also play in others. Instead, the level's cues can
point to a track that no other level uses, and the new music goes into that track. A level file stores each
name with its length, so a cue can only point to a track **whose name has the same length**: the
`mus_ingamebattleNN_lp` names can stand in for one another.

The duels, for example, point their round-1 cue at the unused `mus_ingamebattle02_lp`. Obi-Wan's walkway track
(`28`) is used by no other level, and Anakin's duel is pointed at it too:

```
python tools/audio/hwx.py repoint m14_mus_theduelobi.slp    mods\levels\m14_mus_theduelobi.slp    mus_ingamebattle21_lp mus_ingamebattle02_lp
python tools/audio/hwx.py repoint m15_mus_theduelanakin.slp tmp.slp                               mus_ingamebattle24_lp mus_ingamebattle02_lp
python tools/audio/hwx.py repoint tmp.slp                   mods\levels\m15_mus_theduelanakin.slp mus_ingamebattle17_lp mus_ingamebattle28_lp
```

The edited level goes in `mods\levels\`; the log shows `Mod override: \levels\<level>.slp` when it loads.

## Tools

`tools/audio/hwx.py` (Python 3, standard library only) works from files dumped from your own copy of the game:

- `slots`: every music track with its rate, size and length
- `decode`: a track to WAV
- `encode`: a WAV to a track, fitted to the slot
- `cues`: the tracks a level's cues play
- `repoint`: points a level's cues to another track whose name has the same length

Don't share the encoded tracks or edited level files: the music is the film's soundtrack, and the levels are game
files. Share the edit, for example the cut points, and these steps. Post them in
[Discussions](https://github.com/jedijosh920/SWROTS-PC/discussions) (*Show and tell*).

## Limits

- **Lengths are fixed.** With an edited `ep3.xb_wml` under `mods\audio\xbox\`, the game showed the "disc is
  dirty or damaged" screen as the main menu loaded; why is not known yet. Until the sound table can be
  replaced, music must be cut or looped to the existing lengths.
- **Only same-length names can be swapped** in a level file, and only two tracks are used by no level:
  `mus_ingamebattle02_lp` and `mus_m02b_hallwaybattle01_lp`. A level's cues that name the same track can't
  be given different music: in the duels, the start and the last rounds both play `mus_m14_dotforch_lp`.
- **An edited level file can crash the game.** In our tests (v0.3.1 and v0.4.0, starting straight into
  the level with `map:` in `mods\Default_Xbox.cfg`), the game crashed as it loaded a level file from
  `mods\levels\`, even an unchanged dumped one. Replacing tracks by name, as above, works in every level.
  Giving a level its own track (repointing) needs this to be fixed or better understood.
- Music in the pre-rendered movies is part of the video.

# Adding versus fighters

Yoda was added to the versus select screen as a fighter of his own; any character class on the disc
can be added the same way. This is a code change (one table row, then a build), not a loose-file mod
yet. How it works underneath: [the versus roster](../research/versus-roster.md).

For a quick try without building anything, the [debug console](../debug-menu.md)'s
`duelist <slot> <class>` puts a class in one of the nine existing slots instead, e.g.
`duelist 0 IDooku`.

## What a fighter needs

- **A character class the game knows**, e.g. `IYoda`, `IPalpatine`, `IJediKnight`. `duelist` lists
  the fighters and refuses unknown names; `player <class>` is a quick way to check that a class loads
  and plays in a level at all.
- **A combat moveset.** The class must be able to fight a saber duelist: attacks (its
  `a_<name>.csv`), reactions, and the animations for them. Characters built for other roles (clone
  troopers, droids) load but fight poorly. Reactions the class lacks fall back to the human base
  class's, which play Anakin's animations; where that looks wrong on a differently built body, map
  them to the character's own animations in `src/game/aliases.cpp` (Yoda's blocks are done this way).
- **Pictures** for the select screen, in `interfc\front_end\`: a grid head (64x64 STX) and a bust
  (256x128 STX). Yoda's (`s_selduel_yodahead`, `s_selduel_yodabust`) are on the disc. Pictures a
  character does not have would have to be supplied as loose files under `mods\` (not tried yet).
- **A name and a title** as text IDs from `gameinfo\strings` (Yoda: `IDS_SHELL_YODA`,
  `IDS_SHELL_JEDI_MASTER`). Adding new text is not supported yet, so use IDs that exist.
- **Duel cameras.** The intro and win shots belong to a fighter (`cinematics\introcamera\`); a new
  fighter borrows another's, named in its row.

The character itself (model, textures, animations, tables) is found in whichever level's PAK has
it and loaded only for duels it is in; nothing has to be copied.

## Adding the row

In `src/game/roster.cpp`, `kExtraFighters` has one row per extra fighter:

```cpp
constexpr ExtraFighter kExtraFighters[] = {
    { "IYoda", "s_selduel_yodahead", "s_selduel_yodabust", "IDS_SHELL_YODA", "IDS_SHELL_JEDI_MASTER", "Anakin",
        { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } }, "meshes\\chars\\yoda\\" },
};
```

| Field | Meaning |
|---|---|
| class name | the character class, e.g. `"IYoda"` |
| head, bust | picture names in `interfc\front_end\`, without `.stx` |
| name, title | text IDs |
| cameras | the fighter whose intro and win cameras it uses: `Anakin`, `Obiwan`, `Dooku`, ... as in `cinematics\introcamera\<Name>_Intro_Cam.cin` |
| sabers | per player an RGB saber colour (0-1) used when it fights itself; `0, 0, 0` keeps the character's own. Yoda's player 2 is red |
| meshes | the folder of the character's textures, for player 2's darker look when it fights itself (generated), or `nullptr` for none |

Each row becomes a cell after Random (slots 10, 11, ...), unlocked, with its name, title and bust,
and Random can choose it. Rebuild, open Versus, and try it against itself and against each of the
nine.

## Limits

- Only one extra fighter (Yoda) has been tried; whether the grid's layout has room for more is
  untested.
- No full-body picture on the select screen (the disc has none for an added character); the arena
  screen shows its bust instead. A mod can give Yoda his: `mods\interfc\front_end\s_selduel_fb_yoda_v1.stx`
  (player 1's, 128x256 DXT5, as a custom character's) and `s_selduel_fb_yoda_v2.stx` (player 2's in Yoda against
  Yoda, Sith Yoda); with them the select and arena screens show them, as the game's fighters' full bodies.
- The menus show the normal bust for player 2 in a mirror match, even when the fighter gets a
  darker look in the duel.
- No intro or win animation of its own unless the disc has `animation\<name>\<name>_intro.bnm`.

# Modding SWROTS-PC

The PC port loads game files from loose folders, so mods are plain files placed next to the game.
No PAK rebuilding or disc image editing is needed.

- [Getting started](getting-started.md): the `mods\` folder, modding settings, testing a level directly
- [Dumping assets](dumping-assets.md): extracting the files a level loads, to use as mod sources
- [Replacing textures](replacing-textures.md): STX textures and the embedded-name rule
- [Swapping characters](swapping-characters.md): a character model swap, worked through with Palpatine
  (to just play as another character or body, see the [debug menu](../debug-menu.md#playing-as-another-character))
- [Replacing weapons](replacing-weapons.md): a lightsaber hilt replaced, the story and versus sabers, and the
  weapon tools in `tools/weapons/`
- [Replacing music](replacing-music.md): the music tracks, fitting a replacement to a track's length and rate,
  giving a level its own track, and `tools/audio/hwx.py`
- [Adding versus fighters](adding-versus-fighters.md): giving a character its own versus select-screen slot
- [Custom characters](custom-characters.md): a character class of a mod's own (its body, costume, saber colour,
  name and side; another class's moves), dropped into `mods\characters\`
- [Converting a playermodel](converting-playermodels.md): a Jedi Academy or Movie Duels player model turned into
  a custom character's body — the skeleton targets, the conversion pipeline and the tools in `tools/characters/`
- [Custom maps](custom-maps.md): a map of a mod's own (a model and collision boxes) on the developer map, for
  Heroes vs Villains or walking around, dropped into `mods\maps\`
- [How loading works](how-loading-works.md): level PAKs, load order and what the port does to allow overrides
- [File formats](file-formats.md): what is known about the PAK, STX and MSH formats (weapon and character
  models; `tools/meshes/charmsh.py` reads and writes character models) and the character table

## Sharing mods

- **Share your own work as ready-made files** that players drop into their `mods\` folder: new models,
  hilts, textures and music you made yourself. That is the easiest way for players.
- **Never include the game's own files.** A mod that changes one of the
  game's files is best shared as a script or tool that makes the result from the player's own copy of the
  game (as `tools/weapons/` does for hilts).
- **Where:** a mod site such as the game's [ModDB page](https://www.moddb.com/games/star-wars-episode-iii-revenge-of-the-sith/mods)
  (say that the mod needs SWROTS-PC: the mods there now are for the PS2 version). Post a link, and any guides
  you write, in [Discussions](https://github.com/jedijosh920/SWROTS-PC/discussions) (*Show and tell*).
- Tools and guides are welcome in this repository as pull requests (see [CONTRIBUTING](../../CONTRIBUTING.md)),
  with credit to their authors.

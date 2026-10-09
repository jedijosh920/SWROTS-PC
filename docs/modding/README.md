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
- [How loading works](how-loading-works.md): level PAKs, load order and what the port does to allow overrides
- [File formats](file-formats.md): what is known about the PAK, STX, MSH formats and the character table

Never redistribute the game's own files. Share mods as the changed files, or better as a
script or tool that produces them from the user's own copy of the game. Post them, and guides you
write, in [Discussions](https://github.com/jedijosh920/SWROTS-PC/discussions) (*Show and tell*).

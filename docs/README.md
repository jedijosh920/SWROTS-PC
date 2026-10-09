# Documentation

## Playing

- [Installing](install.md): setup from your disc image, the folder layout, reinstalling
- [Linux, Steam Deck and handhelds](linux.md): playing through Proton
- [Settings](settings.md): `settings.ini`: window, graphics, frame rate, developer options
- [Co-op](coop.md): two players in the story missions: turning it on, controllers, known issues
- [Controls](controls.md): keyboard, mouse and controllers, `controls.ini`
- [Debug menu](debug-menu.md): the game's own console, variables and debug displays, and playing
  as any character (class, costume, texture set, body)

## Modding

- [Modding overview](modding/README.md)
- [Getting started](modding/getting-started.md): the `mods\` folder and useful config overrides
- [Dumping assets](modding/dumping-assets.md): extracting the files a level loads
- [Replacing textures](modding/replacing-textures.md)
- [Swapping characters](modding/swapping-characters.md): a worked example
- [Replacing weapons](modding/replacing-weapons.md): lightsaber hilts, worked through with Anakin's Episode III hilt
- [Adding versus fighters](modding/adding-versus-fighters.md): a select-screen slot of its own, as Yoda has
- [How loading works](modding/how-loading-works.md): level archives and what the port changes
- [File formats](modding/file-formats.md): PAK, STX, MSH and more

## Development

- [How it works](architecture.md): the port's design
- [Contributing](../CONTRIBUTING.md): setup, rules, debugging tools
- [Symbol tools](../tools/symbols/README.md): reconstructing names for the game's functions

## Research

Notes on the game's engine, from its own executable and from related games.

- [Engine variables](research/engine-variables.md): every console variable and command, launch
  arguments, the mission list, what the retail build left out
- [Indiana Jones and the Emperor's Tomb](research/indiana-jones.md): the same engine, with symbol maps
- [The PS2 version](research/ps2-build.md): what its disc contains that the Xbox one does not
- [The versus roster](research/versus-roster.md): how Yoda became a versus fighter, with addresses
- [Characters](research/characters.md): classes, costume lists, texture sets (the 501st), bodies and
  animation bindings, with addresses
- [The camera system](research/camera-system.md): from the master camera to the screen, camera effects,
  the free camera
- [Two players in the story missions](research/coop.md): how the game does co-op, and a second player
  for the campaign (work in progress)

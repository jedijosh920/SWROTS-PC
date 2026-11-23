# Installing

You need your own copy of *Star Wars: Episode III - Revenge of the Sith* for the original Xbox: the
North American release, or the PAL release (France, *La Revanche des Sith*; or Europe, in English, Spanish
and Italian). This project contains no game files.

1. Put `swrots.exe` and `core.dll` in a folder of your choice, e.g. `C:\Games\SWROTS`.
2. Start `swrots.exe`. The first time, it asks for your disc image:
   - an `.iso` (an Xbox disc image: XISO or a full Redump-style image), or
   - a `.7z`, `.zip` or `.rar` archive containing one (unpacked with Windows' built-in `tar`,
     Windows 10 1803 or newer; if that fails, unpack it yourself and choose the `.iso`).
3. The game files are copied into `GameData\` next to `swrots.exe` (about 2.5 GB, plus the same
   again temporarily when unpacking an archive). The game checks that it is a supported release
   (see the [README](../README.md#requirements)).
4. A Start menu shortcut is created, and optionally a desktop shortcut. Both use the game's own
   icon (`swrots.ico`, taken from the game's executable).

The game then starts. From the command line, `swrots.exe --install <image>` skips the file dialog.

To reinstall, delete the `GameData` folder. Saves (`saves\`), settings (`settings.ini`, `controls.ini`)
and mods (`mods\`) are kept. To switch releases (e.g. from the North American disc to the French one),
reinstall from the other disc image; keep the saves of each release apart, in case one release does not
accept the other's.

## Folder layout

| | |
|---|---|
| `swrots.exe`, `core.dll` | The game |
| `GameData\` | Game files from your disc (never modified) |
| `saves\` | Save games |
| `mods\` | Mods ([modding guide](modding/README.md)) |
| `cache\` | Temporary files |
| `settings.ini` | [Settings](settings.md) |
| `controls.ini` | [Controls](controls.md) |
| `logs\` | `swrots.log` (this run; include it when reporting a problem), `swrots.previous.log` (the run before, e.g. one that crashed), `Message.log` (the game's own messages) |

## Troubleshooting

- **Lightsaber blades away from their hilts** (seen on Intel Iris Xe integrated graphics): Intel's graphics
  driver draws them in the wrong place. On a laptop that also has an NVIDIA or AMD graphics card, let the game
  use it: Windows Settings → System → Display → Graphics → add `swrots.exe` → **High performance**. Otherwise,
  update the Intel graphics driver.
- **On Linux, a Steam Deck or a handheld**: see [Linux, Steam Deck and handhelds](linux.md).
- **Anything else**: [report it](https://github.com/jedijosh920/SWROTS-PC/issues/new/choose) with
  `logs\swrots.log`; it records your system, graphics card and frame rate.

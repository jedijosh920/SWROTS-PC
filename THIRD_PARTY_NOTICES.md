# Third-party notices

SWROTS-PC is licensed under the GNU General Public License v3.0 or later ([LICENSE](LICENSE)). It
contains or is derived from the following third-party work.

## Cxbx-Reloaded

<https://github.com/Cxbx-Reloaded/Cxbx-Reloaded> -- GNU General Public License v2.0 or later.
Copyright (c) the Cxbx and Cxbx-Reloaded authors.

Adapted in this project (each file says so in its header):

- `src/d3d/hlsl.cpp` -- from Cxbx-Reloaded's vertex and pixel shader HLSL templates
- `src/d3d/pshader.cpp` -- register combiner (pixel shader) decoding, following `XbPixelShader.cpp` / `PixelShader.cpp`
- `src/d3d/vshader.cpp` -- vertex program decoding and HLSL generation, following `XbVertexShader.cpp` / `VertexShader.cpp`
- `src/d3d/xd3d.h` -- Xbox Direct3D type layouts, from `XbD3D8Types.h`

`src/game/sdk_symbols.inc` (addresses of the Xbox library functions in the game's executable) was
generated with the help of Cxbx-Reloaded's symbol scanner results for this title.

These files are used under the "or any later version" option of their licence, as part of a work
licensed under the GPL v3.0 or later.

## Dear ImGui

<https://github.com/ocornut/imgui> -- MIT License, Copyright (c) 2014-2025 Omar Cornut.
Included in `third_party/imgui/` (version 1.91.9b) with its licence text in
[third_party/imgui/LICENSE.txt](third_party/imgui/LICENSE.txt).

## Lua

<https://www.lua.org> -- MIT License, Copyright (C) 1994-2023 Lua.org, PUC-Rio.
Included in `third_party/lua/` (version 5.4.6, the library only: without `lua.c` and `luac.c`) with its licence text
in [third_party/lua/LICENSE.txt](third_party/lua/LICENSE.txt). It runs the scripts of missions and modes.

## Microsoft Windows components

The port uses Direct3D 9, the Direct3D shader compiler, XAudio2, XInput and other components that
are part of Windows. They are not included in this repository or its releases.

## The game

*Star Wars: Episode III - Revenge of the Sith* is (c) Lucasfilm Entertainment Company Ltd. and was
developed by The Collective and published by LucasArts. No part of the game is included in this
project. Star Wars and related names are trademarks of Lucasfilm Ltd. This project is not affiliated
with or endorsed by them.
